usage() {
    echo "사용법: $0 <nvme-number>"
    echo "예시: $0 0   # -> /dev/nvme0n1p1 을 사용"
    exit 1
}


if [ -z "$1" ]; then
	usage
fi

DEVNUM="$1"

<<<<<<< HEAD
DEVICE="/dev/nvme${DEVNUM}n1p1"
#DEVICE="/dev/md${DEVNUM}"
#DEVICE="/dev/nvme${DEVNUM}n1"
=======
#DEVICE="/dev/nvme${DEVNUM}n1p1"
#DEVICE="/dev/md${DEVNUM}"
DEVICE="/dev/nvme${DEVNUM}n1"
>>>>>>> 9566225a9 (priori3 myfileserver-4 <- 1%)

if [ ! -b "$DEVICE" ]; then
	echo "error: we can't find device($DEVICE)"
	exit 1
fi

sudo mkfs.f2fs -f "$DEVICE"
if [ $? -ne 0 ]; then
	echo "error: we failed to create f2fs"
	exit 1
fi

echo "success mkfs to $DEVICE"
