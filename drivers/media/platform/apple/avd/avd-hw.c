#include "linux/dev_printk.h"
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "avd.h"
#include "avd-regs.h"

/* The plan is to move this to the cm3 */

#define AVD_V3_VP_INSN_FIFO_IOVA	0x4068
#define AVD_V3_VP_INSN_FIFO_MASK	0x4084
#define AVD_V3_VP_INSN_FIFO_CACH	0x40a0
#define AVD_V3_VP_INSN_FIFO_XFER	0x40bc
#define AVD_V3_VP_CTRL_UNK	  0x4040
#define AVD_V3_CTRL_CM3_IRQ_MASK	0x405c

#define AVD_V4_VP_INSN_FIFO_IOVA_HI	0x30c
#define AVD_V4_VP_INSN_FIFO_IOVA_LO	0x150
#define AVD_V4_VP_INSN_FIFO_MASK	0x18c
#define AVD_V4_VP_INSN_FIFO_CACH	0x1c8
#define AVD_V4_VP_INSN_FIFO_XFER	0x204
#define AVD_V4_VP_CTRL_CM3_IRQ_MASK	0x0fc
#define AVD_V4_PP_CTRL_CM3_IRQ_MASK	0x120

/* seems stabile after this */
#define AVD_V5_VP_INSN_FIFO_IOVA_HI	0x20c
#define AVD_V5_VP_INSN_FIFO_IOVA_LO	0x1d0
#define AVD_V5_VP_INSN_FIFO_MASK	0x248
#define AVD_V5_VP_INSN_FIFO_CACH	0x284
#define AVD_V5_VP_INSN_FIFO_XFER	0x2c0
#define AVD_V5_VP_CTRL_CM3_IRQ_MASK	0x15c
#define AVD_V5_PP_CTRL_CM3_IRQ_MASK	0x190

#define AVD_CM3_UNK	  BIT(0)
#define AVD_CM3_ERROR	BIT(1)
#define AVD_CM3_DONE	BIT(2)
#define AVD_CM3_FULL	BIT(3)

#define AVD_VP_CM3_MASK (AVD_CM3_UNK | AVD_CM3_ERROR | AVD_CM3_DONE)
#define AVD_PP_CM3_MASK (AVD_CM3_UNK | AVD_CM3_DONE)

int avd_boot(struct avd_dev *avd)
{
	u32 val;
	int ret;

	avd_w32(base, AVD_REG_POWER_ON, 0xfff);

	if (avd->variant->revision != 3)
		dev_info_once(avd->dev, "booting hw version: %04x", avd_r32(ctrl, 0));

	memcpy_toio(avd->code, avd->fw->data, avd->fw->size);

	avd_w32(mbox, AVD_REG_RUN_CTRL, AVD_RUN_CTRL_UNK_STOP);

	avd_w32(mbox, AVD_REG_MBOX1_STATUS, AVD_MBOX_ENABLE);

	avd_w32(mbox, AVD_REG_MBOX_IRQ_ENABLE, AVD_MBOX1_NOT_EMPTY);
	avd_w32(mbox, AVD_REG_RUN_CTRL, AVD_RUN_CTRL_UNK_RUN);

	/* wait for cm3 to boot */
	ret = readl_poll_timeout(avd->mbox + AVD_REG_FLAG0_SET,
			val, val == 1, 10, 10000);
	if (ret)
		return ret;

	return 0;
}

void avd_shutdown(struct avd_dev *avd)
{
	avd_w32(mbox, AVD_REG_RUN_CTRL, AVD_RUN_CTRL_UNK_STOP);
	avd_w32(mbox, AVD_REG_FLAG0_CLR, 0x1);
	avd_w32(mbox, AVD_REG_MBOX_IRQ_ENABLE, 0x0);
}

#define m32(off, val) (avd_m32(ctrl, off, val))
#define w32(off, val) (avd_w32(ctrl, off, val))


void t8103_configure_stream(struct avd_dev *avd, dma_addr_t addr, u8 fifo_idx,
			  u32 vp_slot)
{
	w32(AVD_V3_VP_INSN_FIFO_IOVA + (fifo_idx * 4), addr >> 8);
	w32(AVD_V3_VP_INSN_FIFO_MASK + (fifo_idx * 4), 0x100000);
	w32(AVD_V3_VP_INSN_FIFO_CACH + (fifo_idx * 4), 0);
	w32(AVD_V3_VP_INSN_FIFO_XFER + (fifo_idx * 4), 0);

	w32(AVD_V3_VP_CTRL_UNK + (vp_slot * 4), 0);
	m32(AVD_V3_CTRL_CM3_IRQ_MASK,
			AVD_VP_CM3_MASK << (vp_slot * 5)
			| (AVD_PP_CM3_MASK << 20));
}

void t8112_configure_stream(struct avd_dev *avd, dma_addr_t addr, u8 fifo_idx,
			  u32 vp_slot)
{
	w32(AVD_V4_VP_INSN_FIFO_IOVA_HI + (fifo_idx * 4), addr >> 32);
	w32(AVD_V4_VP_INSN_FIFO_IOVA_LO + (fifo_idx * 4), addr & 0xffffffff);
	w32(AVD_V4_VP_INSN_FIFO_MASK + (fifo_idx * 4), 0);
	w32(AVD_V4_VP_INSN_FIFO_CACH + (fifo_idx * 4), 0);
	w32(AVD_V4_VP_INSN_FIFO_XFER + (fifo_idx * 4), 0);

	m32(AVD_V4_VP_CTRL_CM3_IRQ_MASK + (vp_slot * 4), AVD_VP_CM3_MASK);
	m32(AVD_V4_PP_CTRL_CM3_IRQ_MASK, AVD_PP_CM3_MASK);
}

void t8122_configure_stream(struct avd_dev *avd, dma_addr_t addr, u8 fifo_idx,
			  u32 vp_slot)
{
	w32(AVD_V5_VP_INSN_FIFO_IOVA_HI + (fifo_idx * 4), addr >> 32);
	w32(AVD_V5_VP_INSN_FIFO_IOVA_LO + (fifo_idx * 4), addr & 0xffffffff);
	w32(AVD_V5_VP_INSN_FIFO_MASK + (fifo_idx * 4), 0);
	w32(AVD_V5_VP_INSN_FIFO_CACH + (fifo_idx * 4), 0);
	w32(AVD_V5_VP_INSN_FIFO_XFER + (fifo_idx * 4), 0);

	m32(AVD_V5_VP_CTRL_CM3_IRQ_MASK + (vp_slot * 4), AVD_VP_CM3_MASK);
	m32(AVD_V5_PP_CTRL_CM3_IRQ_MASK, AVD_PP_CM3_MASK);
}

#define r32(off) (avd_r32(ctrl, off))
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
