#!/usr/bin/env bash
# run_tests.sh — 集成回归（13 场景）
#
# 场景表（名称 —— 目的 —— 通过判据）:
#  1 冒烟       单连接回显: 字节进=字节出                  bench 全过 + 会话 5s 内回收
#  2 完整性     内容逐字节正确(不只字节数)                  8 档全过(64K/512K 回绕边界+半关闭)
#  3 关闭语义   客户端收 FIN 而非 RST                      FIN≥95% 且 RST≤5% (§6.4 常驻回归门)
#  4 并发       64 连接互不串扰                            bench 全过
#  5 重载(选)   高并发浸润: 退役路径/fd 压力                数据完整 + 会话回收 + fd 回落
#  6 RST 风暴   对端传输中被强杀: 不泄漏不挂死              alive=0 + fd 回落 + 事后回显全过
#  7 后端故障   摘除 / 传输中死亡 / 恢复回池                三阶段 bench 全过 + 回收
#  8 建连黑洞   不可达后端: 有界拒绝                       6s 内关闭 + 无 fd 泄漏
#  9 无可用后端 全池摘除: 快速拒绝                         2s 内拒绝 + 打点出现
# 10 停滞计时器     只收死连接: 静默/停摆被收, 活跃不误杀       6s 收走 + 15s 存活 + 0 误杀
# 11 冻结收尾   对端冻结: 停滞计时器有界收尾无泄漏             两触发(挂起/被杀)均 alive=0 + fd 回落
# 12 背压有界   后端只收不读: 内存有界+客户端感背压         RSS 增长<256MB + 背压可感 + 解除后回收
# 13 关停       SIGTERM 必退且总是有界                      干净: ≤10s 退出且退出码 0; 滞留: 第 2 次必硬退
#
# 用法:
#   bash tests/run_tests.sh                    # 快速套(12 场景, ~2 分计时器)
#   HEAVY=1 bash tests/run_tests.sh           # 含场景 5 重载浸润(默认 1024×64MB×2 = 128GB)
#   HEAVY=1 HEAVY_CONNS=128 HEAVY_MB=8 bash tests/run_tests.sh   # 缩减形状验证重载路径
#   ASAN=1 BIN=build/bin-asan/tcp_proxy bash tests/run_tests.sh # ASAN 套(重载不跑)
#
# env 面:
#   RCV_ENV/SND_ENV   主 proxy SO_RCVBUF/SO_SNDBUF 注入值。
#                     注入目的=覆盖 setsockopt 代码路径;
#                     其余场景实例不注入(= autotune 默认路径, 两条路径都有回归覆盖)。
#   TCP_IDLE_DEADLINE_SEC  期限/冻结场景的独立实例显式传 6(与代码默认 300s 解耦, 测试可等)。
#   测试配置(worker_threads=4 / output_buffer_kb=512 / 4 后端)是覆盖选择, 与性能标定解耦,
#   不随标定变——小改动请走 changelog。
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
BIN=${BIN:-$ROOT/build/bin}
LOG=$ROOT/build/test_logs

ASAN_MODE=0
case "$BIN" in *asan*) ASAN_MODE=1;; esac
[ "${ASAN:-0}" = "1" ] && ASAN_MODE=1
if [ "$ASAN_MODE" = "1" ]; then
  export ASAN_OPTIONS="log_path=$LOG/asan:abort_on_error=1:detect_leaks=0:detect_stack_use_after_return=1"
