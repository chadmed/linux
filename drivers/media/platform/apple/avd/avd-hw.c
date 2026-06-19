#include "linux/dev_printk.h"
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "avd.h"

#define INST_OFF(idx) (0x1c8 + (idx * 4)) /* bytes of instructions written */
#define ADDR_LOW(idx) (0x150 + (idx * 4))
#define ADDR_HIGH(idx) (0x30c + (idx * 4))

#define INST_FIFO_CACHE_USED(slot) (0x5c + (slot * 4))
#define INST_FIFO_CACHE_CAPACITY(slot) (0x34 + (slot * 4))

/* sorry for all the "magic" numbers in this file */

#define w32(off, val) (avd_w32(ctrl, off, val))
#define m32(off, val) (avd_m32(ctrl, off, val))

int avd_boot(struct avd_dev *avd)
{
	u32 val;
	int ret;

	avd_w32(base, 0x1000000, 0xfff);
	/* dev_info_once(avd->dev, "booting hw version: %04x", avd_r32(ctrl, 0)); */

	avd_w32(wrap, 0x14, 1);
	avd_w32(wrap, 0x18, 0);

	avd_w32(wrap, 0x14, 0);

	memcpy_toio(avd->code, avd->fw->data, avd->fw->size);

	avd_w32(mbox, 0x08, 0xe);
	avd_w32(mbox, 0x10, 0x0);
	avd_w32(mbox, 0x48, 0x0);

	avd_w32(mbox, 0x50, 0x1);
	avd_w32(mbox, 0x68, 0x1);
	avd_w32(mbox, 0x5c, 0x1);
	avd_w32(mbox, 0x74, 0x1);

	avd_w32(mbox, 0x10, 0x2);
	avd_w32(mbox, 0x48, 0x8);
	avd_w32(mbox, 0x08, 0x1);

	/* wait for cm3 to boot */
	ret = readl_poll_timeout(avd->mbox + 0x90, val, val == 1, 10, 10000);
	if (ret)
		return ret;

	avd_w32(wrap, 0x14, 0x0);
	return 0;
}

void avd_shutdown(struct avd_dev *avd)
{
	avd_w32(mbox, 0x08, 0xe);
	avd_w32(mbox, 0x98, 0x1);
	avd_w32(mbox, 0x10, 0x0);
	avd_w32(mbox, 0x48, 0x0);
}

#define w32(off, val) (avd_w32(ctrl, off, val))
#define r32(off) (avd_r32(ctrl, off))

void t8103_configure_stream(struct avd_dev *avd, dma_addr_t addr, u8 fifo_idx,
			  u32 vp_slot)
{
	w32(0x4068 + (fifo_idx * 4), addr >> 8);
	w32(0x4084 + (fifo_idx * 4), 0x100000);
	w32(0x40a0 + (fifo_idx * 4), 0);
	w32(0x40bc + (fifo_idx * 4), 0);

	w32(0x4040 + (vp_slot * 4), 0);
	m32(0x405c, 7 << (vp_slot * 5) | (5 << 20)); /* irq mask */
}

void t8112_configure_stream(struct avd_dev *avd, dma_addr_t addr, u8 fifo_idx,
			  u32 vp_slot)
{
	w32(ADDR_HIGH(fifo_idx), addr >> 32);
	w32(ADDR_LOW(fifo_idx), addr & 0xffffffff);
	/* unkown AVD_VP_INSN_FIFO_MASK */
	w32(0x18c + (fifo_idx * 4), 0);
	/* unkown AVD_VP_INSN_FIFO_CACH */
	w32(0x204 + (fifo_idx * 4), 0);
	w32(INST_OFF(fifo_idx), 0); /* clear instruction offset */

	m32(0x0fc + (vp_slot * 4), 7); /* irq mask */
	m32(0x120, 5);
}

void avd_status(struct avd_dev *avd, u32 vp)
{
	/*
	 * The same for rev 3?
	 *
	 * + 0xc status bitmask 8 is no errors?
	 * + 0x0 looks like sl->first_mb_in_slice
	 * + 0x4 another status bitmask? 0x3f on succes? 0x40 on error?
	 * + 0x8 slice bytes read? is almost never the same as hdr_size written
	 */
	u32 start = 0x1000 | (vp << 8);
	dev_info(avd->dev, "VP%d: %08x %08x %08x %08x", vp, r32(start),
		 r32(start + 4), r32(start + 8), r32(start + 12));
	dev_info(avd->dev, "VP%d: %08x %08x %08x", vp, r32(start + 16),
		 r32(start + 20), r32(start + 24));
}

#undef w32
#undef r32
#undef m32
