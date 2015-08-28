/* Copyright (c) 2015 Google Inc.
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * 64 bit KPT helpers */

#ifndef ROS_INC_VMM_H
#define ROS_INC_VMM_H

#define	VMM_VMCALL_PRINTF	0x1	/* Enable VMCALL output console hack */

#define VMM_ALL_FLAGS	(VMM_VMCALL_PRINTF)

enum {
	RESUME,
	REG_RSP_RIP_CR3,
	REG_RIP,
	REG_ALL,
};

/* eventually, this is a system call. For now, it's #c/vmctl.
 * You fill in the blanks, and write the struct to #c/vmctl.
 * On return, i.e. vmexit, it's updated with the new values.
 */
struct vmctl {
	uint64_t command;
	uint64_t cr3;
	uint64_t gva;
	uint64_t gpa;
	uint64_t exit_qual;
	uint64_t shutdown;
	uint64_t ret_code;
	uint64_t core;
	uint32_t interrupt;
	uint32_t intrinfo1;
	uint32_t intrinfo2;
	struct hw_trapframe regs;
};

char *regname(uint8_t reg);
int decode(struct vmctl *v, uint64_t *gpa, uint8_t *destreg, uint64_t **regp, int *store, int *size, int *advance);
int io(struct vmctl *v);

#endif /* ROS_INC_VMM_H */
