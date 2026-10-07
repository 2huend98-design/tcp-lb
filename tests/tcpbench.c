// tcpbench.c — 纯吞吐探针: 裸字节流, 零协议、零校验
// 判定:
//   - 主判据 = 数据完整性(sent==target && recv==target) —— 交易成功
//   - 收尾 RST 单列为信号, 不阻塞 PASS
//   - 短量(数据不完整) 一票否决
//
// 编译: gcc -O2 -Wall -o tcpbench tcpbench.c
// 用法: ./tcpbench <port> [--conns N] [--size_mb M|--size_kb K] [--rounds R]
//                    [--timeout S] [--procs P]
//
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#define TXBLK  (512*1024)
#define RXBUF  (256*1024)

static uint64_t g_target;
static int      g_timeout_s = 120;
static double   g_interval  = 0;    /* >0: 每隔 S 秒打印聚合接收速率时间序列 (诊断双峰) */
static int      g_widx      = 0;    /* 多进程时子进程编号, 时间序列行前缀 */
static uint8_t  txblk[TXBLK], rxbuf[RXBUF];

/* 失败分类 */
enum {
    EK_NONE = 0,
    EK_RST, EK_EPIPE, EK_SHORT, EK_EARLY_EOF, EK_TIMEOUT, EK_CONNECT, EK_OTHER
};
static const char* kind_name(int k){
    switch(k){
        case EK_RST:       return "RST";
        case EK_EPIPE:     return "EPIPE";
        case EK_SHORT:     return "SHORT";
        case EK_EARLY_EOF: return "EARLY-EOF";
        case EK_TIMEOUT:   return "TIMEOUT";
        case EK_CONNECT:   return "CONNECT";
        case EK_OTHER:     return "OTHER";
        default:           return "?";
    }
}
static int classify_errno(int e){
    if (e == ECONNRESET) return EK_RST;
    if (e == EPIPE)      return EK_EPIPE;
    return EK_OTHER;
}

typedef struct {
    int fd, phase, wdone, eof, failed, closed, ev;
    uint64_t remaining, sent, received;
    uint16_t local_port;
    int      err_kind;
    double   fail_time;
    int      connected_flag;
    int      recv_done_flag;
    char err[112];
} conn_t;

