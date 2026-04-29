# SPDX-License-Identifier: GPL-2.0
obj-m += delfs.o

delfs-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o
delfs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o
delfs-y		+= shrinker.o extent_cache.o sysfs.o


delfs-$(CONFIG_F2FS_STAT_FS) += debug.o
delfs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
delfs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
delfs-$(CONFIG_FS_VERITY) += verity.o
delfs-$(CONFIG_F2FS_FS_COMPRESSION) += compress.o
delfs-$(CONFIG_F2FS_IOSTAT) += iostat.o

ccflags-y += -g

#delfs-m += debug.o
#delfs-m += xattr.o
#delfs-m += acl.o
#delfs-m += verity.o
#delfs-m += compress.o
#delfs-$(CONFIG_F2FS_IOSTAT) += iostat.o

#delfs-y += fs/open.o fs/namei.o calc/calclock.o
delfs-y += calc/calclock.o fs/namespace.o fs/super.o mm/filemap.o mm/page-writeback.o \
	fs/fs-writeback.o fs/sync.o oldinv.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

