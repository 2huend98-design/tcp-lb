#!/bin/bash
set -u

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_PATH")")"
cd "$ROOT_DIR"

LOG_DIR="$ROOT_DIR/build/bufscan_nginx"
mkdir -p "$LOG_DIR"
RESULT="$LOG_DIR/result.txt"
SUMMARY="$LOG_DIR/summary.tsv"
: > "$RESULT"; : > "$SUMMARY"

# ---------- 协议参数 ----------
BUF_KIB_VALUES="${BUF_KIB_VALUES:-16 32 48 64 96 128 192 256 512 1024 2048 4096 8192}"
W="${W:-4}"
E="${E:-4}"
MTU="${MTU:-1500}"
STALL="${STALL:-60}"
TIMEOUT="${TIMEOUT:-70}"
CONNS="${CONNS:-1024}"
SIZE_MB="${SIZE_MB:-8}"
ROUNDS="${ROUNDS:-30}"
FAST_THRESHOLD="${FAST_THRESHOLD:-6000}"
STALL_THRESHOLD="${STALL_THRESHOLD:-3000}"
MEM_TARGET="${MEM_TARGET:-5000}"
MEM_WAIT_MAX="${MEM_WAIT_MAX:-90}"
SETTLE_SEC="${SETTLE_SEC:-3}"
NGINX_BIN="${NGINX_BIN:-nginx}"
RENDER_ONLY="${RENDER_ONLY:-0}"

# ---------- nginx 检查 ----------
command -v "$NGINX_BIN" >/dev/null || { echo "✗ nginx 未找到 (NGINX_BIN=$NGINX_BIN)"; exit 1; }
NGINX_VER=$("$NGINX_BIN" -v 2>&1)
NGINX_ARGS=$("$NGINX_BIN" -V 2>&1 | tail -1)
LOAD_LINE=""
if echo "$NGINX_ARGS" | grep -q -- '--with-stream=dynamic'; then
    MOD_DIR=$(echo "$NGINX_ARGS" | grep -oP '(?<=--modules-path=)\S+' || echo /usr/lib/nginx/modules)
    LOAD_LINE="load_module $MOD_DIR/ngx_stream_module.so;"
elif ! echo "$NGINX_ARGS" | grep -q -- '--with-stream'; then
    echo "✗ 此 nginx 未编译 stream 模块"; exit 1
fi

BIN_DIR="$ROOT_DIR/build/bin"
for bin in tcpbench echo_server; do
    [ -x "$BIN_DIR/$bin" ] || { echo "✗ missing $BIN_DIR/$bin"; exit 1; }
done

MTU_ORIG=$(cat /sys/class/net/lo/mtu)
FD_ORIG=$(ulimit -n)

NGINX_PID=""
CLEANED=0
cleanup() {
    [ "$CLEANED" = "1" ] && return; CLEANED=1
    [ -n "$NGINX_PID" ] && { kill -9 "$NGINX_PID" 2>/dev/null || true; }
    pkill -9 -f 'bin/tcpbench' 2>/dev/null || true
    pkill -9 -f 'bin/echo_server' 2>/dev/null || true
    for p in 8080 $(seq 9000 9010); do sudo fuser -k $p/tcp 2>/dev/null; done
    sleep 1
    [ "$MTU" != "$MTU_ORIG" ] && sudo ip link set lo mtu "$MTU_ORIG" 2>/dev/null || true
    ulimit -n "$FD_ORIG" 2>/dev/null || true
    pgrep -f 'nginx: master|echo_server|tcpbench' >/dev/null \
        && { echo ">>> WARN leaked:"; pgrep -af 'nginx: master|echo_server|tcpbench'; } \
        || echo ">>> clean"
}
trap 'cleanup' EXIT
trap 'exit 130' INT TERM

clean_wait() {
    pkill -9 -f 'bin/tcpbench' 2>/dev/null || true
    pkill -9 -f 'bin/echo_server' 2>/dev/null || true
    for p in 8080 $(seq 9000 9010); do sudo fuser -k $p/tcp 2>/dev/null; done
    local waited=0 mem=0
    while [ $waited -lt $MEM_WAIT_MAX ]; do
        mem=$(awk '/^TCP:/ {for(i=1;i<=NF;i++) if($i=="mem") print $(i+1)}' /proc/net/sockstat)
        [ -z "$mem" ] && mem=0
        [ "$mem" -lt $MEM_TARGET ] && break
        sleep 3; waited=$((waited+3))
    done
    sudo ip route flush cache 2>/dev/null || true
    sleep "$SETTLE_SEC"
    printf '    [clean] mem=%s(%ds) settle=%ds\n' "$mem" "$waited" "$SETTLE_SEC"
}

