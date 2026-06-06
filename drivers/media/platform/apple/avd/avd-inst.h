#ifndef AVD_INST_H_
#define AVD_INST_H_
/* instruction stream things  */
#include "avd.h"
#include "linux/types.h"

/* i have no clue what this is */
#define INST_DMA1 0 /* (0x14 << 16 | 0x14) */
#define INST_DMA2 0 /* (0x4000000  | INST_DMA1) */
#define INST_DMA3 0 /* (0x07 << 16 | 0x07) */

#define VP_SLOT_NUM 4
#define VP_SLOT_NONE 255
#define VP_SLOT_START 0xc

static inline u32 fifo_size(void)
{
	return 0x100000 * 12;
}

static inline u32 swrap(u32 x, u32 w)
{
	return x & (w - 1);
}
static inline bool boolify(u32 v)
{
	return !!(v);
}

/*
 * VP's start at VP_CTRL_OFFSET
 * I think theres 8 fifo registers
 * based on that the first 4 dont work i think thoose are ads.
 * but the last 4 are up for graps.
 *
 * I hope its the same in revision 3, that would explain the 'mystery codec'
 *
 * VP_SLOT_START + (8*4) i think its two PP fifo registers
 * My guess is ads, and then regular
 * */
static inline void push(struct avd_dev *avd, struct avd_ctx *ctx, u32 inst)
{
	avd_w32(ctrl, VP_SLOT_START + (ctx->vp_slot * 4), inst);
}
static inline void push_address(struct avd_dev *avd, struct avd_ctx *ctx,
				dma_addr_t addr)
{
	/* i really hope i dont have the whole >> 7 or 8 step with the addresses */
	push(avd, ctx, (u32)(addr & 0xffffffff));
	push(avd, ctx, (u32)(addr >> 32));
}

#ifdef DEBUG_INST
#define push(inst, name)                                           \
	do {                                                       \
		dev_info(ctx->dev->dev, "%8x | %s", (inst), name); \
		push(avd, ctx, inst);                              \
	} while (0)

#else
#define push(inst, name) push(avd, ctx, inst)
#endif

#ifdef DEBUG_INST_ADDR
#define pusha(inst, name, i)                                      \
	do {                                                      \
		dev_info(ctx->dev->dev, "%8llx  | %s[%d] (high)", \
			 (inst) >> 32, name, i);                  \
		dev_info(ctx->dev->dev, "%8llx  | %s[%d]",        \
			 (inst) & 0xffffffff, name, i);           \
		push_address(avd, ctx, inst)                      \
	} while (0)

#else
#define pusha(inst, name, i) push_address(avd, ctx, inst)
#endif

#endif /* AVD_INST_H_ */
