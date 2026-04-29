#!/bin/bash
set -e

# ------------------------------
# NVMe 번호 입력
# ------------------------------
if [ -z "$1" ]; then
    echo "Usage: $0 <nvme_number>"
    echo "Example: $0 6  → monitors nvme6n1"
    exit 1
fi

NVME_NUM="$1"
TARGET_DEV="nvme${NVME_NUM}n1"

# ------------------------------
# 설정
# ------------------------------
FIO_JOB="/home/syslab/workspace_hwan/DeLFS/script/gc/gc_30.fio"
LOG_DIR="/home/syslab/workspace_hwan/DeLFS/script/logs"
mkdir -p "$LOG_DIR"

TS=$(date +"%Y%m%d_%H%M%S")

DSTAT_LOG="$LOG_DIR/dstat_$TS.csv"
FIO_LOG="$LOG_DIR/fio_$TS.log"
SESSION="log_$TS"

echo "[INFO] Target device: $TARGET_DEV"
echo "[INFO] dstat log: $DSTAT_LOG"
echo "[INFO] fio log:   $FIO_LOG"

# ------------------------------
# 1) dstat 로깅 시작 (tmux)
# ------------------------------
tmux new-session -d -s "$SESSION"

tmux send-keys -t "$SESSION" "
dstat -d -D $TARGET_DEV --output $DSTAT_LOG 1
" C-m

echo "[INFO] dstat logging started (tmux session: $SESSION)"

# ------------------------------
# 2) FIO 실행 + summary 한 줄 저장
# ------------------------------
echo "[INFO] Running FIO..."

fio --status-interval=1 "$FIO_JOB" 2>&1 | grep "WRITE:" > "$FIO_LOG"

echo "[INFO] FIO finished."

# ------------------------------
# 3) dstat 로거 종료 (Ctrl+C)
# ------------------------------
tmux send-keys -t "$SESSION" C-c
sleep 1
tmux kill-session -t "$SESSION" 2>/dev/null || true

echo "[INFO] dstat logging stopped."
echo "[INFO] All logs saved to $LOG_DIR"

