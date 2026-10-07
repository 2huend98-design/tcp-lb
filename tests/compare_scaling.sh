#!/bin/bash
set -u
# ============================================================
#  tests/compare_scaling.sh —— 三方并发扩展性对比
# ============================================================
# 矩阵：BUF × CONNS × 代理
#   BUF   ∈ {16, 64, 128, 1024, 4096, 8192} KiB
#   CONNS ∈ {1, 2, 3, 4, 128, 1024}
#   代理   = {tcp-lb, haproxy, nginx}
# 每档保持总量 ≈ 8GB（SIZE_MB = 8192 / CONNS）
# 2 轮 warmup + 30 轮计量；各自代理用同一 BUF
#
# 隔离（三层）：
#   1. bufscan_*.sh 内部 clean_wait（mem 沉降 + route flush）
#   2. compare_scaling 的 iso_quiet：cell 间 + proxy 间 强制清场
#   3. trap：Ctrl-C/异常退出时清场
#
# 输出：build/compare_scaling/
#   scaling.tsv    —— 原始数据
#   matrix.tsv     —— pivot 视图
# 断点续跑：已有行会被跳过
# ============================================================

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_PATH")")"
cd "$ROOT_DIR"

OUT="$ROOT_DIR/build/compare_scaling"
mkdir -p "$OUT"
TSV="$OUT/scaling.tsv"
PIVOT="$OUT/matrix.tsv"

# 表头（首次创建）
if [ ! -s "$TSV" ]; then
    printf 'BUF_KiB\tCONNS\tSIZE_MB\ttcp_med\ttcp_iqr\ttcp_stall\thap_med\thap_iqr\thap_stall\tngx_med\tngx_iqr\tngx_stall\n' > "$TSV"
fi

# ---------- 参数 ----------
BUFS="${BUFS:-16 64 128 1024 4096 8192}"
CONNS_LIST="${CONNS_LIST:-1 2 3 4 128 1024}"
TOTAL_GB="${TOTAL_GB:-8}"
ROUNDS="${ROUNDS:-30}"
HA_BIN="${HA_BIN:-/opt/hap16m/bin/haproxy}"
MAXCONN="${MAXCONN:-4096}"
SETTLE_SEC="${SETTLE_SEC:-3}"
MEM_TARGET="${MEM_TARGET:-5000}"
MEM_WAIT_MAX="${MEM_WAIT_MAX:-60}"

TOTAL_CELLS=0
for _ in $BUFS; do for __ in $CONNS_LIST; do TOTAL_CELLS=$((TOTAL_CELLS+1)); done; done

echo "============================================================"
echo "  compare_scaling.sh"
echo "  BUFS ∈ $BUFS"
echo "  CONNS ∈ $CONNS_LIST"
echo "  TOTAL_GB=$TOTAL_GB  ROUNDS=$ROUNDS  SETTLE_SEC=$SETTLE_SEC"
echo "  HA_BIN=$HA_BIN  MAXCONN=$MAXCONN"
echo "  总 cell = $TOTAL_CELLS × 3 代理 = $((TOTAL_CELLS*3))"
echo "============================================================"

# ---------- 隔离：强清场 ----------
iso_quiet() {
    pkill -9 -x tcpbench   2>/dev/null || true
    pkill -9 -x haproxy    2>/dev/null || true
    pkill -9 -x nginx      2>/dev/null || true
    pkill -9 -f 'bin/tcp_proxy' 2>/dev/null || true
    pkill -9 -f 'bin/echo_server' 2>/dev/null || true
    for p in 8080 $(seq 9000 9010); do sudo fuser -k $p/tcp 2>/dev/null; done
    sudo ip route flush cache 2>/dev/null || true

    # 等 mem 沉降
    local waited=0 mem=0
    while [ $waited -lt $MEM_WAIT_MAX ]; do
        mem=$(awk '/^TCP:/ {for(i=1;i<=NF;i++) if($i=="mem") print $(i+1)}' /proc/net/sockstat)
        [ -z "$mem" ] && mem=0
        [ "$mem" -lt $MEM_TARGET ] && break
        sleep 2; waited=$((waited+2))
    done
    sleep "$SETTLE_SEC"
    printf '    [iso] mem=%s(%ds) settle=%ds\n' "$mem" "$waited" "$SETTLE_SEC"
}

