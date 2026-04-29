#!/bin/bash

usage() {
    echo "사용법: $0 <nvme-number>"
    echo "예시: $0 0"
    exit 1
}

if [ -z "$1" ]; then
    usage
fi

DEVNUM="$1"

DEVICE="/dev/nvme${DEVNUM}n1"
PARTITION="/dev/nvme${DEVNUM}n1p1"

if [ ! -b "$PARTITION" ]; then
    echo "error: we can't find partition($PARTITION)"
    exit 1
fi

TEST_SO="tests/create_delete.so"
LOG_FILE="create_delete_test_delfs.log"

# -----------------------------------------------------------------------------
# 모듈 로드 등 기존 명령어들
# -----------------------------------------------------------------------------
sudo modprobe lz4hc_compress
sudo modprobe lz4_compress

sudo rmmod delfs 2>/dev/null || true
sudo insmod /home/syslab/workspace_hwan/DeLFS/DeLFS/module_DeLFS/delfs.ko

echo 0 | sudo tee /proc/sys/kernel/randomize_va_space > /dev/null

sudo umount /mnt/test 2>/dev/null || true

cd build || exit 1

echo "=================================================="
echo "Running test: create_delete (delfs)"
echo "Test file    : $TEST_SO"
echo "Log file     : $LOG_FILE"
echo "=================================================="

sudo ./c_harness -v -P \
  -f "$DEVICE" \
  -d /dev/cow_ram0 \
  -t delfs \
  -e 2097152 \
  "$TEST_SO" 2>&1 | tee "$LOG_FILE"

RET=${PIPESTATUS[0]}
if [ "$RET" -ne 0 ]; then
    echo "ERROR: create_delete (delfs) failed with exit code $RET"
    exit "$RET"
fi

echo "create_delete (delfs) test completed successfully."
