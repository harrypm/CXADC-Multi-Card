/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2020 Jorge Maidana <jorgem.linux@gmail.com>
 *
 * This driver is a derivative of:
 *
 * Device driver for Conexant 2388x based TV cards
 * Copyright (c) 2003 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
 *
 * Device driver for Conexant 2388x based TV cards
 * Copyright (c) 2005-2006 Mauro Carvalho Chehab <mchehab@kernel.org>
 *
 * cxadc.c - CX2388x ADC DMA driver for Linux 2.6.18 version 0.3
 * Copyright (c) 2005-2007 Hew How Chee <how_chee@yahoo.com>
 *
 * cxadc.c - CX2388x ADC DMA driver for Linux 3.x version 0.5
 * Copyright (c) 2013-2015 Chad Page <Chad.Page@gmail.com>
 */

#ifndef CX88SDR_H
#define CX88SDR_H

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define CX88SDR_XTAL_FREQ		28636363 /* Xtal Frequency */
#define CX88SDR_ADC_FREQ_MIN		12672000 /* Min ADC Frequency */
#define CX88SDR_ADC_FREQ_DEF		28800000 /* Def ADC Frequency */
#define CX88SDR_ADC_FREQ_MAX		36480000 /* Max ADC Frequency */

/* Default values for raw video mode */
//#define CX88SDR_RAW_VIDEO_MODE

/* Real formats */
#ifndef V4L2_SDR_FMT_RU8
#define V4L2_SDR_FMT_RU8		V4L2_SDR_FMT_CU8
#endif
#ifndef V4L2_SDR_FMT_RU16LE
#define V4L2_SDR_FMT_RU16LE		V4L2_SDR_FMT_CU16LE
#endif

#define CX88SDR_DRV_NAME		"CX2388x SDR"
#define CX88SDR_MAX_CARDS		32

#define CX88SDR_DEV_CNTRL2		0x200034 /* Device Control #2 */
#define CX88SDR_PCI_INT_MSK		0x200040 /* PCI Interrupt Mask */
#define CX88SDR_PCI_INT_MSK_CLEAR	0x000000 /* PCI Interrupt Mask Clear */
#define CX88SDR_PCI_INT_MSK_VAL		0x000001 /* PCI Interrupt Mask Value */
#define CX88SDR_VID_INT_MSK		0x200050 /* Video Interrupt Mask */
#define CX88SDR_VID_INT_MSK_CLEAR	0x000000 /* Video Interrupt Mask Clear */
#define CX88SDR_VID_INT_MSK_VAL		0x018888 /* Video Interrupt Mask Value */
#define CX88SDR_VID_INT_STAT		0x200054 /* Video Interrupt Status */
#define CX88SDR_VID_INT_STAT_CLEAR	0x0fffff /* Video Interrupt Status Clear */

#define CX88SDR_DMA24_PTR2		0x3000cc /* IPB DMAC Current Table Pointer */
#define CX88SDR_DMA24_CNT1		0x30010c /* IPB DMAC Buffer Limit */
#define CX88SDR_DMA24_CNT2		0x30014c /* IPB DMAC Table Size */
#define CX88SDR_VBI_GP_CNT		0x31c02c /* VBI General Purpose Counter */
#define CX88SDR_VID_DMA_CNTRL		0x31c040 /* IPB DMA Control */
#define CX88SDR_INPUT_FORMAT		0x310104 /* Input Format Register */
#define CX88SDR_HTOTAL			0x310120 /* Total Pixel Count */
#define CX88SDR_OUTPUT_FORMAT		0x310164 /* Output Format */
#define CX88SDR_PLL_REG			0x310168 /* PLL Register */
#define CX88SDR_PLL_ADJ_CTRL		0x31016c /* PLL Adjust Control Register */
#define CX88SDR_SCONV_REG		0x310170 /* Sample Rate Conversion Register */
#define CX88SDR_CAPTURE_CTRL		0x310180 /* Capture Control */
#define CX88SDR_COLOR_CTRL		0x310184 /* Color Format/Control */
#define CX88SDR_VBI_PACKET		0x310188 /* VBI Packet Size/Delay */
#define CX88SDR_AGC_TIP3		0x310210 /* AGC Sync Tip Adjust 3 */
#define CX88SDR_AGC_ADJ3		0x31021c /* AGC Gain Adjust 3 */
#define CX88SDR_AGC_ADJ4		0x310220 /* AGC Gain Adjust 4 */
#define CX88SDR_AFE_CFG_IO		0x35c04c /* ADC Mode Select */

#define CX88SDR_SRAM_ADDR		0x180000 /* 32 KByte SRAM */
#define CX88SDR_DMA24_CMDS_ADDR		(CX88SDR_SRAM_ADDR + 0x0100) /* DMA #24 CMDS */
#define CX88SDR_RISC_INST_QUEUE_ADDR	(CX88SDR_SRAM_ADDR + 0x0800) /* RISC Instruction Queue */
#define CX88SDR_CDT_ADDR		(CX88SDR_SRAM_ADDR + 0x1000) /* Cluster Descriptor Table */
#define CX88SDR_CLUSTER_BUF_ADDR	(CX88SDR_SRAM_ADDR + 0x4000) /* Cluster Buffers */

