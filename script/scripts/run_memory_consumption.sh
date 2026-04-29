#!/bin/bash
set -e

# ------------------------------
# 설정
# ------------------------------
FIO_JOB="/mnt/delfs/benchmark/fio/rand-write.fio"
LOG_DIR="/mnt/delfs/benchmark/scripts/logs"
mkdir -p "$LOG_DIR"

TS=$(date +"%Y%m%d_%H%M%S")
CSV="$LOG_DIR/memory_$TS.csv"
SESSION="memlog_$TS"

echo "[INFO] Output CSV: $CSV"

# ------------------------------
# 1) CSV 헤더 생성
# ------------------------------
echo "Timestamp,MemTotal,MemFree,ActiveAnon,InactiveAnon,ActiveFile,InactiveFile,Slab" > "$CSV"

# ------------------------------
# 2) tmux 로거 시작
# ------------------------------
tmux new-session -d -s "$SESSION"

tmux send-keys -t "$SESSION" "
while true; do
    TS2=\$(date '+%Y-%m-%d %H:%M:%S')
    MT=\$(grep MemTotal /proc/meminfo | awk '{print \$2}')
    MF=\$(grep MemFree /proc/meminfo | awk '{print \$2}')
    AA=\$(grep 'Active(anon)' /proc/meminfo | awk '{print \$2}')
    IA=\$(grep 'Inactive(anon)' /proc/meminfo | awk '{print \$2}')
    AF=\$(grep 'Active(file)' /proc/meminfo | awk '{print \$2}')
    IF=\$(grep 'Inactive(file)' /proc/meminfo | awk '{print \$2}')
    SL=\$(grep Slab /proc/meminfo | awk '{print \$2}')

    echo \"\$TS2,\$MT,\$MF,\$AA,\$IA,\$AF,\$IF,\$SL\" >> $CSV

    sleep 1
done
" C-m

echo "[INFO] Memory logging started: tmux session '$SESSION'"

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