# ---------- trap ----------
CLEANED=0
cleanup() {
    [ "$CLEANED" = "1" ] && return; CLEANED=1
    echo
    echo ">>> cleanup ..."
    pkill -9 -x tcpbench   2>/dev/null || true
    pkill -9 -x haproxy    2>/dev/null || true
    pkill -9 -x nginx      2>/dev/null || true
    pkill -9 -f 'bin/tcp_proxy' 2>/dev/null || true
    pkill -9 -f 'bin/echo_server' 2>/dev/null || true
    for p in 8080 $(seq 9000 9010); do sudo fuser -k $p/tcp 2>/dev/null; done
    sleep 1
    pgrep -x haproxy >/dev/null || pgrep -x nginx >/dev/null \
        || pgrep -f 'bin/tcp_proxy|bin/echo_server|bin/tcpbench' >/dev/null \
        && { echo ">>> WARN leaked"; pgrep -ax haproxy; pgrep -ax nginx; } \
        || echo ">>> clean"
}
trap 'cleanup' EXIT INT TERM

# ---------- 停系统服务（防污染） ----------
sudo systemctl stop haproxy nginx 2>/dev/null || true
pgrep -x haproxy >/dev/null && { echo "✗ system haproxy running"; exit 1; }
pgrep -x nginx   >/dev/null && { echo "✗ system nginx running"; exit 1; }
echo ">>> system haproxy/nginx stopped"
echo

# ---------- 单代理单 cell 运行器 ----------
# 返回格式：med iqr stall
run_cell() {
    local proxy=$1 BUF=$2 CONNS=$3 SIZE_MB=$4
    local log="$OUT/${proxy}_B${BUF}_C${CONNS}.log"

    case "$proxy" in
        tcp)
            CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS BUF_VALUES="$BUF" \
                bash tests/bufscan_tcp.sh > "$log" 2>&1
            local line
            line=$(grep -E "^${BUF}\b" build/bufscan_tcp/summary.tsv 2>/dev/null | tail -1)
            if [ -z "$line" ]; then echo "0 0 0"; return; fi
            # tcp summary: BUF med IQR min max stall fast retrans per_GB [pause recover sg]
            echo "$line" | awk '{print $2, $3, $6}'
            ;;
        hap)
            HA_BIN=$HA_BIN MAXCONN=$MAXCONN \
            CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS BUF_KIB_VALUES="$BUF" \
                bash tests/bufscan_haproxy.sh > "$log" 2>&1
            local line
            line=$(grep -E "^${BUF}\b" build/bufscan_haproxy/summary.tsv 2>/dev/null | tail -1)
            if [ -z "$line" ]; then echo "0 0 0"; return; fi
            # hap summary: BUF med IQR min max stall fast retrans per_GB
            echo "$line" | awk '{print $2, $3, $6}'
            ;;
        nginx)
            CONNS=$CONNS SIZE_MB=$SIZE_MB ROUNDS=$ROUNDS BUF_KIB_VALUES="$BUF" \
                bash tests/bufscan_nginx.sh > "$log" 2>&1
            local line
            line=$(grep -E "^${BUF}\b" build/bufscan_nginx/summary.tsv 2>/dev/null | tail -1)
            if [ -z "$line" ]; then echo "0 0 0"; return; fi
            # nginx summary: BUF med IQR min max stall fast retrans per_GB
            echo "$line" | awk '{print $2, $3, $6}'
            ;;
    esac
}

# ---------- 主循环 ----------
T0=$(date +%s)
DONE=0

