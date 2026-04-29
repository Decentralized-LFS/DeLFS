# SPDX-License-Identifier: GPL-2.0
obj-m += f3fs.o

f3fs-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o
f3fs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o
f3fs-y		+= shrinker.o extent_cache.o sysfs.o


f3fs-$(CONFIG_F2FS_STAT_FS) += debug.o
f3fs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
f3fs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
f3fs-$(CONFIG_FS_VERITY) += verity.o
f3fs-$(CONFIG_F2FS_FS_COMPRESSION) += compress.o
f3fs-$(CONFIG_F2FS_IOSTAT) += iostat.o

ccflags-y += -g

#f3fs-m += debug.o
#f3fs-m += xattr.o
#f3fs-m += acl.o
#f3fs-m += verity.o
#f3fs-m += compress.o
#f3fs-$(CONFIG_F2FS_IOSTAT) += iostat.o

f3fs-y += calc/calclock.o mm/filemap.o mm/page-writeback.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

