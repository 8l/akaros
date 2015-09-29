#include <stdio.h> 
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <parlib/arch/arch.h>
#include <parlib/ros_debug.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <ros/syscall.h>
#include <sys/mman.h>
#include <vmm/coreboot_tables.h>
#include <ros/vmm.h>
#include <vmm/virtio.h>
#include <vmm/virtio_mmio.h>
#include <vmm/virtio_ids.h>

/* this test will run the "kernel" in the negative address space. We hope. */
int *mmap_blob;
unsigned long long stack[1024];
volatile int shared = 0;
volatile int quit = 0;
int mcp = 1;

#define MiB 0x100000u
#define GiB (1u<<30)
#define GKERNBASE (16*MiB)
#define KERNSIZE (128*MiB+GKERNBASE)
uint8_t _kernel[KERNSIZE];

unsigned long long *p512, *p1, *p2m;

pthread_t *my_threads;
void **my_retvals;
int nr_threads = 3;
char *line, *consline, *outline;
struct scatterlist iov[32];
unsigned int inlen, outlen, conslen;
int debug = 1;
/* unlike Linux, this shared struct is for both host and guest. */
//	struct virtqueue *constoguest = 
//		vring_new_virtqueue(0, 512, 8192, 0, inpages, NULL, NULL, "test");
volatile int gaveit = 0, gotitback = 0;
struct scatterlist out[] = { {NULL, sizeof(outline)}, };
struct scatterlist in[] = { {NULL, sizeof(line)}, };
uint64_t virtio_mmio_base = 0x100000000;

void consout(void *arg)
{
	struct virtio_threadarg *a = arg;
	struct virtqueue *v = a->dev->virtio;
	fprintf(stderr, "talk thread ..\n");
	uint16_t head;
	uint32_t vv;
	int i;
	int num;
	printf("Sleep 15 seconds\n");
	uthread_sleep(15);
	printf("----------------------- TT a %p\n", a);
	printf("talk thread ttargs %x v %x\n", a, v);
	
	for(num = 0;;num++) {
		/* host: use any buffers we should have been sent. */
		head = wait_for_vq_desc(v, iov, &outlen, &inlen);
		if (debug)
			printf("vq desc head %d, gaveit %d gotitback %d\n", head, gaveit, gotitback);
		for(i = 0; debug && i < outlen + inlen; i++)
			printf("v[%d/%d] v %p len %d\n", i, outlen + inlen, iov[i].v, iov[i].length);
		/* host: if we got an output buffer, just output it. */
		for(i = 0; i < outlen; i++) {
			num++;
			printf("Host:%s:\n", (char *)iov[i].v);
		}
		
		if (debug)
			printf("outlen is %d; inlen is %d\n", outlen, inlen);
		/* host: fill in the writeable buffers. */
		/* why we're getting these I don't know. */
		for (i = outlen; i < outlen + inlen; i++) {
			if (debug) fprintf(stderr, "send back empty writeable");
			iov[i].length = 0;
		}
		if (debug) printf("call add_used\n");
		/* host: now ack that we used them all. */
		add_used(v, head, outlen+inlen);
		if (debug) printf("DONE call add_used\n");
	}
	fprintf(stderr, "All done\n");
}