fi
mkdir -p "$LOG"; rm -f "$LOG"/*.log "$LOG"/asan.* 2>/dev/null
[ "$ASAN_MODE" = "1" ] && echo "== ASAN 模式 BIN=$BIN (报告: $LOG/asan.<pid>) =="

HEAVY=${HEAVY:-0}
HEAVY_CONNS=${HEAVY_CONNS:-1024}   # [旧标定] 官方浸润形状
HEAVY_MB=${HEAVY_MB:-64}           # [旧标定]
RCV_ENV=${RCV_ENV:-131072}         # [旧标定]
SND_ENV=${SND_ENV:-65536}          # [旧标定]

STBUF=(stdbuf -oL -eL)
[ "$ASAN_MODE" = "1" ] && STBUF=()
ulimit -n 65535 2>/dev/null || true

PASS=0; FAIL=0
gate() { if [ "$2" = "0" ]; then echo "  [PASS] $1"; PASS=$((PASS+1));
         else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi; }
sc()   { echo; echo "==== [$1] $2 ===="; echo "  目的: $3"; }

cleanup() { pkill -f 'bin/echo_server' 2>/dev/null; pkill -f 'bin/tcp_proxy' 2>/dev/null; sleep 0.3; }
trap cleanup EXIT
cleanup

gcc -O2 -Wall -o "$BIN/tcpbench" tests/tcpbench.c || { echo "bench build failed"; exit 1; }

wait_port() {
  for _ in $(seq "$3"); do
    if (exec 3<>"/dev/tcp/$1/$2") 2>/dev/null; then exec 3>&- 3<&-; return 0; fi
    sleep 0.1
  done; return 1; }

fd_of()   { ls "/proc/$1/fd" 2>/dev/null | wc -l; }
rss_kb()  { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null || echo 0; }
stats_of(){ kill -USR1 "$1" 2>/dev/null; sleep 0.3; grep -a '\[stats\]' "$2" | tail -1; }

wait_alive0() {  # $1=pid $2=log $3=tries(×0.5s) → 输出耗时秒; rc1=timeout
  local pid=$1 log=$2 tries=$3 s t0 t1
  t0=$(date +%s.%N)
  for _ in $(seq "$tries"); do
    sleep 0.5
    s=$(stats_of "$pid" "$log")
    case "$s" in *alive=0*)
      t1=$(date +%s.%N)
      awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}'; return 0;;
    esac
  done
  echo "timeout"; return 1
}

# 单后端独立实例配置: $1=listen_port $2=backend_port
one_cfg() {
  cat > "$LOG/cfg_$1.json" <<EOJ
{"listen_port": $1, "backends": [{"address": "127.0.0.1", "port": $2}],
 "balancer": "round_robin", "health_check_interval_sec": 3, "health_check_timeout_sec": 1}
EOJ
}

# ---- 共用: 4 echo 后端(9000-9003) + 主代理(8080) ----
echo "==== 启动 4 echo 后端(9000-9003) + 主代理(8080) ===="
for p in 9000 9001 9002 9003; do
  "$BIN/echo_server" "$p" > "$LOG/echo_$p.log" 2>&1 &
done
cat > "$LOG/test_config.json" <<'EOJ'
{ "listen_port": 8080, "backends": [
    {"address": "127.0.0.1", "port": 9000},
    {"address": "127.0.0.1", "port": 9001},
    {"address": "127.0.0.1", "port": 9002},
    {"address": "127.0.0.1", "port": 9003}
  ], "balancer": "round_robin",
  "health_check_interval_sec": 3, "health_check_timeout_sec": 1,
  "worker_threads": 4, "output_buffer_kb": 512 }
EOJ
TCP_RCVBUF_BYTES=$RCV_ENV TCP_SNDBUF_BYTES=$SND_ENV \
  "${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/test_config.json" > "$LOG/proxy.log" 2>&1 &
PROXY_PID=$!
wait_port 127.0.0.1 8080 50 || { echo "proxy 没起来"; exit 1; }
sleep 0.5
BASE_FD=$(fd_of "$PROXY_PID")
echo "proxy pid=$PROXY_PID fd基线=$BASE_FD  (RCV=$RCV_ENV SND=$SND_ENV)"

# ================= 1 冒烟 =================
sc "1" "冒烟·单连接 1MB" "基本回显: 字节进=字节出"
"$BIN/tcpbench" 8080 --conns 1 --size_mb 1 --timeout 15 > "$LOG/t0.log" 2>&1
gate "冒烟·bench 全过" $?
T0_DT=$(wait_alive0 "$PROXY_PID" "$LOG/proxy.log" 10); T0_RC=$?
gate "冒烟·会话有界回收(${T0_DT}s ≤5s)" "$T0_RC"

# ================= 2 完整性 =================
sc "2" "完整性·8 档尺寸逐字节比对" "内容逐字节正确(不只字节数); 64K/512K 环形缓冲回绕边界; 半关闭闭环"
python3 tests/integrity_test.py 8080 > "$LOG/t0b.log" 2>&1
gate "完整性·8/8 尺寸逐字节一致" $?
tail -1 "$LOG/t0b.log" | sed 's/^/  | /'

# ================= 3 关闭语义 =================
sc "3" "关闭语义·客户端视角 FIN/RST 计数" "回显收完后客户端应收到代理的 FIN 而非 RST(§6.4 收尾 RST 案的常驻回归门)"
python3 - <<'PYEOF' > "$LOG/t0c.log" 2>&1
import socket, sys
N = 100
fin, rst, to = 0, 0, 0
for i in range(N):
    try:
        s = socket.create_connection(("127.0.0.1", 8080), timeout=10)
        s.settimeout(10)
        payload = b"a" * 8192
        s.sendall(payload)
        got = 0
        ok = True
        while got < len(payload):
            try:
                d = s.recv(65536)
            except ConnectionResetError:
                rst += 1; ok = False; break
            except socket.timeout:
                to += 1; ok = False; break
            if d == b"":
                to += 1; ok = False; break
            got += len(d)
        if not ok: continue
        s.shutdown(socket.SHUT_WR)
        try:
            d = s.recv(1)
            if d == b"": fin += 1
            else: to += 1
        except ConnectionResetError:
            rst += 1
        except socket.timeout:
            to += 1
        s.close()
    except Exception:
        to += 1
print(f"FIN={fin} RST={rst} TIMEOUT={to} N={N}")
ok = fin >= N * 95 // 100
sys.exit(0 if ok else 1)
PYEOF
gate "关闭语义·FIN 占比≥95%(收干净 FIN 而非 RST)" $?
tail -1 "$LOG/t0c.log" | sed 's/^/  | /'

# ================= 4 并发 =================
sc "4" "并发·64 连接 × 8MB × 2 轮" "多连接互不串扰"
"$BIN/tcpbench" 8080 --conns 64 --size_mb 8 --rounds 2 --timeout 60 > "$LOG/t1.log" 2>&1
gate "并发·bench 全过" $?

# ================= 5 重载(选) =================
sc "5" "重载·${HEAVY_CONNS} 连接 × ${HEAVY_MB}MB × 2 轮浸润" "高并发下的退役路径与 fd 压力(官方形状 1024×64MB×2=128GB)"
if [ "$HEAVY" != "1" ]; then
  echo "  [SKIP] 未启用(HEAVY=1 开启; ASAN 下无意义)"
elif [ "$ASAN_MODE" = "1" ]; then
  echo "  [SKIP] ASAN 模式下跳过重载(吞吐无意义)"
else
  "$BIN/tcpbench" 8080 --conns "$HEAVY_CONNS" --size_mb "$HEAVY_MB" --rounds 2 --timeout 1800 > "$LOG/t2.log" 2>&1
  gate "重载·bench 数据完整(主判据)" $?
  grep -a 'round\|==\|BENCH-FAIL\|\[note\]' "$LOG/t2.log" | sed 's/^/  | /'
  T2_DT=$(wait_alive0 "$PROXY_PID" "$LOG/proxy.log" 12); T2_RC=$?
  gate "重载·会话有界回收(alive=0, ${T2_DT}s)" "$T2_RC"
  FD_NOW=$(fd_of "$PROXY_PID")
  [ "$FD_NOW" -le $((BASE_FD + 3)) ]; gate "重载·fd 回落($FD_NOW<=基线$BASE_FD+3)" $?
fi

# ================= 6 RST 风暴 =================
sc "6" "RST 风暴·传输中 3 次强杀客户端" "对端被 SIGKILL: 会话不泄漏、代理不挂死"
for i in 1 2 3; do
  "$BIN/tcpbench" 8080 --conns 32 --size_mb 16 --timeout 30 > "$LOG/t3_$i.log" 2>&1 &
  BPID=$!
  sleep "1.$((2 + i * 4))"
  kill -9 "$BPID" 2>/dev/null
  wait "$BPID" 2>/dev/null
done
T3_DT=$(wait_alive0 "$PROXY_PID" "$LOG/proxy.log" 16); T3_RC=$?
gate "RST风暴·会话有界回收(alive=0, ${T3_DT}s)" "$T3_RC"
FD_NOW=$(fd_of "$PROXY_PID")
[ "$FD_NOW" -le $((BASE_FD + 3)) ]; gate "RST风暴·fd 回落($FD_NOW<=基线$BASE_FD+3)" $?
"$BIN/tcpbench" 8080 --conns 16 --size_mb 2 --timeout 30 > "$LOG/t3_after.log" 2>&1
gate "RST风暴·事后回显仍全过" $?

# ================= 7 后端故障 =================
sc "7" "后端故障·摘除 → 传输中死亡 → 恢复" "健康检查摘除死后端, 恢复后回池"
pkill -f 'echo_server 9003'; sleep 5
"$BIN/tcpbench" 8080 --conns 16 --size_mb 2 --timeout 30 > "$LOG/t4a.log" 2>&1
gate "后端故障·摘除后 3 后端 bench 全过" $?
"$BIN/tcpbench" 8080 --conns 8 --size_mb 8 --timeout 30 > "$LOG/t4b.log" 2>&1 &
BPID=$!
sleep 1.0
pkill -f 'echo_server 9002'
wait "$BPID" 2>/dev/null
T4_DT=$(wait_alive0 "$PROXY_PID" "$LOG/proxy.log" 16); T4_RC=$?
gate "后端故障·传输中杀死后会话有界回收(alive=0, ${T4_DT}s)" "$T4_RC"
FD_NOW=$(fd_of "$PROXY_PID")
[ "$FD_NOW" -le $((BASE_FD + 3)) ]; gate "后端故障·fd 回落($FD_NOW)" $?
"$BIN/echo_server" 9002 > "$LOG/echo_9002b.log" 2>&1 &
"$BIN/echo_server" 9003 > "$LOG/echo_9003b.log" 2>&1 &
sleep 5
"$BIN/tcpbench" 8080 --conns 16 --size_mb 2 --timeout 30 > "$LOG/t4c.log" 2>&1
gate "后端故障·恢复后 4 后端 bench 全过" $?

# ================= 8 建连黑洞 =================
sc "8" "建连黑洞·后端不可达" "SYN 无响应时 3s 建连期限体面关闭, fd 无泄漏"
cat > "$LOG/bad_config.json" <<'EOF'
{ "listen_port": 8081,
  "backends": [
    {"address": "10.255.255.1", "port": 9},
    {"address": "10.255.255.2", "port": 9}
  ],
  "balancer": "round_robin",
  "health_check_interval_sec": 0, "health_check_timeout_sec": 1 }
EOF
"${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/bad_config.json" > "$LOG/proxy_bad.log" 2>&1 &
BAD_PID=$!
wait_port 127.0.0.1 8081 50 || { echo "bad proxy 没起来"; kill "$BAD_PID"; }
sleep 0.3
BAD_BASE=$(fd_of "$BAD_PID")
python3 - <<'PYEOF'
import socket, time, sys
t0 = time.time()
s = socket.create_connection(("127.0.0.1", 8081), timeout=10)
s.settimeout(10)
try:
    data = s.recv(4096)
    got_eof = (data == b"")
except (ConnectionResetError, socket.timeout) as e:
    got_eof = (not isinstance(e, socket.timeout))
dt = time.time() - t0
ok = got_eof and dt < 6.0
print(f"closed={got_eof} within={dt:.2f}s -> {'OK' if ok else 'STUCK'}")
sys.exit(0 if ok else 1)
PYEOF
gate "建连黑洞·连接 6s 内被关(3s 期限生效)" $?
sleep 0.5
BAD_FD=$(fd_of "$BAD_PID")
[ "$BAD_FD" -le $((BAD_BASE + 2)) ]; gate "建连黑洞·无 fd 泄漏($BAD_FD<=$BAD_BASE+2)" $?
kill "$BAD_PID" 2>/dev/null; wait "$BAD_PID" 2>/dev/null

# ================= 9 无可用后端 =================
sc "9" "无可用后端·全池摘除" "健康检查全标死后新客户端被快速拒绝(不挂)"
cat > "$LOG/nobackend_config.json" <<EOJ
{"listen_port": 8086, "backends": [{"address": "127.0.0.1", "port": 9999}],
 "balancer": "round_robin", "health_check_interval_sec": 1, "health_check_timeout_sec": 1}
EOJ
"${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/nobackend_config.json" > "$LOG/proxy_nobackend.log" 2>&1 &
NOBACKEND_PID=$!
wait_port 127.0.0.1 8086 50 || { echo "无可用后端 proxy 没起来"; exit 1; }
sleep 3
python3 - <<'PYEOF'
import socket, time, sys
t0 = time.time()
s = socket.create_connection(("127.0.0.1", 8086), timeout=5)
s.settimeout(5)
try:
    data = s.recv(4096)
    got_eof = (data == b"")
except (ConnectionResetError, socket.timeout) as e:
    got_eof = (not isinstance(e, socket.timeout))
dt = time.time() - t0
ok = got_eof and dt < 2.0
print(f"closed={got_eof} within={dt:.2f}s -> {'OK' if ok else 'STUCK'}")
sys.exit(0 if ok else 1)
PYEOF
gate "无可用后端·客户端被快速拒绝(<2s)" $?
grep -aq 'No available backend' "$LOG/proxy_nobackend.log" && gate "无可用后端·打点出现" 0 || gate "无可用后端·打点出现" 1
kill "$NOBACKEND_PID" 2>/dev/null; wait "$NOBACKEND_PID" 2>/dev/null

# ================= 10 停滞计时器 =================
sc "10" "停滞计时器·只收死连接" "无进展连接 6s 被收; 持续有进展的连接不误杀"
echo "  -- 10a 静默滞留: 建连后一言不发 → 6s 收走 --"
one_cfg 8083 9000
TCP_IDLE_DEADLINE_SEC=6 "${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/cfg_8083.json" > "$LOG/proxy_t8.log" 2>&1 &
T8_PID=$!
wait_port 127.0.0.1 8083 50 || { echo "t8 proxy 没起来"; exit 1; }
python3 - <<'PYEOF' > "$LOG/t8_client.log" 2>&1 &
import socket, time
s = socket.create_connection(("127.0.0.1", 8083), timeout=10)
time.sleep(20)
s.close()
PYEOF
T8C_PID=$!
T8_DT=$(wait_alive0 "$T8_PID" "$LOG/proxy_t8.log" 16); T8_RC=$?
gate "停滞计时器·静默滞留被收走(alive=0, ${T8_DT}s)" "$T8_RC"
gate "停滞计时器·stall-giveup 打点出现" "$(grep -aq 'stall-giveup' "$LOG/proxy_t8.log" && echo 0 || echo 1)"
kill "$T8C_PID" 2>/dev/null; wait "$T8C_PID" 2>/dev/null
kill "$T8_PID" 2>/dev/null; wait "$T8_PID" 2>/dev/null

echo "  -- 10b 活跃对照: 每秒 1 字节持续有进展 → 不误杀 --"
one_cfg 8084 9000
TCP_IDLE_DEADLINE_SEC=6 "${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/cfg_8084.json" > "$LOG/proxy_alive.log" 2>&1 &
ALIVE_PID=$!
wait_port 127.0.0.1 8084 50 || { echo "活跃对照 proxy 没起来"; exit 1; }
python3 - <<'PYEOF' > "$LOG/alive_client.log" 2>&1 &
import socket, time
s = socket.create_connection(("127.0.0.1", 8084), timeout=10)
s.settimeout(3)
for i in range(15):
    try: s.send(b"x")
    except Exception: break
    time.sleep(1)
time.sleep(2)
s.close()
PYEOF
AC_PID=$!
sleep 8
AC_ST=$(stats_of "$ALIVE_PID" "$LOG/proxy_alive.log")
gate "停滞计时器·活跃 8s 时仍存活(alive=2)" "$(echo "$AC_ST" | grep -qc 'alive=2' && echo 0 || echo 1)"
sleep 8
AC_ST=$(stats_of "$ALIVE_PID" "$LOG/proxy_alive.log")
gate "停滞计时器·活跃 15s 内不被误杀(alive=2)" "$(echo "$AC_ST" | grep -qc 'alive=2' && echo 0 || echo 1)"
gate "停滞计时器·活跃期 0 次误杀(stall-giveup=0)" "$(grep -ac 'stall-giveup' "$LOG/proxy_alive.log" | grep -q '^0$' && echo 0 || echo 1)"
kill "$AC_PID" 2>/dev/null; wait "$AC_PID" 2>/dev/null
kill "$ALIVE_PID" 2>/dev/null; wait "$ALIVE_PID" 2>/dev/null

echo "  -- 10c 停摆: 活跃后突然沉默 → 6s 收走 --"
one_cfg 8085 9000
TCP_IDLE_DEADLINE_SEC=6 "${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/cfg_8085.json" > "$LOG/proxy_stall.log" 2>&1 &
STALL_PID=$!
wait_port 127.0.0.1 8085 50 || { echo "停摆 proxy 没起来"; exit 1; }
python3 - <<'PYEOF' > "$LOG/stall_client.log" 2>&1 &
import socket, time
s = socket.create_connection(("127.0.0.1", 8085), timeout=10)
s.settimeout(3)
for i in range(4):
    try: s.send(b"x" * 16)
    except Exception: break
    time.sleep(0.5)
time.sleep(20)
s.close()
PYEOF
SC_PID=$!
ST_DT=$(wait_alive0 "$STALL_PID" "$LOG/proxy_stall.log" 20); ST_RC=$?
gate "停滞计时器·停摆被收走(alive=0, ${ST_DT}s)" "$ST_RC"
kill "$SC_PID" 2>/dev/null; wait "$SC_PID" 2>/dev/null
kill "$STALL_PID" 2>/dev/null; wait "$STALL_PID" 2>/dev/null

# ================= 11 冻结收尾 =================
sc "11" "冻结收尾·对端冻结时停滞计时器有界收尾" "两触发: A 传输中被强杀(EOF/RST 路径) B 发完不读不关(纯停滞); 均无泄漏"
echo "  -- 11A 传输中 kill -9 客户端 ×3(不同 kill 时刻) --"
one_cfg 8087 9000
TCP_IDLE_DEADLINE_SEC=6 "${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/cfg_8087.json" > "$LOG/proxy_freeze_kill.log" 2>&1 &
FK_PID=$!
wait_port 127.0.0.1 8087 50 || { echo "freeze-kill proxy 没起来"; exit 1; }
FK_BASE=$(fd_of "$FK_PID")
for i in 1 2 3; do
  "$BIN/tcpbench" 8087 --conns 256 --size_mb 16 --timeout 60 > "$LOG/fk_$i.log" 2>&1 &
  BPID=$!
  sleep "$(awk -v x="$i" 'BEGIN{printf "%.1f", 0.8 + x * 1.2}')"
  kill -9 "$BPID" 2>/dev/null
  wait "$BPID" 2>/dev/null
done
FK_DT=$(wait_alive0 "$FK_PID" "$LOG/proxy_freeze_kill.log" 40); FK_RC=$?
gate "冻结收尾·被杀后 alive=0(${FK_DT}s)" "$FK_RC"
FD_NOW=$(fd_of "$FK_PID")
[ "$FD_NOW" -le $((FK_BASE + 3)) ]; gate "冻结收尾·被杀后 fd 回落($FD_NOW<=基线$FK_BASE+3)" $?
kill "$FK_PID" 2>/dev/null; wait "$FK_PID" 2>/dev/null

echo "  -- 11B 挂起客户端: 发 1MB 后不读不关(确定触发) --"
one_cfg 8088 9000
TCP_IDLE_DEADLINE_SEC=6 "${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/cfg_8088.json" > "$LOG/proxy_hang.log" 2>&1 &
HANG_PID=$!
wait_port 127.0.0.1 8088 50 || { echo "挂起客户端 proxy 没起来"; exit 1; }
python3 - <<'PYEOF' > "$LOG/hang_client.log" 2>&1 &
import socket, time
s = socket.create_connection(("127.0.0.1", 8088), timeout=10)
s.settimeout(5)
try:
    s.sendall(b"x" * (1024 * 1024))
except Exception as e:
    print(f"sendall err: {e}")
time.sleep(30)
s.close()
PYEOF
HC_PID=$!
sleep 2
HANG_DT=$(wait_alive0 "$HANG_PID" "$LOG/proxy_hang.log" 20); HANG_RC=$?
gate "冻结收尾·停滞计时器收走挂起连接(alive=0, ${HANG_DT}s)" "$HANG_RC"
gate "冻结收尾·stall-giveup 打点出现" "$(grep -aq 'stall-giveup' "$LOG/proxy_hang.log" && echo 0 || echo 1)"
kill "$HC_PID" 2>/dev/null; wait "$HC_PID" 2>/dev/null
kill "$HANG_PID" 2>/dev/null; wait "$HANG_PID" 2>/dev/null

# ================= 12 背压有界 =================
sc "12" "背压有界·后端只收不读" "输出端冻住: 暂停-排空路径生效, 代理内存有界, 客户端可感背压"
one_cfg 8082 9005
python3 - 18 > "$LOG/freeze_server.log" 2>&1 <<'PYEOF' &
import socket, sys, time
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 9005)); srv.listen(16)
conns = []; end = time.time() + float(sys.argv[1])
srv.settimeout(0.5)
while time.time() < end:
    try:
        c, _ = srv.accept(); conns.append(c)
    except socket.timeout:
        pass
for c in conns: c.close()
PYEOF
FREEZE_SRV_PID=$!
wait_port 127.0.0.1 9005 50 || { echo "mock server 没起来"; exit 1; }
"${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/cfg_8082.json" > "$LOG/proxy_freeze.log" 2>&1 &
FREEZE_PID=$!
wait_port 127.0.0.1 8082 50 || { echo "背压测试 proxy 没起来"; exit 1; }
sleep 4
FZ_BASE_FD=$(fd_of "$FREEZE_PID")
RSS_BEFORE=$(rss_kb "$FREEZE_PID")
python3 - 8 > "$LOG/freeze_client.log" 2>&1 <<'PYEOF' &
import socket, time, sys
s = socket.create_connection(("127.0.0.1", 8082), timeout=10)
s.settimeout(1.0)
chunk = b"x" * 65536
CAP = 512 * 1024 * 1024
sent, stalls, reason = 0, 0, "deadline"
deadline = time.time() + float(sys.argv[1])
while time.time() < deadline and sent < CAP:
    try:
        n = s.send(chunk); sent += n
        if n < len(chunk): stalls += 1
    except socket.timeout:
        stalls += 1
    except (BrokenPipeError, ConnectionResetError):
        reason = "reset"; break
if sent >= CAP: reason = "cap"
bp = stalls > 0 or reason == "reset"
print(f"pushed={sent/1048576:.1f}MB stalls={stalls} reason={reason} BP={'yes' if bp else 'no'}")
PYEOF
CPID=$!
sleep 10
kill "$CPID" 2>/dev/null; wait "$CPID" 2>/dev/null
RSS_AFTER=$(rss_kb "$FREEZE_PID")
RSS_GROW_MB=$(( (RSS_AFTER - RSS_BEFORE) / 1024 ))
gate "背压有界·冻结期代理存活" "$(kill -0 "$FREEZE_PID" 2>/dev/null && echo 0 || echo 1)"
sed 's/^/  | /' "$LOG/freeze_client.log"
gate "背压有界·客户端感到背压(卡顿/部分写/被掐断)" "$(grep -aq 'BP=yes' "$LOG/freeze_client.log" && echo 0 || echo 1)"
[ "$RSS_GROW_MB" -lt 256 ]; gate "背压有界·内存有界(增长 ${RSS_GROW_MB}MB < 256MB)" $?
FZ_DT=$(wait_alive0 "$FREEZE_PID" "$LOG/proxy_freeze.log" 15); FZ_RC=$?
gate "背压有界·解除后有界回收(${FZ_DT}s)" "$FZ_RC"
FD_NOW=$(fd_of "$FREEZE_PID")
[ "$FD_NOW" -le $((FZ_BASE_FD + 3)) ]; gate "背压有界·fd 回落($FD_NOW<=基线$FZ_BASE_FD+3)" $?
kill "$FREEZE_PID" 2>/dev/null; wait "$FREEZE_PID" 2>/dev/null
kill "$FREEZE_SRV_PID" 2>/dev/null; wait "$FREEZE_SRV_PID" 2>/dev/null

# ================= 13 关停 =================
sc "13" "关停·SIGTERM 必退且总是有界" "干净时立即排水退出; 滞留时第 1 次等待排水、第 2 次立即硬退"
echo "  -- 13a 干净关停(无滞留会话) --"
"$BIN/tcpbench" 8080 --conns 4 --size_mb 1 --timeout 15 > "$LOG/t6_pre.log" 2>&1
TERM_T0=$(date +%s.%N)
kill -TERM "$PROXY_PID"
for i in $(seq 100); do kill -0 "$PROXY_PID" 2>/dev/null || break; sleep 0.1; done
TERM_T1=$(date +%s.%N)
if kill -0 "$PROXY_PID" 2>/dev/null; then
  gate "关停·干净·10s 内退出" 1
  kill -9 "$PROXY_PID" 2>/dev/null
else
  DT=$(awk -v a="$TERM_T0" -v b="$TERM_T1" 'BEGIN{printf "%.1f", b-a}')
  gate "关停·干净·SIGTERM 后退出(${DT}s)" 0
fi
tail -2 "$LOG/proxy.log" | sed 's/^/  | /'
wait "$PROXY_PID" 2>/dev/null; T6_RC=$?
gate "关停·干净·退出码=0(rc=$T6_RC)" "$T6_RC"

echo "  -- 13b 滞留关停(半请求挂起) --"
TCP_RCVBUF_BYTES=$RCV_ENV TCP_SNDBUF_BYTES=$SND_ENV \
  "${STBUF[@]}" "$BIN/tcp_proxy" "$LOG/test_config.json" > "$LOG/proxy2.log" 2>&1 &
PROXY_PID=$!
wait_port 127.0.0.1 8080 50 || { echo "proxy2 没起来"; exit 1; }
python3 - <<'PYEOF' > "$LOG/t7_client.log" 2>&1 &
import socket, time
s = socket.create_connection(("127.0.0.1", 8080), timeout=10)
s.send(b"half-request-no-fin")
time.sleep(30)
s.close()
PYEOF
CPID=$!
sleep 1
T7_T0=$(date +%s.%N)
kill -TERM "$PROXY_PID"
sleep 1
if kill -0 "$PROXY_PID" 2>/dev/null; then
  gate "关停·滞留·第 1 次 SIGTERM 后仍在等排水" 0
else
  gate "关停·滞留·第 1 次 SIGTERM 后仍在等排水" 1
fi
kill -TERM "$PROXY_PID"
for i in $(seq 50); do kill -0 "$PROXY_PID" 2>/dev/null || break; sleep 0.1; done
T7_T1=$(date +%s.%N)
if kill -0 "$PROXY_PID" 2>/dev/null; then
  gate "关停·滞留·第 2 次 SIGTERM 后 5s 内退出" 1
  kill -9 "$PROXY_PID" 2>/dev/null
else
  DT=$(awk -v a="$T7_T0" -v b="$T7_T1" 'BEGIN{printf "%.1f", b-a}')
  PASS_OK=$(awk -v d="$DT" 'BEGIN{print (d>=1 && d<=6) ? 0 : 1}')
  gate "关停·滞留·第 2 次必退且有界(${DT}s, 应≈1~2s)" "$PASS_OK"
fi
grep -aq 'ForceQuit: hard exit' "$LOG/proxy2.log" \
  && gate "关停·滞留·硬退打点出现" 0 \
  || gate "关停·滞留·硬退打点出现" 1
wait "$PROXY_PID" 2>/dev/null; T7_RC=$?
case "$T7_RC" in
  0) gate "关停·滞留·退出码=0" 0 ;;
  *) gate "关停·滞留·退出码=0 (rc=$T7_RC ← 异常! 查 $LOG/asan.* 和 proxy2.log)" 1 ;;
esac
kill "$CPID" 2>/dev/null; wait "$CPID" 2>/dev/null

# ================= 判据外扫描(信息性) =================
echo
echo "==== 判据外扫描(异常打点, 信息性) ===="
for pat in 'connect timeout' 'stall-giveup'; do
  N=$(grep -ac "$pat" "$LOG/proxy.log" 2>/dev/null); N=${N:-0}
  echo "  proxy.log '$pat': $N"
done
if compgen -G "$LOG/asan.*" > /dev/null; then
  for f in "$LOG"/asan.*; do
    echo "  ⚠ ASAN 报告: $f"
    grep -a -m1 'ERROR: AddressSanitizer' "$f" | sed 's/^/    /'
    grep -a -m1 'SUMMARY: ' "$f" | sed 's/^/    /'
  done
  gate "无 ASAN 报告" 1
else
  echo "  无 asan.* 报告文件"
  gate "无 ASAN 报告" 0
fi

echo
echo "==== 汇总: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" = "0" ] && echo "ALL PASS" || echo "存在失败"
exit $(( FAIL > 0 ? 1 : 0 ))
