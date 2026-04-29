#!/bin/bash
set -e

usage() {
    echo "사용법: $0 <nvme-number>"
    echo "예시: $0 0   # -> /dev/nvme0n1p1 을 사용"
    exit 1
}


if [ -z "$1" ]; then
	usage
fi

DEVNUM="$1"

DEV="nvme${DEVNUM}n1"
STAT=/sys/block/$DEV/stat
JOB=/home/syslab/workspace_hwan/DeLFS/script/gc/gc_30.fio
#JOB=/home/syslab/workspace_hwan/DeLFS/script/gc/gc_2t_1.fio
#JOB=/home/syslab/workspace_hwan/DeLFS/script/gc/gc_2t_2.fio

read_stat() {
    awk '{print $3, $7}' "$STAT"
}

sync
read R0 W0 < <(read_stat)

fio "$JOB"

sync
read R1 W1 < <(read_stat)

DR=$((R1 - R0))
DW=$((W1 - W0))

RB=$((DR * 512))
WB=$((DW * 512))

awk -v rb="$RB" -v wb="$WB" '
BEGIN {
    printf("device=%s\n", "'"$DEV"'");
    printf("read_bytes=%d\n", rb);
    printf("write_bytes=%d\n", wb);
    printf("read_gib=%.2f\n", rb/1024/1024/1024);
    printf("write_gib=%.2f\n", wb/1024/1024/1024);
}'
