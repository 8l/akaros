#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <parlib/parlib.h>
#include <parlib/vcore.h>


#define SPEED_WRITE				1
#define SPEED_NULL				2
#define SPEED_FASTCALL			3
#define SPEED_NOTLS_NULL		4
#define SPEED_NOTLS_AND_RETURN	5
#define SPEED_NOTLS_AND_PCPUI	6
#define SPEED_NOTLS_NOTRACE		7
#define SPEED_NOTLS_NOFINISH	8
#define SPEED_NOTLS_NOSYSCALL	9

const char *tests[] = {
	"NOTEST",
	"SPEED_WRITE",
	"SPEED_NULL",
	"SPEED_FASTCALL",
	"SPEED_NOTLS_NULL",
	"SPEED_NOTLS_AND_RETURN",
	"SPEED_NOTLS_AND_PCPUI",
	"SPEED_NOTLS_NOTRACE",
	"SPEED_NOTLS_NOFINISH",
	"SPEED_NOTLS_NOSYSCALL",
};

static void usage(void)
{
	printf("Need a test (integer), options are:\n");
	for (int i = 1; i < sizeof(tests) / sizeof(char*); i++)
		printf("\t%d: %s\n", i, tests[i]);
	exit(-1);
}

int main(int argc, char *argv[])
{
	int iter = 10000, test;
	struct timeval start_time, end_time;
	double usec, rate;
	int fd;
	void *tls_desc = get_tls_desc();
	struct syscall sysc = {0};
	sysc.num = SYS_null;
	sysc.flags = (void*)SC_DONE;

	if (argc < 2)
		usage();
	test = atoi(argv[1]);

	fd = open("/dev/null", O_WRONLY);
	if (fd < 0)
		exit(1);
	gettimeofday(&start_time, NULL);

	switch (test) {
	case (SPEED_WRITE):
		for (int i = 0; i < iter; i++)
			write(fd, &fd, 1);
		break;
	case (SPEED_NULL):
		for (int i = 0; i < iter; i++)
			sys_null();
		break;
	case (SPEED_FASTCALL):
		for (int i = 0; i < iter; i++) {
			long dummy;
			asm volatile ("syscall" : "=D"(dummy), "=S"(dummy) /* clobber D, S */
			                        : "D"(0xf0f0000000000002), "S"(0)
			                        : "rax", "r11", "rcx", "rdx", "memory");
		}
		break;
	case (SPEED_NOTLS_NULL):
		for (int i = 0; i < iter; i++)
			__ros_arch_syscall((long)&sysc + 0x8000000000000000, 1);
		break;
	case (SPEED_NOTLS_AND_RETURN):
		for (int i = 0; i < iter; i++)
			__ros_arch_syscall((long)&sysc + 0xc000000000000000, 1);
		break;
	case (SPEED_NOTLS_AND_PCPUI):
		for (int i = 0; i < iter; i++)
			__ros_arch_syscall((long)&sysc + 0xa000000000000000, 1);
		break;
	case (SPEED_NOTLS_NOTRACE):
		for (int i = 0; i < iter; i++)
			__ros_arch_syscall((long)&sysc + 0x9000000000000000, 1);
		break;
	case (SPEED_NOTLS_NOFINISH):
		for (int i = 0; i < iter; i++)
			__ros_arch_syscall((long)&sysc + 0x8800000000000000, 1);
		break;
	case (SPEED_NOTLS_NOSYSCALL):
		for (int i = 0; i < iter; i++)
			__ros_arch_syscall((long)&sysc + 0x8400000000000000, 1);
		break;
	default:
		printf("Unknown test %d\n", test);
		exit(-1);
	}
	

// 700, raw sysc in userspace, return with finish in run_local_sysc
//	for (i = 0; i < iter; i++) {
//		__ros_arch_syscall((long)&sysc, 1);
//	}

// 672, raw sysc in userspace, return *without* finish in run_local_sysc
//	for (i = 0; i < iter; i++) {
//		__ros_arch_syscall((long)&sysc, 1);
//	}

// 568, raw sysc in userspace, return without finish sysenter_callwrapper, after
// copying the ctx into pcpui
//	for (i = 0; i < iter; i++) {
//		__ros_arch_syscall((long)&sysc, 1);
//	}

// 495, raw sysc in userspace, return without finish in sysenter_callwrapper,
// without copying the ctx (just return and unwind)
//	for (i = 0; i < iter; i++) {
//		__ros_arch_syscall((long)&sysc, 1);
//	}
	// same deal, but did 10 extra rdmsr of fs_base (with mov rdmsr shl or)
	// 930, then 847 for the rest.
	// 	diff of 350, so 35ns per rdmsr bit.  damn.......

// 150ns, raw sysc in userspace, return without finish in sysenter_callwrapper,
// no FS GS saving/restoring (just swapgs)
// 		10x extra swapgs: 414ns.  264 diff, 26ns per.
//	for (i = 0; i < iter; i++) {
//		__ros_arch_syscall((long)&sysc, 1);
//	}

// ~121.50 ns avg, 10000 iter.  syscall, with a little MSR work to set TLS
// 			728 ns avg, 10000 iter.  write fsbase, with 10x wrmsr in the kernel
// 				60 ns per wrmsr.  damn.......
//	for (i = 0; i < iter; i++) {
//		__fastcall_setfsbase((uintptr_t)tls_desc);
//	}

// 57 ns avg, 10000 iter.  raw syscall, cmp, branch, return (hacked trapentry.S)
//	for (i = 0; i < iter; i++) {
//		long dummy;
//		asm volatile ("syscall" : "=D"(dummy), "=S"(dummy) /* clobber D, S */
//		                        : "D"(0xf0f0000000000002), "S"(0)
//		                        : "rax", "r11", "rcx", "rdx", "memory");
//	}

	gettimeofday(&end_time, NULL);
	usec = end_time.tv_usec - start_time.tv_usec;
	usec += (end_time.tv_sec - start_time.tv_sec) * 1000000;

	rate = (double)iter / usec;

	printf("Test %s\n", tests[test]);
	printf("avg time=%.2lfns, rate = %.2lfM/s\n", 1000.0 * usec/(double)iter,
	       rate);
}

/* 1379433.066804 == akaros */
/* Linux 3.5.0-23-generic: avg time=191.81ns, rate = 5.21M/s */