static void fail(conn_t *c, int kind, const char *fmt, ...) {
    if (c->failed) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    c->failed = 1;
    c->err_kind = kind;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    c->fail_time = t.tv_sec + t.tv_nsec*1e-9;
}
static double now_s(void){ struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

/* 时刻格式化 —— -1.0 表示"未定义", 显示 n/a */
static void fmt_seg(char *buf, size_t n, double t) {
    if (t < 0.0) snprintf(buf, n, "n/a");
    else         snprintf(buf, n, "%.2f", t);
}

static void pump_out(conn_t *c){
    while (c->remaining) {
        size_t off=c->sent%TXBLK, n=c->remaining;
        if (n>TXBLK-off) n=TXBLK-off;
        ssize_t k=send(c->fd,txblk+off,n,MSG_NOSIGNAL);
        if (k>0){ c->sent+=(uint64_t)k; c->remaining-=(uint64_t)k; continue; }
        if (k<0&&errno==EINTR) continue;
        if (k<0&&(errno==EAGAIN||errno==EWOULDBLOCK)) return;
        fail(c, classify_errno(errno), "send: %s", strerror(errno)); return;
    }
    if (!c->wdone){
        if (shutdown(c->fd,SHUT_WR)==0) c->wdone=1;
        else fail(c, classify_errno(errno), "shutdown: %s", strerror(errno));
    }
}
static void drain_in(conn_t *c){
    for (;;) {
        ssize_t k=recv(c->fd,rxbuf,RXBUF,0);
        if (k>0){ c->received+=(uint64_t)k; continue; }
        if (k==0){ c->eof=1;
            if(!c->wdone) fail(c, EK_EARLY_EOF, "提前EOF: 发%llu/%lluB",
                (unsigned long long)c->sent,(unsigned long long)g_target);
            return; }
        if (errno==EINTR) continue;
        if (errno==EAGAIN||errno==EWOULDBLOCK) return;
        fail(c, classify_errno(errno), "recv: %s", strerror(errno)); return;
    }
}
static void kill_conn(int epfd,conn_t*c){
    if (c->closed) return;
    epoll_ctl(epfd,EPOLL_CTL_DEL,c->fd,NULL);
    close(c->fd); c->fd=-1; c->closed=1;
}
static void mask_update(int epfd,conn_t*c){
    uint32_t e=(c->phase==0)?EPOLLOUT:EPOLLIN;
    if (c->phase==1&&c->remaining) e|=EPOLLOUT;
    if (e!=(uint32_t)c->ev){
        struct epoll_event x={.events=e,.data.ptr=c};
        epoll_ctl(epfd,EPOLL_CTL_MOD,c->fd,&x); c->ev=(int)e;
    }
}

typedef struct{
    double time_s;
    double t_connect;      // v5: 建连阶段结束时刻(相对 t0); -1.0 = 未达到
    double t_recv_done;    // v5: 传输阶段结束时刻; -1.0 = 未达到
    int pass, clean, fail_short, fail_rst;
    uint64_t rx, tx;
}stat_t;

static stat_t run_round(int port,int conns,int timeout_s){
    stat_t st; memset(&st,0,sizeof st);
    st.t_connect = -1.0;
    st.t_recv_done = -1.0;
    conn_t *cs=calloc((size_t)conns,sizeof(conn_t));
    struct epoll_event *evs=calloc(1024,sizeof(struct epoll_event));
    int epfd=epoll_create1(0);
    struct sockaddr_in sa; memset(&sa,0,sizeof sa);
    sa.sin_family=AF_INET; sa.sin_port=htons((uint16_t)port);
    sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int nopen=0;
    for (int i=0;i<conns;i++){
        conn_t *c=&cs[i]; c->remaining=g_target;
        c->fd=socket(AF_INET,SOCK_STREAM,0);
        if (c->fd<0){ fail(c, EK_OTHER, "socket: %s", strerror(errno)); c->closed=1; continue; }
        int one=1;
        setsockopt(c->fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
        fcntl(c->fd,F_SETFL,fcntl(c->fd,F_GETFL,0)|O_NONBLOCK);
        if (connect(c->fd,(struct sockaddr*)&sa,sizeof sa)<0 && errno!=EINPROGRESS){
            fail(c, EK_CONNECT, "connect: %s", strerror(errno));
            close(c->fd); c->fd=-1; c->closed=1; continue;
        }
        struct epoll_event e={.events=EPOLLOUT,.data.ptr=c};
        epoll_ctl(epfd,EPOLL_CTL_ADD,c->fd,&e); c->ev=EPOLLOUT; nopen++;
    }
    double t0=now_s(), deadline=t0+timeout_s;
    double last_t=t0; uint64_t last_rx=0;
    int n_connected = 0;
    int n_recv_done = 0;
    double t_connect_abs = 0;
    double t_recv_done_abs = 0;
    while (nopen>0 && now_s()<deadline){
        int n=epoll_wait(epfd,evs,1024,200);
        if (n<0){ if (errno==EINTR) continue; break; }
        for (int q=0;q<n;q++){
            conn_t *c=evs[q].data.ptr;
            uint32_t e=evs[q].events;
            if (c->closed) continue;
            if (c->phase==0){
                int err=0; socklen_t el=sizeof err;
                if ((e&(EPOLLERR|EPOLLHUP))||
                    getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&err,&el)<0||err) {
                    fail(c, EK_CONNECT, "connect失败: %s", strerror(err?err:EIO));
                } else {
                    c->phase=1;
                    struct sockaddr_in local; socklen_t ll=sizeof local;
                    if (getsockname(c->fd,(struct sockaddr*)&local,&ll)==0)
                        c->local_port = ntohs(local.sin_port);
                    if (!c->connected_flag) {
                        c->connected_flag = 1;
                        n_connected++;
                        if (t_connect_abs == 0 && n_connected == conns)
                            t_connect_abs = now_s();
                    }
                }
            } else {
                if (e&(EPOLLIN|EPOLLHUP|EPOLLERR)) drain_in(c);
                if (!c->failed&&(e&EPOLLOUT)) pump_out(c);
            }
            if (c->phase == 1 && !c->recv_done_flag && c->received == g_target) {
                c->recv_done_flag = 1;
                n_recv_done++;
                if (t_recv_done_abs == 0 && n_recv_done == conns)
                    t_recv_done_abs = now_s();
            }
            if (c->failed||(c->wdone&&c->eof)){ kill_conn(epfd,c); nopen--; }
            else if (c->fd>=0) mask_update(epfd,c);
        }
        /* 聚合接收速率时间序列 —— 让快/慢双峰模态的切换时刻与形状可见 */
        if (g_interval>0){
            double now=now_s();
            if (now-last_t>=g_interval){
                uint64_t agg=0; for(int i=0;i<conns;i++) agg+=cs[i].received;
                double rate=(now>last_t)?(double)(agg-last_rx)/(now-last_t)/1048576.0:0.0;
                fprintf(stderr,"[w%d t=%6.2fs] rx=%5lluMiB rate=%5.0fMiB/s open=%d\n",
                        g_widx, now-t0, (unsigned long long)(agg>>20), rate, nopen);
                last_rx=agg; last_t=now;
            }
        }
    }
    if (nopen>0)
        for (int i=0;i<conns;i++)
            if (!cs[i].closed){ fail(&cs[i], EK_TIMEOUT, "超时"); kill_conn(epfd,&cs[i]); }
    st.time_s=now_s()-t0;
    /* 时刻未达到时 -1.0, 不回退到 time_s —— 回退会污染聚合 */
    st.t_connect   = (t_connect_abs   > 0) ? (t_connect_abs   - t0) : -1.0;
    st.t_recv_done = (t_recv_done_abs > 0) ? (t_recv_done_abs - t0) : -1.0;
    for (int i=0;i<conns;i++){
        conn_t *c=&cs[i];
        st.rx+=c->received; st.tx+=c->sent;
        int data_ok = (c->sent == g_target && c->received == g_target);
        int clean   = data_ok && c->eof;
        if (data_ok) {
            st.pass++;
            if (clean) st.clean++;
            else       st.fail_rst++;
        } else {
            st.fail_short++;
        }
        if (c->failed && st.fail_short + st.fail_rst <= 12) {
            fprintf(stderr, "    [BENCH-FAIL] conn#%d :%s local_port=%u t=%.3fs sent=%llu recv=%llu : %s\n",
                    i, kind_name(c->err_kind), (unsigned)c->local_port,
                    c->fail_time - t0,
                    (unsigned long long)c->sent, (unsigned long long)c->received,
                    c->err);
        }
    }
    free(cs); free(evs); close(epfd);
    return st;
}

/* ---- 多进程 ---- */
#define TB_MAX_PROCS 64
typedef struct{int port,timeout_s,rounds,n_conn;uint64_t target;}wc_t;
static wc_t g_wc; static int g_ctl_fd=-1;
static pid_t g_pids[TB_MAX_PROCS]; static int g_procs;

static int xsnd(int f,const void*b,size_t n){const char*p=b;
    while(n){ssize_t k=send(f,p,n,0);
        if(k<0){if(errno==EINTR)continue;return -1;} p+=k;n-=(size_t)k;}return 0;}
static int xrcv(int f,void*b,size_t n){char*p=b;
    while(n){ssize_t k=recv(f,p,n,0);
        if(k<=0){if(k<0&&errno==EINTR)continue;return -1;} p+=k;n-=(size_t)k;}return 0;}

static void die_all(int s){ (void)s;
    static const char WMSG[]="\n[tcpbench] 总看门狗超时\n";
    for(int i=0;i<g_procs;i++) if(g_pids[i]>0) kill(g_pids[i],SIGKILL);
    ssize_t w=write(STDERR_FILENO,WMSG,sizeof WMSG-1); (void)w;
    _exit(132);
}
static void child_main(void){
    signal(SIGPIPE,SIG_IGN);
    struct timeval tv={.tv_sec=g_wc.timeout_s+60,.tv_usec=0};
    setsockopt(g_ctl_fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    for (int r=0;r<g_wc.rounds;r++){
        char go;
        if (xrcv(g_ctl_fd,&go,1)) _exit(3);
        stat_t st=run_round(g_wc.port,g_wc.n_conn,g_wc.timeout_s);
        if (xsnd(g_ctl_fd,&st,sizeof st)) _exit(4);
    }
    exit(0);
}
static int run_multi(int port,int conns,uint64_t target,int rounds,int ts,int procs){
    int per=conns/procs,rem=conns%procs,ok=1;
    g_procs=procs;
    int (*ctl)[2]=calloc((size_t)procs,sizeof *ctl);
    if(!ctl){fprintf(stderr,"OOM\n");return 0;}
    g_wc.port=port; g_wc.rounds=rounds; g_wc.timeout_s=ts; g_wc.target=target;
    fflush(stdout); fflush(stderr);
    int born=0;
    for (int w=0;w<procs;w++){
        g_wc.n_conn=per+(w<rem);
        if (socketpair(AF_UNIX,SOCK_STREAM,0,ctl[w])){ perror("socketpair"); break; }
        pid_t pid=fork();
        if (pid<0){ perror("fork"); break; }
        if (pid==0){ close(ctl[w][0]); g_ctl_fd=ctl[w][1]; g_widx=w; child_main(); }
        g_pids[born++]=pid; close(ctl[w][1]);
    }
    signal(SIGALRM,die_all);
    alarm((unsigned)((long)rounds*(ts+5))+300);
    if (born<procs){
        fprintf(stderr,"仅 %d/%d worker 启动\n",born,procs); ok=0;
    } else for (int r=0;r<rounds;r++){
        stat_t agg; memset(&agg,0,sizeof agg);
        agg.t_connect = -1.0;
        agg.t_recv_done = -1.0;
        double max_time_s = -1.0;
        char go='G';
        for (int w=0;w<procs;w++) if (xsnd(ctl[w][0],&go,1)) ok=0;
        for (int w=0;w<procs;w++){
            stat_t s;
            if (xrcv(ctl[w][0],&s,sizeof s)){
                fprintf(stderr,"worker%d 失联\n",w);
                memset(&s,0,sizeof s);
                s.fail_short = per+(w<rem);
                s.t_connect = -1.0;
                s.t_recv_done = -1.0;
                ok=0;
            }
            agg.pass      += s.pass;
            agg.clean     += s.clean;
            agg.fail_short+= s.fail_short;
            agg.fail_rst  += s.fail_rst;
            agg.rx        += s.rx;
            agg.tx        += s.tx;
            /* 主导 worker (time_s 最大者) 的完整时刻线 */
            if (s.time_s > max_time_s) {
                max_time_s      = s.time_s;
                agg.time_s      = s.time_s;
                agg.t_connect   = s.t_connect;
                agg.t_recv_done = s.t_recv_done;
            }
        }
        if (agg.fail_short) ok=0;

        double sc  = agg.t_connect;
        double sx  = agg.t_recv_done;
        double scl = (sx >= 0.0) ? (agg.time_s - sx) : -1.0;
        double xd  = (sx >= 0.0 && sc >= 0.0) ? (sx - sc) : -1.0;
        if (xd < 0.0) xd = 0.0;
        char bc[16], bx[16], bcl[16];
        fmt_seg(bc,  sizeof bc,  sc);
        fmt_seg(bx,  sizeof bx,  xd);
        fmt_seg(bcl, sizeof bcl, scl);

        printf("[round %d/%d] %d/%d DATA-PASS (clean=%d rst=%d) time=%.2fs "
               "seg connect=%s xfer=%s close=%s throughput=%.0fMiB/s\n",
               r+1,rounds,agg.pass,conns,agg.clean,agg.fail_rst,
               agg.time_s, bc, bx, bcl,
               agg.rx/1048576.0/(agg.time_s>0?agg.time_s:1e-9));
        if (agg.fail_rst)
            printf("    [note] round %d: %d conns data-complete but RST at close (signal, not failure)\n",
                   r+1,agg.fail_rst);
    }
    alarm(0);
    for (int w=0;w<born;w++){ kill(g_pids[w],SIGKILL); waitpid(g_pids[w],NULL,0); }
    for (int w=0;w<born;w++) close(ctl[w][0]);
    free(ctl);
    return ok;
}

static long long arg_num(const char *tag,const char *s){
    char *e; long long v=strtoll(s,&e,10);
    if (e==s||*e!='\0'||v<=0||v>100000000){
        fprintf(stderr,"非法数值(%s): %s\n",tag,s); exit(2);
    }
    return v;
}

int main(int argc,char**argv){
    if (argc<2){
        fprintf(stderr,"用法: %s <port> [--conns N] [--size_mb M|--size_kb K] "
                "[--rounds R] [--timeout S] [--procs P] [--interval S]\n",argv[0]);
        return 2;
    }
    signal(SIGPIPE,SIG_IGN);
    int port=(int)arg_num("<port>",argv[1]);
    int conns=1,rounds=1,kb=0,procs=0; double mb=0;
    for (int i=2;i<argc;i++){
        if      (!strcmp(argv[i],"--conns")   &&i+1<argc) conns=(int)arg_num("--conns",argv[++i]);
        else if (!strcmp(argv[i],"--size_mb") &&i+1<argc) mb=(double)arg_num("--size_mb",argv[++i]);
        else if (!strcmp(argv[i],"--size_kb") &&i+1<argc) kb=(int)arg_num("--size_kb",argv[++i]);
        else if (!strcmp(argv[i],"--rounds")  &&i+1<argc) rounds=(int)arg_num("--rounds",argv[++i]);
        else if (!strcmp(argv[i],"--timeout") &&i+1<argc) g_timeout_s=(int)arg_num("--timeout",argv[++i]);
        else if (!strcmp(argv[i],"--procs")   &&i+1<argc) procs=(int)arg_num("--procs",argv[++i]);
        else if (!strcmp(argv[i],"--interval")&&i+1<argc) g_interval=atof(argv[++i]);
        else { fprintf(stderr,"未知参数: %s\n",argv[i]); return 2; }
    }
    if (mb<=0&&kb<=0) kb=256;
    g_target = mb>0 ? (uint64_t)(mb*1048576.0) : (uint64_t)kb*1024u;
    for (size_t o=0;o<TXBLK;o+=4096) memset(txblk+o,(int)(o/4096),4096);
    long nc=sysconf(_SC_NPROCESSORS_ONLN);
    if (procs<=0) procs=(nc>0)?(int)nc:1;
    if (procs>conns) procs=conns;
    if (procs>TB_MAX_PROCS) procs=TB_MAX_PROCS;
    if (procs<1) procs=1;
    printf("port=%d raw conns=%d procs=%d per_conn=%lluB rounds=%d\n",
           port,conns,procs,(unsigned long long)g_target,rounds);
    int ok=1;
    if (procs==1){
        for (int r=0;r<rounds;r++){
            stat_t st=run_round(port,conns,g_timeout_s);
            if (st.fail_short) ok=0;

            double sc  = st.t_connect;
            double sx  = st.t_recv_done;
            double scl = (sx >= 0.0) ? (st.time_s - sx) : -1.0;
            double xd  = (sx >= 0.0 && sc >= 0.0) ? (sx - sc) : -1.0;
            if (xd < 0.0) xd = 0.0;
            char bc[16], bx[16], bcl[16];
            fmt_seg(bc,  sizeof bc,  sc);
            fmt_seg(bx,  sizeof bx,  xd);
            fmt_seg(bcl, sizeof bcl, scl);

            printf("[round %d/%d] %d/%d DATA-PASS (clean=%d rst=%d) time=%.2fs "
                   "seg connect=%s xfer=%s close=%s throughput=%.0fMiB/s\n",
                   r+1,rounds,st.pass,conns,st.clean,st.fail_rst,
                   st.time_s, bc, bx, bcl,
                   st.rx/1048576.0/(st.time_s>0?st.time_s:1e-9));
            if (st.fail_rst)
                printf("    [note] round %d: %d conns data-complete but RST at close (signal, not failure)\n",
                       r+1,st.fail_rst);
        }
    } else ok=run_multi(port,conns,g_target,rounds,g_timeout_s,procs);
    printf("== %s ==\n",ok?"ALL PASS":"存在失败");
    return ok?0:1;
}