for BUF in $BUFS; do
    for CONNS in $CONNS_LIST; do
        DONE=$((DONE+1))
        SIZE_MB=$((TOTAL_GB * 1024 / CONNS))
        ELAPSED=$(( $(date +%s) - T0 ))

        # 断点续跑：跳过已完成
        if grep -qE "^${BUF}\s+${CONNS}\s" "$TSV"; then
            echo "[$DONE/$TOTAL_CELLS] BUF=$BUF CONNS=$CONNS SKIP (already done)"
            continue
        fi

        printf '\n########## [%d/%d] BUF=%s KiB  CONNS=%s  SIZE_MB=%s  (elapsed=%ds) ##########\n' \
            "$DONE" "$TOTAL_CELLS" "$BUF" "$CONNS" "$SIZE_MB" "$ELAPSED"

        # proxy 间强制隔离
        iso_quiet
        TCP_RES=$(run_cell tcp   "$BUF" "$CONNS" "$SIZE_MB")
        iso_quiet
        HAP_RES=$(run_cell hap   "$BUF" "$CONNS" "$SIZE_MB")
        iso_quiet
        NGX_RES=$(run_cell nginx "$BUF" "$CONNS" "$SIZE_MB")

        TCP_MED=$(echo "$TCP_RES" | awk '{print $1}')
        TCP_IQR=$(echo "$TCP_RES" | awk '{print $2}')
        TCP_STALL=$(echo "$TCP_RES" | awk '{print $3}')

        HAP_MED=$(echo "$HAP_RES" | awk '{print $1}')
        HAP_IQR=$(echo "$HAP_RES" | awk '{print $2}')
        HAP_STALL=$(echo "$HAP_RES" | awk '{print $3}')

        NGX_MED=$(echo "$NGX_RES" | awk '{print $1}')
        NGX_IQR=$(echo "$NGX_RES" | awk '{print $2}')
        NGX_STALL=$(echo "$NGX_RES" | awk '{print $3}')

        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$BUF" "$CONNS" "$SIZE_MB" \
            "$TCP_MED" "$TCP_IQR" "$TCP_STALL" \
            "$HAP_MED" "$HAP_IQR" "$HAP_STALL" \
            "$NGX_MED" "$NGX_IQR" "$NGX_STALL" >> "$TSV"

        printf '  tcp: %s / %s / %s\n' "$TCP_MED" "$TCP_IQR" "$TCP_STALL"
        printf '  hap: %s / %s / %s\n' "$HAP_MED" "$HAP_IQR" "$HAP_STALL"
        printf '  ngx: %s / %s / %s\n' "$NGX_MED" "$NGX_IQR" "$NGX_STALL"
    done
done

T1=$(date +%s)

# ---------- Pivot 输出 ----------
{
    echo "# Pivot 视图：每 BUF 一张表"
    echo "# 格式：CONNS | tcp / hap / ngx (med, MiB/s)"
    echo
    for BUF in $BUFS; do
        echo "### BUF=$BUF KiB ###"
        printf '%-8s %-12s %-12s %-12s\n' "CONNS" "tcp-lb" "haproxy" "nginx"
        for CONNS in $CONNS_LIST; do
            row=$(awk -F'\t' -v b="$BUF" -v c="$CONNS" '$1==b && $2==c {print $4, $7, $10}' "$TSV")
            tcp=$(echo "$row" | awk '{print $1}')
            hap=$(echo "$row" | awk '{print $2}')
            ngx=$(echo "$row" | awk '{print $3}')
            printf '%-8s %-12s %-12s %-12s\n' "$CONNS" "${tcp:-·}" "${hap:-·}" "${ngx:-·}"
        done
        echo
    done
} > "$PIVOT"

echo
echo "════════════════════════════════════════════════════════════"
echo "  完整数据（scaling.tsv）"
echo "════════════════════════════════════════════════════════════"
column -t -s $'\t' "$TSV"

echo
echo "════════════════════════════════════════════════════════════"
echo "  Pivot 视图"
echo "════════════════════════════════════════════════════════════"
cat "$PIVOT"

echo
echo "  total: $((T1 - T0))s"
echo "  tsv:   $TSV"
echo "  pivot: $PIVOT"
echo "  logs:  $OUT/"
