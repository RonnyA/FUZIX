/*
 *	OSI hard disk controller. Compared with modern systems this is
 *	fairly primitive. Note that we don't currently deal with bad
 *	track mapping. The disk format supports it but the tools need
 *	some tweaking (fsck in particular) to understand inode 0 is a "file"
 *	of all the bad blocks. We do the low level parts in asm because
 *	of the weird 'make visible'. This also needs care with interrupt
 *	disables.
 *
 *	Only handles single drive for now
 */

#include <kernel.h>

/* The physical media runs in 3584 byte blocks (7 Fuzix sectors) so we
   have to do math. It also depends on drive type. For the moment assume
   a CD-36 */
#define SPT	5		/* of 3584 bytes */
#define HPT	6

struct {
	uint8_t head;
	uint8_t cyl;
	uint16_t start;
	uint16_t end;
	uint8_t *dptr;
} hddat;			/* Passes request to asm helper */

/*
0	0010  0725
1	0750  0E65
2	0E90  15A5
3	15D0  1CE5
4	1D10  2425
*/

/* Will need extending for the biggest drives ? */

uint16_t secst[] = {
	0x0010,
	0x0750,
	0x0E90,
	0x15D0,
	0x1D10
};

uint16_t secend[] = {
	0x0725,
	0x0E65,
	0x15A5,
	0x1CE5,
	0x2425
};

#define hdbuf ((volatile uint8_t *)(0xE000))

static unsigned curdirty;
static unsigned curblock = 0xFFFF;

static unsigned map_block(register unsigned block, unsigned off)
{
	/* TODO: fast path 'next block' case */
	unsigned sec, head, cyl;

	sec = block % SPT;
	block /= SPT;
	head = block % HPT;
	cyl = block / HPT;

	hddat.head = head | (cyl >> 8);
	hddat.cyl = cyl;
	hddat.start = secst[sec] + off;
	hddat.end = secend[sec] + off;

	kprintf("M: %u %u %u\n", cyl, head, sec);
	kprintf("MAP: %u-%u\n", hddat.start, hddat.end);
	return sec;
}

/* Older drives use a bump on carry 8bit checksum. later ones oddly
   use a weaker 8bit sum : TODO older */
static uint8_t checksum(register uint8_t *p, register unsigned n)
{
	register uint16_t sum = 0;
	while (n--) {
		sum += *p++;
#ifdef OLD_CSUM
		if (sum & 0x0100) {
		      sum &= 0xFF:
			sum++;
		}
#endif
	}
	return sum;
}

static uint16_t checksum16(register uint8_t *p, register unsigned n)
{
	register uint16_t sum = 0;
	while (n--)
		sum += *p;
	return sum;
}

static unsigned read_block(unsigned block)
{
	register uint16_t csum;
	register unsigned irq;
	unsigned tries = 0;

	while(tries++ < 5) {
		kprintf("rb try %u\n", tries - 1);
		/* Do we need to offset the start/end values ? */
		map_block(block, 3);
		irq = di();
		if (osihd_read() == 1) {
			irqrestore(irq);
			/* TODO: check right cyl low */
			csum = checksum(hdbuf + 0x11, 5);
			if (csum != hdbuf[0x17]) {
				kprintf("osihd: bad header checksum.\n");
				continue;
			}
			csum = checksum16(hdbuf + 0x18, 3584);
			if (csum != hdbuf[0xE18] || (csum >> 8) != hdbuf[0xE19]) {
				kprintf("osihd: bad data checksum.\n");
				continue;
			}
			return 1;
		}
	}
	irqrestore(irq);
	kprintf("osihd: disk read error\n");
	return 0;
}

static unsigned write_block(unsigned block)
{
	register uint16_t sec = map_block(block, 0);
	register irqflags_t irq;
	unsigned tries = 0;

	while(tries++ < 5) {
		hdbuf[0x10] = 0xA1;
		hdbuf[0x11] = 0x00;
		hdbuf[0x12] = 0x00;
		hdbuf[0x13] = hddat.cyl;
		hdbuf[0x14] = hddat.head & 0x7F;
		hdbuf[0x15] = sec;
		hdbuf[0x16] = 0x00;
		hdbuf[0x17] = checksum(hdbuf + 0x11, 5);
		sec = checksum16(hdbuf + 0x18, 3584);
		hdbuf[0xE18] = sec;
		hdbuf[0xE19] = sec >> 8;

		/* Should move this into the asm as we can re-enable interrupts
		   during the seek and maybe copy wait */
		irq = di();
		if (osihd_write() == 1) {
			irqrestore(irq);
			return 1;
		}
	}
	irqrestore(irq);
	/* TODO: dig out status somewhere */
	kprintf("osihd: error\n");
	return 0;
}

/* Would be nice to make the user/kernel disk read write cases
   scan the buffer cache for other dirty blocks for this block
   and if so write them out at the same time. TODO */
static void flush_block(void)
{
	if (curdirty)
		write_block(curblock);
	curdirty = 0;
}

int osihd_xfer(uint_fast8_t dev, bool is_read, uint32_t lba, uint8_t *dptr)
{
	/* Worst case is a CD74 and this will fit into a 16bit value */
	/* TODO: fast path next lba case */
	unsigned block = lba / SPT;
	unsigned boff = lba % SPT;
	kprintf("osihd_xfer %u\n", (unsigned)lba);
	if (block != curblock) {
		flush_block();
		if (read_block(block) == 0) {
			curblock = 0xFFFF;
			return 0;
		}
		curblock = block;
	}

	if (is_read)
		osihd_copy_out(boff, dptr);
	else {
		osihd_copy_in(boff, dptr);
		curdirty = 1;
	}
	/* TODO: if O_SYNC should probably force the write ? */
	return 1;
}

/* Should add geometry ioctls ? */
int osihd_ioctl(uint_fast8_t dev, uarg_t request, char *unused)
{
	if (request != BLKFLSBUF)
		return -1;
	flush_block();
	return 0;
}

void osihd_install(void)
{
	kputs("osihd: ");
	td_register(0, osihd_xfer, osihd_ioctl, 1);
}