read_retrans() {
    awk '/^Tcp:/ {n++; if(n==1){for(i=1;i<=NF;i++) if($i=="RetransSegs") c=i} else if(n==2&&c){print $c; exit}}' /proc/net/snmp
}

one_round() {
    local R
    R=$(timeout $((TIMEOUT+15)) "$BIN_DIR/tcpbench" 8080 \
        --conns $CONNS --size_mb $1 --timeout $TIMEOUT --procs 1 2>&1 \
        | grep -o 'throughput=[0-9]*' | tail -1 | cut -d= -f2)
    echo "${R:-0}"
}

gen_nginx_conf() {
    local file=$1 buf=$2 tag=$3 ups=""
    for ((i=0; i<E; i++)); do ups="$ups    server 127.0.0.1:$((9000+i));"$'\n'; done
    cat > "$file" <<EOF
${LOAD_LINE}
worker_processes $W;
worker_rlimit_nofile 200000;
worker_shutdown_timeout 5s;
error_log $LOG_DIR/nginx_err_$tag.log warn;
pid $LOG_DIR/nginx_$tag.pid;
daemon off;
events { worker_connections 8192; }
stream {
    upstream echo_pool {
${ups}    }
    server {
        listen 8080 backlog=65535;
        proxy_pass echo_pool;
        proxy_buffer_size ${buf}k;
        proxy_connect_timeout 5s;
        proxy_timeout ${STALL}s;
        tcp_nodelay on;
    }
}
EOF
}

# ============================================================
# RENDER_ONLY 模式：只渲染 conf + nginx -t，不动环境
# ============================================================
if [ "$RENDER_ONLY" = "1" ]; then
    echo ">>> RENDER_ONLY 预检: $NGINX_VER"
    echo ">>> 跳过 MTU/ulimit/归属检查"
    for BUF in $BUF_KIB_VALUES; do
        CONF="$LOG_DIR/conf_preflight_B${BUF}.conf"
        gen_nginx_conf "$CONF" "$BUF" "preflight_B${BUF}"
        if "$NGINX_BIN" -t -c "$CONF" >/dev/null 2>&1; then
            echo "  B${BUF}k  ✅ accepted"
        else
            echo "  B${BUF}k  ❌ rejected: $("$NGINX_BIN" -t -c "$CONF" 2>&1 | tail -2 | tr '\n' ' ')"
        fi
    done
    exit 0
fi

# ---------- 正式模式 ----------
if pgrep -x nginx >/dev/null 2>&1; then
    echo "✗ 检测到已有 nginx 进程——先 stop + disable 系统 nginx，勿盲杀"; exit 1
fi
[ "$MTU" != "$MTU_ORIG" ] && { echo ">>> set lo mtu $MTU_ORIG -> $MTU"; sudo ip link set lo mtu "$MTU" || exit 1; }
ulimit -n 200000 2>/dev/null || true

{
    echo "# bufscan_nginx.sh @ $(date -Iseconds)"
    echo "# MTU: $MTU_ORIG -> $MTU"
    echo "# $NGINX_VER"
    echo "# W=$W(worker_processes) E=$E CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS"
    echo "# STALL=$STALL(=proxy_timeout) TIMEOUT=$TIMEOUT(outer=$((TIMEOUT+15)))  FAST_THRESHOLD=$FAST_THRESHOLD"
} >> "$RESULT"

printf 'BUF_KiB\tmed\tIQR\tmin\tmax\tstall\tfast\tretrans\tretrans_per_GB\n' >> "$SUMMARY"

TOTAL=0; for _ in $BUF_KIB_VALUES; do TOTAL=$((TOTAL+1)); done
echo "============================================================"
echo "  bufscan_nginx.sh  |  BUF ∈ $BUF_KIB_VALUES"
echo "  $NGINX_VER  W=$W E=$E CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS"
echo "  MTU=$MTU STALL=$STALL TIMEOUT=$TIMEOUT"
echo "============================================================"

