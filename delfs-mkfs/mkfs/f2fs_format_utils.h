/**
 * f2fs_format_utils.c
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * Dual licensed under the GPL or LGPL version 2 licenses.
 */
#include "f2fs_fs.h"

extern struct delfs_configuration c;

int delfs_trim_device(int, uint64_t);
int delfs_trim_devices(void);
int delfs_format_device(void);


