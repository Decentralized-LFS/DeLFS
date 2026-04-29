# Crash-Consistency Testing Artifact for DeLFS using CrashMonkey

## Path Configuration for Test Scripts

Before running CrashMonkey-DeLFS, you must update the hard-coded DeLFS-related paths to match your local installation path.

### 1. Update `CrashMonkey-DeLFS/code/harness/FsSpecific.cpp`

In `CrashMonkey-DeLFS/code/harness/FsSpecific.cpp`, modify the paths used in:

- `kDeLFSMkfsCommand = "<path where you installed DeLFS>/DeLFS/delfs-mkfs/mkfs/mkfs.delfs -f ";`
- `kDeLFSFsckCommand = "<path where you installed DeLFS>/DeLFS/delfs-mkfs/fsck/fsck.delfs ";`

---

### 2. Update test scripts

In the following files:

- `DeLFS-Create_delete-Test-Script.sh`
- `DeLFS-Checkpoint-Test-Script.sh`

please also modify the relevant path entries so they point to your local DeLFS installation, using the same format:

`<path where you installed DeLFS>/DeLFS/DeLFS/module_DeLFS/delfs.ko`

---

### Example

If your DeLFS directory is installed at:

`/home/username/workspace`

then the DeLFS-related paths should look like:

`/home/username/workspace/DeLFS/DeLFS/...`

---

#### Common
```$ sudo apt-get install -y git make gcc g++ libattr1-dev btrfs-progs f2fs-tools xfsprogs libelf-dev python3 python3-pip
$ python3 -m pip install progress progressbar
$ sudo apt install libattr1-dev
$ cd <CRASHMONKEY DIRECTORY>
$ make
$ mkdir /mnt/snapshot
```
#### Checkpoint
```
./DeLFS-Checkpoint-Test-Script.sh <DEVNUM> /* e.g., 5 (/dev/nvme5n1) */
```


#### Create_delete
```
./DeLFS-Create_delete-Test-Script.sh <DEVNUM> /* e.g., 5 (/dev/nvme5n1) */
```
