#!/bin/bash
set -e

# ------------------------------
# 설정
# ------------------------------
FIO_JOB="/mnt/delfs/benchmark/fio/rand-write.fio"
LOG_DIR="/mnt/delfs/benchmark/scripts/logs"
mkdir -p "$LOG_DIR"

TS=$(date +"%Y%m%d_%H%M%S")
CSV="$LOG_DIR/slab_$TS.csv"
SESSION="slablog_$TS"

echo "[INFO] Output CSV: $CSV"

# ------------------------------
# 1) CSV 헤더 생성 (/proc/slabinfo 용)
# ------------------------------
echo "Timestamp,SlabName,ActiveObjs,NumObjs,ObjSize" > "$CSV"

# ------------------------------
# 2) tmux 로거 시작 (/proc/slabinfo 1초 간격 로깅)
# ------------------------------
tmux new-session -d -s "$SESSION"

tmux send-keys -t "$SESSION" "
while true; do
    TS2=\$(date '+%Y-%m-%d %H:%M:%S')

    # 헤더 2줄 건너뛰고, 모든 slab에 대해 1,2,3,4 필드 로깅
    awk -v t=\"\$TS2\" 'NR>2 {printf \"%s,%s,%s,%s,%s\n\", t,\$1,\$2,\$3,\$4}' /proc/slabinfo >> $CSV

    sleep 1
done
" C-m

echo "[INFO] Slab logging started: tmux session '$SESSION'"

# ------------------------------
# 3) FIO 실행
# ------------------------------
echo "[INFO] Running FIO..."
fio "$FIO_JOB"
echo "[INFO] FIO finished."

# ------------------------------
# 4) tmux 로거 종료
# ------------------------------
tmux send-keys -t "$SESSION" C-c
sleep 1
tmux kill-session -t "$SESSION" 2>/dev/null || true

echo "[INFO] Logging completed."
echo "[INFO] CSV saved to: $CSV"

