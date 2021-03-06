/* Copyright (c) 2010 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Devfs: filesystem interfaces to devices.  For now, we just create the
 * needed/discovered devices in KFS in its /dev/ folder.  In the future, we
 * might want to do something like nodes like other Unixes. */

#pragma once

#include <vfs.h>
#include <kfs.h>

void devfs_init(void);
struct file *make_device(char *path, int mode, int type,
                         struct file_operations *fop);

/* Generic device (block or char) file ops.  Both of these are dummies that say
 * the device can't support the operation. */
int dev_mmap(struct file *file, struct vm_region *vmr);
int dev_c_llseek(struct file *file, off64_t offset, off64_t *ret, int whence);

/* Exporting these for convenience (process creation) */
extern struct file *dev_stdin, *dev_stdout, *dev_stderr;
