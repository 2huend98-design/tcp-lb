#!/bin/bash
set -u

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_PATH")")"
cd "$ROOT_DIR"

BIN_DIR="$ROOT_DIR/build/bin"
LOG_DIR="$ROOT_DIR/build/bufscan_tcp"
mkdir -p "$LOG_DIR"
RESULT="$LOG_DIR/result.txt"
SUMMARY="$LOG_DIR/summary.tsv"
: > "$RESULT"; : > "$SUMMARY"

# ---------- 协议参数 ----------
BUF_VALUES="${BUF_VALUES:-16 32 48 64 96 128 192 256 512 1024 2048 4096 8192}"
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

MTU_ORIG=$(cat /sys/class/net/lo/mtu)
FD_ORIG=$(ulimit -n)

{
    echo "# bufscan_tcp.sh @ $(date -Iseconds)"
    echo "# MTU: $MTU_ORIG -> $MTU"
    echo "# W=$W E=$E CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS"
    echo "# STALL=$STALL TIMEOUT=$TIMEOUT(outer=$((TIMEOUT+15)))"
    echo "# BUF_VALUES=$BUF_VALUES  FAST_THRESHOLD=$FAST_THRESHOLD"
    echo "# RCV/SND = autotune (code default)"
} >> "$RESULT"

printf 'BUF_KiB\tmed\tIQR\tmin\tmax\tstall\tfast\tretrans\tretrans_per_GB\tpause\trecover\tstallgiveup\n' >> "$SUMMARY"

for bin in tcpbench tcp_proxy echo_server; do
    [ -x "$BIN_DIR/$bin" ] || { echo "✗ missing $BIN_DIR/$bin"; exit 1; }
done

CLEANED=0
cleanup() {
    [ "$CLEANED" = "1" ] && return; CLEANED=1
    pkill -9 -f 'bin/tcpbench' 2>/dev/null || true
    pkill -9 -f 'bin/tcp_proxy' 2>/dev/null || true
    pkill -9 -f 'bin/echo_server' 2>/dev/null || true
    for p in 8080 $(seq 9000 9010); do sudo fuser -k $p/tcp 2>/dev/null; done
    sleep 1
    [ "$MTU" != "$MTU_ORIG" ] && sudo ip link set lo mtu "$MTU_ORIG" 2>/dev/null || true
    ulimit -n "$FD_ORIG" 2>/dev/null || true
    pgrep -f 'echo_server|tcp_proxy|tcpbench' >/dev/null \
        && { echo ">>> WARN leaked:"; pgrep -af 'echo_server|tcp_proxy|tcpbench'; } \
        || echo ">>> clean"
}
trap 'cleanup' EXIT
trap 'exit 130' INT TERM

[ "$MTU" != "$MTU_ORIG" ] && { echo ">>> set lo mtu $MTU_ORIG -> $MTU"; sudo ip link set lo mtu "$MTU" || exit 1; }
ulimit -n 200000 2>/dev/null || true

clean_wait() {
    pkill -9 -f 'bin/tcpbench' 2>/dev/null || true
    pkill -9 -f 'bin/tcp_proxy' 2>/dev/null || true
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

gen_json() {
    local file=$1 buf=$2 backs=""
    for ((i=0; i<E; i++)); do backs="$backs,{\"address\":\"127.0.0.1\",\"port\":$((9000+i))}"; done
    cat > "$file" <<JEOF
{ "listen_port": 8080, "backends": [${backs#,}],
  "balancer":"round_robin","health_check_interval_sec":0,"health_check_timeout_sec":1,
  "worker_threads":$W,"output_buffer_kb":$buf }
JEOF
}

TOTAL=0; for _ in $BUF_VALUES; do TOTAL=$((TOTAL+1)); done
echo "============================================================"
echo "  bufscan_tcp.sh  |  BUF ∈ $BUF_VALUES"
echo "  W=$W E=$E  CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS"
echo "  MTU=$MTU STALL=$STALL TIMEOUT=$TIMEOUT"
echo "============================================================"

IDX=0; T0_ALL=$(date +%s)
for BUF in $BUF_VALUES; do
    IDX=$((IDX+1)); TAG="B${BUF}_W${W}_E${E}"
    printf '\n########## [%d/%d] BUF=%s ##########\n' "$IDX" "$TOTAL" "$BUF"
    clean_wait

    for ((i=0; i<E; i++)); do
        "$BIN_DIR/echo_server" $((9000+i)) >"$LOG_DIR/echo_${TAG}_$((9000+i)).log" 2>&1 &
    done
    sleep 0.8

    PJ="$LOG_DIR/p_${TAG}.json"; gen_json "$PJ" "$BUF"
    PROXY_LOG="$LOG_DIR/proxy_${TAG}.log"
    env TCP_IDLE_DEADLINE_SEC=$STALL "$BIN_DIR/tcp_proxy" "$PJ" >"$PROXY_LOG" 2>&1 &
    PROXY_PID=$!

    ok=0
    for _ in $(seq 20); do
        ss -tln 2>/dev/null | grep -q ':8080' && { ok=1; break; }
        kill -0 $PROXY_PID 2>/dev/null || break
        sleep 0.5
    done
    if [ "$ok" != 1 ]; then
        echo "  FAIL: proxy 没监听"
        tail -20 "$PROXY_LOG" | sed 's/^/      /'
        echo -e "$BUF\tFAILED" >> "$SUMMARY"
        kill -9 $PROXY_PID 2>/dev/null; continue
    fi

    for _ in 1 2; do one_round $SIZE_MB >/dev/null; done
    R0=$(read_retrans)

    RAW=()
    printf '  measure:'
    for ((i=1; i<=ROUNDS; i++)); do
        R=$(one_round $SIZE_MB); RAW+=("$R"); printf ' %s' "$R"
        grep -qa '\[freeze-dump\]' "$PROXY_LOG" && { printf ' [FREEZE]'; break; }
    done
    echo
    R1=$(read_retrans); RETRANS=$((R1 - R0))

    sleep 1
    kill -TERM $PROXY_PID 2>/dev/null; sleep 0.3
    kill -9 $PROXY_PID 2>/dev/null

    PAUSE=$(grep -ac '\[dbg-pause\]' "$PROXY_LOG" 2>/dev/null); PAUSE=${PAUSE:-0}
    RECOVER=$(grep -ac '\[dbg-recover\]' "$PROXY_LOG" 2>/dev/null); RECOVER=${RECOVER:-0}
    STALLG=$(grep -ac 'stall-giveup' "$PROXY_LOG" 2>/dev/null); STALLG=${STALLG:-0}

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

    LINE="[$TAG] BUF=$BUF | valid=${#NVALS[@]}/$ROUNDS med=$MED iqr=$IQR min=$MIN max=$MAX | fast=$FAST stall=$STALL_C | pause=$PAUSE recover=$RECOVER stall-giveup=$STALLG | retrans=$RETRANS (per_GB=$R_PER_GB)"
    echo "  => $LINE"; echo "$LINE" >> "$RESULT"; echo "  raw: ${RAW[*]}" >> "$RESULT"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$BUF" "$MED" "$IQR" "$MIN" "$MAX" "$STALL_C" "$FAST" "$RETRANS" "$R_PER_GB" "$PAUSE" "$RECOVER" "$STALLG" >> "$SUMMARY"
done

T1_ALL=$(date +%s)
echo; echo "════════════════════════════════════════════════════════════"
echo "  主表"; echo "════════════════════════════════════════════════════════════"
column -t -s $'\t' "$SUMMARY"
echo; echo "  total: $((T1_ALL - T0_ALL))s"; echo "  tsv: $SUMMARY"