void consin(void *arg)
{

	fprintf(stderr, "consinput; nothing to do\n");
#if 0
	struct ttargs *a = arg;
	void *v = a->virtio;
	fprintf(stderr, "talk thread ..\n");
	uint16_t head;
	uint32_t vv;
	int i;
	int num;
	printf("Sleep 15 seconds\n");
	uthread_sleep(15);
	printf("----------------------- TT a %p\n", a);
	printf("talk thread ttargs %x v %x\n", a, v);
	
	if (debug) printf("Spin on console being read, print num queues, halt\n");
	while ((vv = read32(v+VIRTIO_MMIO_DRIVER_FEATURES)) == 0) {
		printf("no ready ... \n");
		if (debug) {
			dumpvirtio_mmio(stdout, v);
		}
		printf("sleep 1 second\n");
		uthread_sleep(1);
	}
	if (debug)printf("vv %x, set selector %x\n", vv, read32(v + VIRTIO_MMIO_DRIVER_FEATURES_SEL));
	if (debug) printf("loop forever");
	while (! quit)
		;
	for(num = 0;;num++) {
		/* host: use any buffers we should have been sent. */
		head = wait_for_vq_desc(guesttocons, iov, &outlen, &inlen);
		if (debug)
			printf("vq desc head %d, gaveit %d gotitback %d\n", head, gaveit, gotitback);
		for(i = 0; debug && i < outlen + inlen; i++)
			printf("v[%d/%d] v %p len %d\n", i, outlen + inlen, iov[i].v, iov[i].length);
		/* host: if we got an output buffer, just output it. */
		for(i = 0; i < outlen; i++) {
			num++;
			printf("Host:%s:\n", (char *)iov[i].v);
		}
		
		if (debug)
			printf("outlen is %d; inlen is %d\n", outlen, inlen);
		/* host: fill in the writeable buffers. */
		for (i = outlen; i < outlen + inlen; i++) {
			/* host: read a line. */
			memset(consline, 0, 128);
			if (1) {
				if (fgets(consline, 4096-256, stdin) == NULL) {
					exit(0);
				} 
				if (debug) printf("GOT A LINE:%s:\n", consline);
			} else {
				sprintf(consline, "hi there. %d\n", i);
			}
			memmove(iov[i].v, consline, strlen(consline)+ 1);
			iov[i].length = strlen(consline) + 1;
		}
		if (debug) printf("call add_used\n");
		/* host: now ack that we used them all. */
		add_used(guesttocons, head, outlen+inlen);
		if (debug) printf("DONE call add_used\n");
	}
#endif
	fprintf(stderr, "All done\n");

}

struct vqdev vqs[] = {
	{"consout", VIRTIO_ID_CONSOLE, 0, consout, (void *)0},
	{"consin", VIRTIO_ID_CONSOLE, 1, consin, (void *)0},
};

