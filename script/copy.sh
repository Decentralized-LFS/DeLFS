#!/usr/bin/env bash
set -euo pipefail

SRC=/dev/nvme4n1p1
DST=/dev/nvme7n1p1

JOBS=64
BS=4M
BS_BYTES=$((4*1024*1024))

SIZE=$(blockdev --getsize64 "$SRC")
DST_SIZE=$(blockdev --getsize64 "$DST")

if [ "$DST_SIZE" -lt "$SIZE" ]; then
    echo "ERROR: destination is smaller than source"
    exit 1
fi

# 첫 JOBS-1개는 BS 단위로 딱 나누고, 마지막 job이 remainder 담당
BASE_CHUNK=$(( SIZE / JOBS ))
CHUNK=$(( (BASE_CHUNK / BS_BYTES) * BS_BYTES ))

if [ "$CHUNK" -eq 0 ]; then
    echo "ERROR: chunk became 0. Reduce JOBS or BS."
    exit 1
fi

pids=()

for i in $(seq 0 $((JOBS - 1))); do
    if [ "$i" -lt $((JOBS - 1)) ]; then
        OFF=$(( i * CHUNK ))
        LEN=$CHUNK
    else
        OFF=$(( i * CHUNK ))
        LEN=$(( SIZE - OFF ))
    fi

    [ "$LEN" -le 0 ] && continue

    SKIP_BLOCKS=$(( OFF / BS_BYTES ))
    SEEK_BLOCKS=$SKIP_BLOCKS

    if [ "$i" -lt $((JOBS - 1)) ]; then
        COUNT_BLOCKS=$(( LEN / BS_BYTES ))

        echo "job=$i off=$OFF len=$LEN skip_blocks=$SKIP_BLOCKS count_blocks=$COUNT_BLOCKS"

        dd if="$SRC" of="$DST" \
            bs="$BS" \
            iflag=direct,fullblock \
            oflag=direct \
            skip="$SKIP_BLOCKS" seek="$SEEK_BLOCKS" count="$COUNT_BLOCKS" \
            conv=notrunc status=progress &
    else
        # 마지막은 remainder 포함. B suffix 없이 하려면 bs=1로 tail 복사
        FULL_LEN=$(( (LEN / BS_BYTES) * BS_BYTES ))
        TAIL_LEN=$(( LEN - FULL_LEN ))

        if [ "$FULL_LEN" -gt 0 ]; then
            COUNT_BLOCKS=$(( FULL_LEN / BS_BYTES ))
            echo "job=$i(full) off=$OFF len=$FULL_LEN skip_blocks=$SKIP_BLOCKS count_blocks=$COUNT_BLOCKS"

            dd if="$SRC" of="$DST" \
                bs="$BS" \
                iflag=direct,fullblock \
                oflag=direct \
                skip="$SKIP_BLOCKS" seek="$SEEK_BLOCKS" count="$COUNT_BLOCKS" \
                conv=notrunc status=progress &
            pids+=($!)
        fi

        if [ "$TAIL_LEN" -gt 0 ]; then
            TAIL_OFF=$(( OFF + FULL_LEN ))
            echo "job=$i(tail) off=$TAIL_OFF len=$TAIL_LEN"

            dd if="$SRC" of="$DST" \
                bs=1 \
                skip="$TAIL_OFF" seek="$TAIL_OFF" count="$TAIL_LEN" \
                conv=notrunc status=progress &
            pids+=($!)
        fi

        continue
    fi

    pids+=($!)
done

rc=0
for pid in "${pids[@]}"; do
    wait "$pid" || rc=1
done

sync
exit "$rc"
