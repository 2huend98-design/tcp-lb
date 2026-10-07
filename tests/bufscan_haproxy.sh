#!/bin/bash
set -u

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_PATH")")"
cd "$ROOT_DIR"

BIN_DIR="$ROOT_DIR/build/bin"
LOG_DIR="$ROOT_DIR/build/bufscan_haproxy"
mkdir -p "$LOG_DIR"
RESULT="$LOG_DIR/result.txt"
SUMMARY="$LOG_DIR/summary.tsv"
: > "$RESULT"; : > "$SUMMARY"

# ---------- 前置检查 ----------
HA_BIN="${HA_BIN:-haproxy}"
MAXCONN="${MAXCONN:-65535}"
if [ ! -x "$HA_BIN" ]; then
    HA_BIN_RESOLVED=$(command -v "$HA_BIN" 2>/dev/null || true)
    [ -z "$HA_BIN_RESOLVED" ] && { echo "✗ haproxy 未找到 (HA_BIN=$HA_BIN)"; exit 1; }
    HA_BIN="$HA_BIN_RESOLVED"
fi
HA_VER=$("$HA_BIN" -vv 2>&1 | head -1)
echo ">>> HA_BIN = $HA_BIN"
echo ">>> $HA_VER"
MAX_BUF=$("$HA_BIN" -vv 2>&1 | grep -oP 'MAX_BUF_SIZE=\K\d+' | head -1 || echo "")
[ -z "$MAX_BUF" ] && MAX_BUF=262144
echo ">>> MAX_BUF_SIZE = $MAX_BUF"

# ---------- 协议参数 ----------
BUF_KIB_VALUES="${BUF_KIB_VALUES:-16 32 48 64 96 128 192 256 512 1024 2048 4096 8192}"
HAP_THREADS="${HAP_THREADS:-4}"
E="${E:-4}"
MTU="${MTU:-1500}"
TIMEOUT="${TIMEOUT:-70}"
CONNS="${CONNS:-1024}"
SIZE_MB="${SIZE_MB:-8}"
ROUNDS="${ROUNDS:-30}"
FAST_THRESHOLD="${FAST_THRESHOLD:-6000}"
STALL_THRESHOLD="${STALL_THRESHOLD:-3000}"
MEM_TARGET="${MEM_TARGET:-5000}"
MEM_WAIT_MAX="${MEM_WAIT_MAX:-90}"
SETTLE_SEC="${SETTLE_SEC:-3}"

MTU_ORIG=$(cat /sys/class/net/lo/mtu)
FD_ORIG=$(ulimit -n)

{
    echo "# bufscan_haproxy.sh @ $(date -Iseconds)"
    echo "# HA_BIN=$HA_BIN  MAX_BUF_SIZE=$MAX_BUF  MAXCONN=$MAXCONN"
    echo "# MTU: $MTU_ORIG -> $MTU"
    echo "# threads=$HAP_THREADS E=$E CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS"
    echo "# TIMEOUT=$TIMEOUT(outer=$((TIMEOUT+15)))  FAST_THRESHOLD=$FAST_THRESHOLD"
} >> "$RESULT"

printf 'BUF_KiB\tmed\tIQR\tmin\tmax\tstall\tfast\tretrans\tretrans_per_GB\n' >> "$SUMMARY"

for bin in tcpbench echo_server; do
    [ -x "$BIN_DIR/$bin" ] || { echo "✗ missing $BIN_DIR/$bin"; exit 1; }
done

CLEANED=0
cleanup() {
    [ "$CLEANED" = "1" ] && return; CLEANED=1
    pkill -9 -x tcpbench 2>/dev/null || true
    pkill -9 -x haproxy 2>/dev/null || true
    pkill -9 -x echo_server 2>/dev/null || true
    for p in 8080 $(seq 9000 9010); do sudo fuser -k $p/tcp 2>/dev/null; done
    sleep 1
    [ "$MTU" != "$MTU_ORIG" ] && sudo ip link set lo mtu "$MTU_ORIG" 2>/dev/null || true
    ulimit -n "$FD_ORIG" 2>/dev/null || true
    if pgrep -x haproxy >/dev/null || pgrep -x echo_server >/dev/null || pgrep -x tcpbench >/dev/null; then
        echo ">>> WARN leaked:"; pgrep -ax haproxy; pgrep -ax echo_server; pgrep -ax tcpbench
    else echo ">>> clean"; fi
}
trap 'cleanup' EXIT
trap 'exit 130' INT TERM

[ "$MTU" != "$MTU_ORIG" ] && { echo ">>> set lo mtu $MTU_ORIG -> $MTU"; sudo ip link set lo mtu "$MTU" || exit 1; }
ulimit -n 200000 2>/dev/null || true