int main(int argc, char **argv)
{
	uint64_t virtiobase = 0x100000000ULL;
	struct vmctl vmctl;
	int amt;
	int vmmflags = VMM_VMCALL_PRINTF;
	uint64_t entry = 0x1000000, kerneladdress = 0x1000000;
	int nr_gpcs = 1;
	int fd = open("#c/vmctl", O_RDWR), ret;
	void * x;
	int kfd = -1;
	static char cmd[512];
	void *coreboot_tables = (void *) 0x1165000;

	// mmap is not working for us at present.
	if ((uint64_t)_kernel > GKERNBASE) {
		printf("kernel array @%p is above , GKERNBASE@%p sucks\n", _kernel, GKERNBASE);
		exit(1);
	}
	memset(_kernel, 0, sizeof(_kernel));

	if (fd < 0) {
		perror("#cons/sysctl");
		exit(1);
	}
	argc--,argv++;
	// switches ...
	// Sorry, I don't much like the gnu opt parsing code.
	while (1) {
		if (*argv[0] != '-')
			break;
		switch(argv[0][1]) {
		case 'n':
			vmmflags &= ~VMM_VMCALL_PRINTF;
			break;
		default:
			printf("BMAFR\n");
			break;
		}
		argc--,argv++;
	}
	if (argc < 1) {
		fprintf(stderr, "Usage: %s vmimage [-n (no vmcall printf)] [coreboot_tables [loadaddress [entrypoint]]]\n", argv[0]);
		exit(1);
	}
	if (argc > 1)
		coreboot_tables = (void *) strtoull(argv[1], 0, 0);
	if (argc > 2)
		kerneladdress = strtoull(argv[2], 0, 0);
	if (argc > 3)
		entry = strtoull(argv[3], 0, 0);
	kfd = open(argv[0], O_RDONLY);
	if (kfd < 0) {
		perror(argv[0]);
		exit(1);
	}
	// read in the kernel.
	x = (void *)kerneladdress;
	for(;;) {
		amt = read(kfd, x, 1048576);
		if (amt < 0) {
			perror("read");
			exit(1);
		}
		if (amt == 0) {
			break;
		}
		x += amt;
	}
	fprintf(stderr, "Read in %d bytes\n", x-kerneladdress);

	fprintf(stderr, "Run with %d cores and vmmflags 0x%x\n", nr_gpcs, vmmflags);
	if (ros_syscall(SYS_setup_vmm, nr_gpcs, vmmflags, 0, 0, 0, 0) != nr_gpcs) {
		perror("Guest pcore setup failed");
		exit(1);
	}
	/* blob that is faulted in from the EPT first.  we need this to be in low
	 * memory (not above the normal mmap_break), so the EPT can look it up.
	 * Note that we won't get 4096.  The min is 1MB now, and ld is there. */
	mmap_blob = mmap((int*)4096, PGSIZE, PROT_READ | PROT_WRITE,
	                 MAP_ANONYMOUS, -1, 0);
	if (mmap_blob == MAP_FAILED) {
		perror("Unable to mmap");
		exit(1);
	}

	mcp = 1;
	if (mcp) {
		my_threads = malloc(sizeof(pthread_t) * nr_threads);
		my_retvals = malloc(sizeof(void*) * nr_threads);
		if (!(my_retvals && my_threads))
			perror("Init threads/malloc");

		pthread_can_vcore_request(FALSE);	/* 2LS won't manage vcores */
		pthread_need_tls(FALSE);
		pthread_mcp_init();					/* gives us one vcore */
		vcore_request(nr_threads - 1);		/* ghetto incremental interface */
		for (int i = 0; i < nr_threads; i++) {
			x = __procinfo.vcoremap;
			printf("%p\n", __procinfo.vcoremap);
			printf("Vcore %d mapped to pcore %d\n", i,
			    	__procinfo.vcoremap[i].pcoreid);
		}
	}

	ret = syscall(33, 1);
	if (ret < 0) {
		perror("vm setup");
		exit(1);
	}
	ret = posix_memalign((void **)&p512, 4096, 3*4096);
	printf("memalign is %p\n", p512);
	if (ret) {
		perror("ptp alloc");
		exit(1);
	}
	p1 = &p512[512];
	p2m = &p512[1024];
	uint64_t kernbase = 0; //0xffffffff80000000;
	uint64_t highkernbase = 0xffffffff80000000;
	p512[PML4(kernbase)] = (unsigned long long)p1 | 7;
	p1[PML3(kernbase)] = /*0x87; */(unsigned long long)p2m | 7;
	p512[PML4(highkernbase)] = (unsigned long long)p1 | 7;
	p1[PML3(highkernbase)] = /*0x87; */(unsigned long long)p2m | 7;
#define _2MiB (0x200000)
	int i;
	for (i = 0; i < 512; i++) {
		p2m[PML2(kernbase + i * _2MiB)] = 0x87 | i * _2MiB;
	}

	kernbase >>= (0+12);
	kernbase <<= (0 + 12);
	uint8_t *kernel = (void *)GKERNBASE;
	//write_coreboot_table(coreboot_tables, ((void *)VIRTIOBASE) /*kernel*/, KERNSIZE + 1048576);
	hexdump(stdout, coreboot_tables, 512);
	printf("kernbase for pml4 is 0x%llx and entry is %llx\n", kernbase, entry);
	printf("p512 %p p512[0] is 0x%lx p1 %p p1[0] is 0x%x\n", p512, p512[0], p1, p1[0]);
	vmctl.command = REG_RSP_RIP_CR3;
	vmctl.cr3 = (uint64_t) p512;
	vmctl.regs.tf_rip = entry;
	vmctl.regs.tf_rsp = (uint64_t) &stack[1024];
	if (mcp) {
		/* set up virtio bits, which depend on threads being enabled. */
		register_virtio_mmio(vqs, 2, virtio_mmio_base);
	}
	printf("threads started\n");
	printf("Writing command :%s:\n", cmd);

	ret = write(fd, &vmctl, sizeof(vmctl));
	if (ret != sizeof(vmctl)) {
		perror(cmd);
	}
	while (1) {
		void showstatus(FILE *f, struct vmctl *v);
		int c;
		vmctl.command = REG_RIP;
		printf("RESUME?\n");
		c = getchar();
		if (c == 'q')
			break;
		printf("RIP %p, shutdown 0x%x\n", vmctl.regs.tf_rip, vmctl.shutdown);
		showstatus(stdout, &vmctl);
		// this will be in a function, someday.
		// A rough check: is the GPA 
		if ((vmctl.shutdown == 5/*EXIT_REASON_EPT_VIOLATION*/) && ((vmctl.gpa & ~0xfffULL) == virtiobase)) {
			printf("DO SOME VIRTIO\n");
			virtio_mmio(&vmctl);
			vmctl.shutdown = 0;
			vmctl.gpa = 0;
			vmctl.command = REG_ALL;
		}
		printf("NOW DO A RESUME\n");
		ret = write(fd, &vmctl, sizeof(vmctl));
		if (ret != sizeof(vmctl)) {
			perror(cmd);
		}
	}

	printf("shared is %d, blob is %d\n", shared, *mmap_blob);

	quit = 1;
	for (int i = 0; i < nr_threads-1; i++) {
		int ret;
		if (pthread_join(my_threads[i], &my_retvals[i]))
			perror("pth_join failed");
		printf("%d %d\n", i, ret);
	}

	return 0;
}
