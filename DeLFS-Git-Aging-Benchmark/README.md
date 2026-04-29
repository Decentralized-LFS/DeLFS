# Git-Benchmark



### 1. Configuration setting
```
$ mkdir -p /mnt/aged
$ mkdir -p /mnt/unaged
$ mkdir -p /mnt/git-benchmark
$ mount /dev/nvme<DEVNUM1>n1 /mnt/git-benchmark
$ cd /mnt/git-benchmark
$ git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
$ for i in $(seq 0 127); do cp -a linux "linux${i}"; done && rm -rf linux
```

### 2. Run scripts

```
cd <path where you installed DeLFS>/DeLFS/DeLFS-Git-Aging-Benchmark/
```

Usage: percore-delfs2.sh <THREADS> <TOTAL_PULLS> <PULLS_PER_TEST> <AGED_NVME_NUM> <UNAGED_NVME_NUM>
```
$ ./percore-delfs2.sh 128 1000 100 0 2
```
Usage: percore-f2fs2.sh <THREADS> <TOTAL_PULLS> <PULLS_PER_TEST> <AGED_NVME_NUM> <UNAGED_NVME_NUM>
```
$ ./percore-f2fs2.sh 128 1000 100 0 2
```
