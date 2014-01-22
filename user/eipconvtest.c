// INFERNO
#if 0
#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>
#endif

#include <stdio.h>
#include <sys/types.h>
#include <stdarg.h>
#include <string.h>

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long uint32_t;

enum {
	Isprefix = 16,
};

uint8_t prefixvals[256] = {
	[0x00] 0 | Isprefix,
	[0x80] 1 | Isprefix,
	[0xC0] 2 | Isprefix,
	[0xE0] 3 | Isprefix,
	[0xF0] 4 | Isprefix,
	[0xF8] 5 | Isprefix,
	[0xFC] 6 | Isprefix,
	[0xFE] 7 | Isprefix,
	[0xFF] 8 | Isprefix,
};

uint8_t v4prefix[16] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0xff, 0xff,
	0, 0, 0, 0
};

void hnputl(void *p, uint32_t v)
{
	uint8_t *a;

	a = p;
	a[0] = v >> 24;
	a[1] = v >> 16;
	a[2] = v >> 8;
	a[3] = v;
}

char *eipconv(int fmt, ...)
{
	va_list ap;
	static char buf[8 * 5];
	static char *efmt = "0x%.2lx0x%.2lx0x%.2lx0x%.2lx0x%.2lx0x%.2lx";
	static char *ifmt = "%d.%d.%d.%d";
	uint8_t *p, ip[16];
	uint32_t *lp;
	uint16_t s;
	int i, j, n, eln, eli;
	va_start(ap, fmt);
	switch (fmt) {
		case 'E':	/* Ethernet address */
			p = va_arg(ap, uint8_t *);
			snprintf(buf, sizeof(buf), efmt, p[0], p[1], p[2], p[3], p[4],
					 p[5]);
			break;
		case 'I':	/* Ip address */
			p = va_arg(ap, uint8_t *);
common:
			if (memcmp(p, v4prefix, 12) == 0)
				snprintf(buf, sizeof(buf), ifmt, p[12], p[13], p[14], p[15]);
			else {
				/* find longest elision */
				eln = eli = -1;
				for (i = 0; i < 16; i += 2) {
					for (j = i; j < 16; j += 2)
						if (p[j] != 0 || p[j + 1] != 0)
							break;
					if (j > i && j - i > eln) {
						eli = i;
						eln = j - i;
					}
				}

				/* print with possible elision */
				n = 0;
				for (i = 0; i < 16; i += 2) {
					if (i == eli) {
						n += snprintf(buf, sizeof(buf) + n, "::");
						i += eln;
						if (i >= 16)
							break;
					} else if (i != 0)
						n += snprintf(buf, sizeof(buf) + n, ":");
					s = (p[i] << 8) + p[i + 1];
					n += snprintf(buf, sizeof(buf) + n, "0x%x", s);
				}
			}
			break;
		case 'i':	/* v6 address as 4 longs */
			lp = va_arg(ap, uint32_t *);
			for (i = 0; i < 4; i++)
				hnputl(ip + 4 * i, *lp++);
			p = ip;
			goto common;
		case 'V':	/* v4 ip address */
			p = va_arg(ap, uint8_t *);
			snprintf(buf, sizeof(buf), ifmt, p[0], p[1], p[2], p[3]);
			break;
		case 'M':	/* ip mask */
			p = va_arg(ap, uint8_t *);

			/* look for a prefix mask */
			for (i = 0; i < 16; i++)
				if (p[i] != 0xff)
					break;
			if (i < 16) {
				if ((prefixvals[p[i]] & Isprefix) == 0)
					goto common;
				for (j = i + 1; j < 16; j++)
					if (p[j] != 0)
						goto common;
				n = 8 * i + (prefixvals[p[i]] & ~Isprefix);
			} else
				n = 8 * 16;

			/* got one, use /xx format */
			snprintf(buf, sizeof(buf), "/%d", n);
			break;
		default:
			strncpy(buf, "(eipconv)", sizeof(buf));
	}

	return buf;
}

uint8_t testvec[11][16] = {
	{0,0,0,0,0,0,0,0,0,0,0xff,0xff,1,3,4,5,} ,
	{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,} ,
	{0xff,0xff,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,} ,
	{0xff,0xff,0xff,0xc0,0,0,0,0,0,0,0,0,0,0,0,0,} ,
	{0xff,0xff,0xff,0xff,0xe0,0,0,0,0,0,0,0,0,0,0,0,} ,
	{0xff,0xff,0xff,0xff,0xff,0xf0,0,0,0,0,0,0,0,0,0,0,} ,
	{0xff,0xff,0xff,0xff,0xff,0xff,0xf8,0,0,0,0,0,0,0,0,0,} ,
	{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,} ,
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,} ,
	{0,0,0,0,0,0x11,0,0,0,0,0,0,0,0,0,0,} ,
	{0,0,0,0x11,0,0,0,0,0,0,0,0,0,0,0,0x12,} ,
};

void main(void)
{
	int i;
	for (i = 0; i < 11; i++)
		printf("%s\n", eipconv('I', &testvec[i]));

}