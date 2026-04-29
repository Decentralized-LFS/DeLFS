#sudo mkfs -t f2fs -f /dev/nvme0n1
make -j12 && sudo ./mkfs/mkfs.f2fs -f /dev/md127
#make -j12 && sudo make -j12 install && sudo mkfs -t f2fs -f /dev/md127