IDX=0; T0_ALL=$(date +%s)
for BUF in $BUF_KIB_VALUES; do
    IDX=$((IDX+1)); TAG="B${BUF}_W${W}_E${E}"
    printf '\n########## [%d/%d] BUF=%s ##########\n' "$IDX" "$TOTAL" "$BUF"
    clean_wait

    for ((i=0; i<E; i++)); do
        "$BIN_DIR/echo_server" $((9000+i)) >"$LOG_DIR/echo_${TAG}_$((9000+i)).log" 2>&1 &
    done
    sleep 0.8

    CONF="$LOG_DIR/conf_${TAG}.conf"; gen_nginx_conf "$CONF" "$BUF" "$TAG"
    "$NGINX_BIN" -c "$CONF" >"$LOG_DIR/nginx_out_${TAG}.log" 2>&1 &
    NGINX_PID=$!

    ok=0
    for _ in $(seq 20); do
        ss -tln 2>/dev/null | grep -q ':8080' && { ok=1; break; }
        kill -0 $NGINX_PID 2>/dev/null || break
        sleep 0.5
    done
    if [ "$ok" != 1 ]; then
        echo "  FAIL: nginx 没监听"
        tail -20 "$LOG_DIR/nginx_err_${TAG}.log" 2>/dev/null | sed 's/^/      /'
        echo -e "$BUF\tFAILED" >> "$SUMMARY"
        kill -9 $NGINX_PID 2>/dev/null; NGINX_PID=""; continue
    fi

    for _ in 1 2; do one_round $SIZE_MB >/dev/null; done
    R0=$(read_retrans)

    RAW=()
    printf '  measure:'
    for ((i=1; i<=ROUNDS; i++)); do
        R=$(one_round $SIZE_MB); RAW+=("$R"); printf ' %s' "$R"
        grep -qa 'upstream timed out\|could not connect' "$LOG_DIR/nginx_err_${TAG}.log" 2>/dev/null \
            && { printf ' [UPSTREAM-ERR]'; break; }
    done
    echo
    R1=$(read_retrans); RETRANS=$((R1 - R0))

    sleep 1
    kill -QUIT $NGINX_PID 2>/dev/null
    for _ in $(seq 16); do kill -0 $NGINX_PID 2>/dev/null || break; sleep 0.5; done
    kill -9 $NGINX_PID 2>/dev/null; NGINX_PID=""

    FAST=0; STALL_C=0; NVALS=()
    for v in "${RAW[@]}"; do
        if [ "$v" -lt $STALL_THRESHOLD ]; then STALL_C=$((STALL_C+1))
        else NVALS+=("$v"); [ "$v" -gt $FAST_THRESHOLD ] && FAST=$((FAST+1)); fi
    done

    if [ ${#NVALS[@]} -gt 0 ]; then
        ST=$(printf '%s\n' "${NVALS[@]}" | sort -n | awk '
            {v[NR]=$1} END {n=NR
                med=(n%2)?v[(n+1)/2]:int((v[n/2]+v[n/2+1])/2)
                p25=v[int(n*0.25)+1]; p75=v[int(n*0.75)+1]
                printf "%d|%d|%d|%d",med,(p75-p25),v[1],v[n]}')
        IFS='|' read MED IQR MIN MAX <<< "$ST"
    else MED=0; IQR=0; MIN=0; MAX=0; fi

    RUNS=${#RAW[@]}; [ "$RUNS" -eq 0 ] && RUNS=1
    GB=$((CONNS * SIZE_MB * RUNS / 1024)); [ "$GB" -eq 0 ] && GB=1
    R_PER_GB=$((RETRANS / GB))

    LINE="[$TAG] BUF=$BUF | valid=${#NVALS[@]}/$ROUNDS med=$MED iqr=$IQR min=$MIN max=$MAX | fast=$FAST stall=$STALL_C | retrans=$RETRANS (per_GB=$R_PER_GB)"
    echo "  => $LINE"; echo "$LINE" >> "$RESULT"; echo "  raw: ${RAW[*]}" >> "$RESULT"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$BUF" "$MED" "$IQR" "$MIN" "$MAX" "$STALL_C" "$FAST" "$RETRANS" "$R_PER_GB" >> "$SUMMARY"
done

T1_ALL=$(date +%s)
echo; echo "════════════════════════════════════════════════════════════"
echo "  主表"; echo "════════════════════════════════════════════════════════════"
column -t -s $'\t' "$SUMMARY"
echo; echo "  total: $((T1_ALL - T0_ALL))s"; echo "  tsv: $SUMMARY"
