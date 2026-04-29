usage() {
    echo "사용법: $0 <nvme-number>"
    echo "예시: $0 7   # -> /dev/nvme7n1p1 을 사용"
    exit 1
}


if [ -z "$1" ]; then
	usage
fi

DEVNUM="$1"

DEVICE="/dev/nvme${DEVNUM}n1p1"

if [ ! -b "$DEVICE" ]; then
	echo "error: we can't find device($DEVICE)"
	exit 1
fi

sudo ./f2fs-tools/mkfs/mkfs.f2fs -f "$DEVICE"
if [ $? -ne 0 ]; then
	echo "error: we failed to create f2fs"
	exit 1
fi

echo "success mkfs to $DEVICE"