#define CX88SDR_RISC_CNT_INCR		(1 << 16) /* Increment Counter */
#define CX88SDR_RISC_CNT_RESET		(3 << 16) /* Reset Counter */
#define CX88SDR_RISC_IRQ1_CNT_MASK	(512 - 1) /* PAGES per Interrupt - 1 */
#define CX88SDR_RISC_IRQ1_NOOP		(0U << 24) /* No Change */
#define CX88SDR_RISC_IRQ1_TRIG		(1U << 24) /* Trigger Interrupt */
#define CX88SDR_RISC_EOL		(1U << 26) /* EOL */
#define CX88SDR_RISC_SOL		(1U << 27) /* SOL */
#define CX88SDR_RISC_WRITE		(1U << 28) /* RISC WRITE Instruction */
#define CX88SDR_RISC_JUMP		(7U << 28) /* RISC JUMP Instruction */
#define CX88SDR_RISC_SYNC		(8U << 28) /* RISC SYNC Instruction */

#define CX88SDR_CDT_SIZE		(64 >> 3) /* CDT 8-byte sized, 2 minimum */
#define CX88SDR_RISC_INST_QUEUE_SIZE	(256 >> 2) /* RISC Instruction Queue 4-byte sized */
#define CX88SDR_VBI_PACKET_SIZE		SZ_2K
#define CX88SDR_VBI_DMA_SIZE		SZ_64M
#define CX88SDR_VBI_DMA_PAGES		(CX88SDR_VBI_DMA_SIZE >> PAGE_SHIFT)

/* 2 RISC WRITE Instructions per PAGE + one PAGE for SYNC and JUMP */
#define CX88SDR_RISC_BUF_SIZE		(PAGE_ALIGN((CX88SDR_VBI_DMA_PAGES * 16) + \
					 PAGE_SIZE))
#define CX88SDR_RISC_WRITE_VBI_PACKET	(CX88SDR_RISC_WRITE | CX88SDR_VBI_PACKET_SIZE | \
					 CX88SDR_RISC_SOL | CX88SDR_RISC_EOL)

enum {
	CX88SDR_INPUT_00, /* Pin 145 */
	CX88SDR_INPUT_01, /* Pin 144 */
	CX88SDR_INPUT_02, /* Pin 143 */
	CX88SDR_INPUT_03, /* Pin 142 */
};

/* Default values */
#define CX88SDR_GAIN_DEFVAL		0x00
#define CX88SDR_AGC_ADJ3_DEFVAL		0x00
#define CX88SDR_INPUT_DEFVAL		CX88SDR_INPUT_00
#define CX88SDR_AFC_PLL_DEFVAL		0x01 /* 0 = Disable UltraLock */
#define CX88SDR_INPUT_VSYNC_DEFVAL	0x00

#ifndef CX88SDR_RAW_VIDEO_MODE		/* SDR mode default values */
#define CX88SDR_GAIN_6DB_DEFVAL		0x01
#define CX88SDR_AGC_TIP3_DEFVAL		0x38
#define CX88SDR_ADC_FREQ_DEFVAL		CX88SDR_ADC_FREQ_DEF
#define CX88SDR_HTOTAL_DEFVAL		(CX88SDR_ADC_FREQ_DEFVAL / 60 / 525) /* NTSC */
#else					/* Raw video mode default values */
#define CX88SDR_GAIN_6DB_DEFVAL		0x00
#define CX88SDR_AGC_TIP3_DEFVAL		0x00
#define CX88SDR_HTOTAL_DEFVAL		910
#define CX88SDR_ADC_FREQ_DEFVAL		(CX88SDR_HTOTAL_DEFVAL * 525 * 60) /* NTSC */
#endif

struct cx88sdr_ctrl {
	u32				freq;
	u32				pixelformat;
	u32				buffersize;
	u32				gain;
	u32				agc_adj3;
	u32				agc_tip3;
	u32				input;
	u32				htotal;
	bool				gain_6db;
	bool				afc_pll;
	bool				input_vsync;
};

struct cx88sdr_dev {
	int				nr;
	char				name[32];

	/* IO */
	struct	pci_dev			*pdev;
	dma_addr_t			risc_buf_addr;
	dma_addr_t			*dma_pages_addr;
	uint32_t	__iomem		*ctrl;
	uint32_t			*risc_buf;
	void				**dma_buf_pages;
	unsigned int			irq;
	int				pci_lat;

	/* V4L2 */
	struct	v4l2_device		v4l2_dev;
	struct	v4l2_ctrl_handler	ctrl_handler;
	struct	video_device		vdev;
	struct	mutex			vdev_mlock;
	struct	mutex			vopen_mlock;
	struct	cx88sdr_ctrl		vctrl;
	u32				vopen;
};

/* Helpers */
static inline uint32_t ctrl_ioread32(struct cx88sdr_dev *dev, uint32_t reg)
{
	return ioread32(dev->ctrl + ((reg) >> 2));
}

static inline void ctrl_iowrite32(struct cx88sdr_dev *dev, uint32_t reg, uint32_t val)
{
	iowrite32((val), dev->ctrl + ((reg) >> 2));
}

#define cx88sdr_pr_info(fmt, ...)	pr_info(KBUILD_MODNAME " %s: " fmt,		\
						pci_name(dev->pdev), ##__VA_ARGS__)
#define cx88sdr_pr_err(fmt, ...)	pr_err(KBUILD_MODNAME " %s: " fmt,		\
						pci_name(dev->pdev), ##__VA_ARGS__)

/* cx88_sdr_v4l2.c */
extern const struct v4l2_ctrl_ops cx88sdr_ctrl_ops;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_gain_6db;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_agc_adj3;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_agc_tip3;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_input;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_afc_pll;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_input_vsync;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_htotal;
extern const struct video_device cx88sdr_template;

int cx88sdr_adc_fmt_set(struct cx88sdr_dev *dev);
void cx88sdr_gain_set(struct cx88sdr_dev *dev);
void cx88sdr_input_set(struct cx88sdr_dev *dev);

#endif