clean_wait() {
    pkill -9 -x tcpbench 2>/dev/null || true
    pkill -9 -x haproxy 2>/dev/null || true
    pkill -9 -x echo_server 2>/dev/null || true
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

gen_haproxy_cfg() {
    local file=$1 buf=$2
    local maxrewrite=$((buf / 16))
    [ "$maxrewrite" -lt 1024 ] && maxrewrite=1024
    [ "$maxrewrite" -gt 16384 ] && maxrewrite=16384
    local servers=""
    for ((i=0; i<E; i++)); do servers="$servers
    server s$i 127.0.0.1:$((9000+i))"; done
    cat > "$file" <<EOF
global
    nbthread $HAP_THREADS
    maxconn $MAXCONN
    log stdout format raw local0 warning
    tune.bufsize $buf
    tune.maxrewrite $maxrewrite
    tune.disable-zero-copy-forwarding

defaults
    mode tcp
    timeout connect 5s
    timeout client  60s
    timeout server  60s
    option tcplog

frontend fe
    bind 127.0.0.1:8080
    default_backend be

backend be
    balance roundrobin
$servers
EOF
    if ! "$HA_BIN" -c -f "$file" >/dev/null 2>&1; then
        sed -i '/tune.disable-zero-copy-forwarding/d' "$file"
        "$HA_BIN" -c -f "$file" >/dev/null 2>&1 || echo "    (cfg 校验仍失败，查启动日志)"
    fi
}

TOTAL=0; for _ in $BUF_KIB_VALUES; do TOTAL=$((TOTAL+1)); done
echo "============================================================"
echo "  bufscan_haproxy.sh  |  BUF ∈ $BUF_KIB_VALUES KiB"
echo "  HA_BIN=$HA_BIN  threads=$HAP_THREADS  E=$E  maxconn=$MAXCONN"
echo "  CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS  MTU=$MTU  TIMEOUT=$TIMEOUT"
echo "============================================================"

IDX=0; T0_ALL=$(date +%s)
for BUF_KIB in $BUF_KIB_VALUES; do
    IDX=$((IDX+1)); BUF_BYTES=$((BUF_KIB * 1024))
    if [ "$BUF_BYTES" -gt "$MAX_BUF" ]; then
        printf '\n########## [%d/%d] BUF=%s KiB  SKIP (exceeds MAX_BUF_SIZE=%s)\n' "$IDX" "$TOTAL" "$BUF_KIB" "$MAX_BUF"
        printf '%s\tSKIP_MAXBUF\n' "$BUF_KIB" >> "$SUMMARY"; continue
    fi
    TAG="HAP_B${BUF_KIB}_T${HAP_THREADS}_E${E}"
    printf '\n########## [%d/%d] BUF=%s KiB (%s B) = %s ##########\n' "$IDX" "$TOTAL" "$BUF_KIB" "$BUF_BYTES" "$TAG"
    clean_wait

    for ((i=0; i<E; i++)); do
        "$BIN_DIR/echo_server" $((9000+i)) >"$LOG_DIR/echo_${TAG}_$((9000+i)).log" 2>&1 &
    done
    sleep 0.8

    CFG="$LOG_DIR/haproxy_${TAG}.cfg"; gen_haproxy_cfg "$CFG" "$BUF_BYTES"
    PROXY_LOG="$LOG_DIR/haproxy_${TAG}.log"
    "$HA_BIN" -f "$CFG" >"$PROXY_LOG" 2>&1 &
    PROXY_PID=$!

    ok=0
    for _ in $(seq 20); do
        ss -tln 2>/dev/null | grep -q ':8080' && { ok=1; break; }
        kill -0 $PROXY_PID 2>/dev/null || break
        sleep 0.5
    done
    if [ "$ok" != 1 ]; then
        echo "  FAIL: haproxy 没监听"
        tail -20 "$PROXY_LOG" | sed 's/^/      /'
        printf '%s\tFAILED\n' "$BUF_KIB" >> "$SUMMARY"
        kill -9 $PROXY_PID 2>/dev/null; continue
    fi

    for _ in 1 2; do one_round $SIZE_MB >/dev/null; done
    R0=$(read_retrans)

    RAW=()
    printf '  measure:'
    for ((i=1; i<=ROUNDS; i++)); do
        R=$(one_round $SIZE_MB); RAW+=("$R"); printf ' %s' "$R"
    done
    echo
    R1=$(read_retrans); RETRANS=$((R1 - R0))

    kill -TERM $PROXY_PID 2>/dev/null; sleep 0.5
    kill -9 $PROXY_PID 2>/dev/null

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

    LINE="[$TAG] BUF=$BUF_KIB KiB | valid=${#NVALS[@]}/$ROUNDS med=$MED iqr=$IQR min=$MIN max=$MAX | fast=$FAST stall=$STALL_C | retrans=$RETRANS (per_GB=$R_PER_GB)"
    echo "  => $LINE"; echo "$LINE" >> "$RESULT"; echo "  raw: ${RAW[*]}" >> "$RESULT"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$BUF_KIB" "$MED" "$IQR" "$MIN" "$MAX" "$STALL_C" "$FAST" "$RETRANS" "$R_PER_GB" >> "$SUMMARY"
done

T1_ALL=$(date +%s)
echo; echo "════════════════════════════════════════════════════════════"
echo "  主表"; echo "════════════════════════════════════════════════════════════"
column -t -s $'\t' "$SUMMARY"
echo; echo "  total: $((T1_ALL - T0_ALL))s"; echo "  tsv: $SUMMARY"
