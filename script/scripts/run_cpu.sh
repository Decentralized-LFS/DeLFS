#!/bin/bash
set -e

# ------------------------------
# 설정
# ------------------------------
FIO_JOB="/mnt/delfs/benchmark/fio/rand-write.fio"
LOG_DIR="/mnt/delfs/benchmark/scripts/logs"
mkdir -p "$LOG_DIR"

TS=$(date +"%Y%m%d_%H%M%S")
CPU_LOG="$LOG_DIR/cpu_$TS.csv"
SESSION="cpulog_$TS"

echo "[INFO] CPU log file: $CPU_LOG"

# ------------------------------
# 1) tmux 로거 시작 (dstat CPU 로깅)
# ------------------------------
tmux new-session -d -s "$SESSION"

tmux send-keys -t "$SESSION" "
dstat -c --output $CPU_LOG 1
" C-m

echo "[INFO] CPU logging started: tmux session '$SESSION'"

# ------------------------------
# 2) FIO 실행
# ------------------------------
echo "[INFO] Running FIO..."
fio "$FIO_JOB"
echo "[INFO] FIO finished."

# ------------------------------
# 3) tmux 로거 종료
# ------------------------------
tmux send-keys -t "$SESSION" C-c
sleep 1
tmux kill-session -t "$SESSION" 2>/dev/null || true

echo "[INFO] CPU logging completed."
echo "[INFO] CSV saved to: $CPU_LOG"

