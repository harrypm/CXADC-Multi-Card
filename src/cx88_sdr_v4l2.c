// SPDX-License-Identifier: GPL-2.0-or-later
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

#include <linux/math64.h>
#include <linux/pci.h>
#include <linux/videodev2.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "cx88_sdr.h"

#define CX88SDR_V4L2_NAME		"CX2388x SDR V4L2"

/* Reserve 16 controls for this driver */
#ifndef V4L2_CID_USER_CX88SDR_BASE
#define V4L2_CID_USER_CX88SDR_BASE	(V4L2_CID_USER_BASE + 0x1f10)
#endif

enum {
	/* +6dB gain control (INIT_6DB_VAL) */
	V4L2_CID_CX88SDR_GAIN_6DB	= (V4L2_CID_USER_CX88SDR_BASE + 0),
	/* AGC Gain Adjust 3 */
	V4L2_CID_CX88SDR_AGC_ADJ3,
	/* AGC Sync Tip Adjust 3 */
	V4L2_CID_CX88SDR_AGC_TIP3,
	/* Pin input select (YADC_SEL) */
	V4L2_CID_CX88SDR_INPUT,
	/* PLL AFC (PLL_ADJ_EN) */
	V4L2_CID_CX88SDR_AFC_PLL,
	/* Enable vertical sync detection (VERTEN) */
	V4L2_CID_CX88SDR_INPUT_VSYNC,
	/* Total number of pixels per line */
	V4L2_CID_CX88SDR_HTOTAL,
};

enum {
	CX88SDR_BAND_RU08,
	CX88SDR_BAND_RU16,
};

struct cx88sdr_fh {
	struct v4l2_fh fh;
	struct cx88sdr_dev *dev;
	uint32_t spage;
};

static const struct v4l2_frequency_band cx88sdr_bands[] = {
	[CX88SDR_BAND_RU08] = {
		.type		= V4L2_TUNER_SDR,
		.capability	= (V4L2_TUNER_CAP_1HZ | V4L2_TUNER_CAP_FREQ_BANDS),
		.rangelow	= CX88SDR_ADC_FREQ_MIN,
		.rangehigh	= CX88SDR_ADC_FREQ_MAX,
	},
	[CX88SDR_BAND_RU16] = {
		.type		= V4L2_TUNER_SDR,
		.capability	= (V4L2_TUNER_CAP_1HZ | V4L2_TUNER_CAP_FREQ_BANDS),
		.rangelow	= (CX88SDR_ADC_FREQ_MIN / 2),
		.rangehigh	= (CX88SDR_ADC_FREQ_MAX / 2),
	},
};

static int cx88sdr_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct cx88sdr_dev *dev = container_of(vdev, struct cx88sdr_dev, vdev);
	struct cx88sdr_fh *fh;
	uint32_t cpage;

	fh = kzalloc(sizeof(*fh), GFP_KERNEL);
	if (!fh)
		return -ENOMEM;

	v4l2_fh_init(&fh->fh, vdev);

	fh->dev = dev;
	file->private_data = &fh->fh;
	v4l2_fh_add(&fh->fh);

	cpage = ctrl_ioread32(dev, CX88SDR_VBI_GP_CNT);
	cpage = (!cpage) ? (CX88SDR_VBI_DMA_PAGES - 1) : (cpage - 1);
	fh->spage = cpage;

	mutex_lock(&dev->vopen_mlock);
	if (!dev->vopen++)
		ctrl_iowrite32(dev, CX88SDR_PCI_INT_MSK, CX88SDR_PCI_INT_MSK_VAL);
	mutex_unlock(&dev->vopen_mlock);
	return 0;
}

static int cx88sdr_release(struct file *file)
{
	struct v4l2_fh *vfh = file->private_data;
	struct cx88sdr_fh *fh = container_of(vfh, struct cx88sdr_fh, fh);
	struct cx88sdr_dev *dev = fh->dev;

	mutex_lock(&dev->vopen_mlock);
	if (!--dev->vopen)
		ctrl_iowrite32(dev, CX88SDR_PCI_INT_MSK, CX88SDR_PCI_INT_MSK_CLEAR);
	mutex_unlock(&dev->vopen_mlock);

	v4l2_fh_del(&fh->fh);
	v4l2_fh_exit(&fh->fh);
	kfree(fh);
	return 0;
}

static ssize_t cx88sdr_read(struct file *file, char __user *buf, size_t size,
			    loff_t *pos)
{
	struct v4l2_fh *vfh = file->private_data;
	struct cx88sdr_fh *fh = container_of(vfh, struct cx88sdr_fh, fh);
	struct cx88sdr_dev *dev = fh->dev;
	ssize_t result = 0;
	uint32_t cpage, page;

	page = (fh->spage + ((*pos % CX88SDR_VBI_DMA_SIZE) >> PAGE_SHIFT)) %
		CX88SDR_VBI_DMA_PAGES;

retry:
	cpage = ctrl_ioread32(dev, CX88SDR_VBI_GP_CNT);
	cpage = (!cpage) ? (CX88SDR_VBI_DMA_PAGES - 1) : (cpage - 1);

	if ((page == cpage) && (file->f_flags & O_NONBLOCK))
		return -EAGAIN;

	while (size && (page != cpage)) {
		u32 len;

		/* Handle partial pages */
		len = (*pos % PAGE_SIZE) ? (PAGE_SIZE - (*pos % PAGE_SIZE)) : PAGE_SIZE;
		if (len > size)
			len = size;

		if (copy_to_user(buf, dev->dma_buf_pages[page] + (*pos % PAGE_SIZE), len))
			return -EFAULT;

		result += len;
		buf    += len;
		*pos   += len;
		size   -= len;
		page    = (fh->spage + ((*pos % CX88SDR_VBI_DMA_SIZE) >> PAGE_SHIFT)) %
			   CX88SDR_VBI_DMA_PAGES;
	}

	if (size && !(file->f_flags & O_NONBLOCK))
		goto retry;

	return result;
}

static __poll_t cx88sdr_poll(struct file *file, struct poll_table_struct *wait)
{
	return (EPOLLIN | EPOLLRDNORM | v4l2_ctrl_poll(file, wait));
}

static const struct v4l2_file_operations cx88sdr_fops = {
	.owner		= THIS_MODULE,
	.open		= cx88sdr_open,
	.release	= cx88sdr_release,
	.read		= cx88sdr_read,
	.poll		= cx88sdr_poll,
	.unlocked_ioctl	= video_ioctl2,
};

static int cx88sdr_querycap(struct file *file, void __always_unused *priv,
			    struct v4l2_capability *cap)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s", pci_name(dev->pdev));
	strscpy(cap->card, CX88SDR_DRV_NAME, sizeof(cap->card));
	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	return 0;
}

static int cx88sdr_enum_fmt_sdr(struct file __always_unused *file,
				void __always_unused *priv,
				struct v4l2_fmtdesc *f)
{
	switch (f->index) {
	case 0:
		f->pixelformat = V4L2_SDR_FMT_RU8;
		break;
	case 1:
		f->pixelformat = V4L2_SDR_FMT_RU16LE;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int cx88sdr_try_fmt_sdr(struct file *file, void __always_unused *priv,
			       struct v4l2_format *f)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	if (f->fmt.sdr.pixelformat != V4L2_SDR_FMT_RU8 &&
	    f->fmt.sdr.pixelformat != V4L2_SDR_FMT_RU16LE)
		f->fmt.sdr.pixelformat = V4L2_SDR_FMT_RU8;
	f->fmt.sdr.buffersize = dev->vctrl.buffersize;
	return 0;
}

static int cx88sdr_g_fmt_sdr(struct file *file, void __always_unused *priv,
			     struct v4l2_format *f)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	f->fmt.sdr.pixelformat = dev->vctrl.pixelformat;
	f->fmt.sdr.buffersize = dev->vctrl.buffersize;
	return 0;
}

static int cx88sdr_s_fmt_sdr(struct file *file, void __always_unused *priv,
			     struct v4l2_format *f)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	if (f->fmt.sdr.pixelformat != V4L2_SDR_FMT_RU8 &&
	    f->fmt.sdr.pixelformat != V4L2_SDR_FMT_RU16LE)
		f->fmt.sdr.pixelformat = V4L2_SDR_FMT_RU8;
	dev->vctrl.pixelformat = f->fmt.sdr.pixelformat;
	f->fmt.sdr.buffersize = dev->vctrl.buffersize;
	return cx88sdr_adc_fmt_set(dev);
}

static int cx88sdr_g_tuner(struct file *file, void __always_unused *priv,
			   struct v4l2_tuner *t)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	if (t->index > 0)
		return -EINVAL;

	switch (dev->vctrl.pixelformat) {
	case V4L2_SDR_FMT_RU8:
		t->rangelow  = cx88sdr_bands[CX88SDR_BAND_RU08].rangelow;
		t->rangehigh = cx88sdr_bands[CX88SDR_BAND_RU08].rangehigh;
		break;
	case V4L2_SDR_FMT_RU16LE:
		t->rangelow  = cx88sdr_bands[CX88SDR_BAND_RU16].rangelow;
		t->rangehigh = cx88sdr_bands[CX88SDR_BAND_RU16].rangehigh;
		break;
	default:
		return -EINVAL;
	}
	strscpy(t->name, "ADC: CX2388x SDR", sizeof(t->name));
	t->type = V4L2_TUNER_SDR;
	t->capability = (V4L2_TUNER_CAP_1HZ | V4L2_TUNER_CAP_FREQ_BANDS);
	return 0;
}

static int cx88sdr_s_tuner(struct file __always_unused *file,
			   void __always_unused *priv,
			   const struct v4l2_tuner *t)
{
	if (t->index > 0)
		return -EINVAL;

	return 0;
}

static int cx88sdr_enum_freq_bands(struct file *file, void __always_unused *priv,
				   struct v4l2_frequency_band *band)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	if (band->tuner > 0 || band->index > 0)
		return -EINVAL;

	switch (dev->vctrl.pixelformat) {
	case V4L2_SDR_FMT_RU8:
		*band = cx88sdr_bands[CX88SDR_BAND_RU08];
		break;
	case V4L2_SDR_FMT_RU16LE:
		*band = cx88sdr_bands[CX88SDR_BAND_RU16];
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int cx88sdr_g_frequency(struct file *file, void __always_unused *priv,
			       struct v4l2_frequency *f)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	if (f->tuner > 0)
		return -EINVAL;

	switch (dev->vctrl.pixelformat) {
	case V4L2_SDR_FMT_RU8:
		/* fallthrough */
	case V4L2_SDR_FMT_RU16LE:
		f->frequency = dev->vctrl.freq;
		f->type = V4L2_TUNER_SDR;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int cx88sdr_s_frequency(struct file *file, void __always_unused *priv,
			       const struct v4l2_frequency *f)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	if (f->tuner > 0 || f->type != V4L2_TUNER_SDR)
		return -EINVAL;

	dev->vctrl.freq = f->frequency;
	return cx88sdr_adc_fmt_set(dev);
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int cx88sdr_g_register(struct file *file, void __always_unused *priv,
			      struct v4l2_dbg_register *reg)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	reg->size = sizeof(uint32_t);
	reg->val = (__u64)ctrl_ioread32(dev, (u32)(reg->reg & 0xfffffc));
	return 0;
}

static int cx88sdr_s_register(struct file *file, void __always_unused *priv,
			      const struct v4l2_dbg_register *reg)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	/* CX2388x has a 24-bit register space */
	ctrl_iowrite32(dev, (u32)(reg->reg & 0xfffffc), lower_32_bits(reg->val));
	return 0;
}
#endif

static const struct v4l2_ioctl_ops cx88sdr_ioctl_ops = {
	.vidioc_querycap		= cx88sdr_querycap,
	.vidioc_enum_fmt_sdr_cap	= cx88sdr_enum_fmt_sdr,
	.vidioc_try_fmt_sdr_cap		= cx88sdr_try_fmt_sdr,
	.vidioc_g_fmt_sdr_cap		= cx88sdr_g_fmt_sdr,
	.vidioc_s_fmt_sdr_cap		= cx88sdr_s_fmt_sdr,
	.vidioc_g_tuner			= cx88sdr_g_tuner,
	.vidioc_s_tuner			= cx88sdr_s_tuner,
	.vidioc_enum_freq_bands		= cx88sdr_enum_freq_bands,
	.vidioc_g_frequency		= cx88sdr_g_frequency,
	.vidioc_s_frequency		= cx88sdr_s_frequency,
	.vidioc_log_status		= v4l2_ctrl_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.vidioc_g_register		= cx88sdr_g_register,
	.vidioc_s_register		= cx88sdr_s_register,
#endif
};

const struct video_device cx88sdr_template = {
	.device_caps	= (V4L2_CAP_SDR_CAPTURE | V4L2_CAP_TUNER |
			   V4L2_CAP_READWRITE),
	.fops		= &cx88sdr_fops,
	.ioctl_ops	= &cx88sdr_ioctl_ops,
	.name		= CX88SDR_V4L2_NAME,
	.release	= video_device_release_empty,
};

void cx88sdr_gain_set(struct cx88sdr_dev *dev)
{
	ctrl_iowrite32(dev, CX88SDR_AGC_ADJ3, ((dev->vctrl.agc_adj3 + 0x28) << 16) |
					      (0x38 << 8) | 0xc0);
	ctrl_iowrite32(dev, CX88SDR_AGC_ADJ4, ((uint32_t)dev->vctrl.gain_6db << 23) |
					      (dev->vctrl.gain << 16) | (0x2c << 8) | 0x34);
	ctrl_iowrite32(dev, CX88SDR_AGC_TIP3, (0x1e48 << 16) | (0xe0 << 8) |
					      (0x40 - dev->vctrl.agc_tip3));
}

void cx88sdr_input_set(struct cx88sdr_dev *dev)
{
	ctrl_iowrite32(dev, CX88SDR_HTOTAL, dev->vctrl.htotal);
	ctrl_iowrite32(dev, CX88SDR_PLL_ADJ_CTRL, ((uint32_t)dev->vctrl.afc_pll << 25) |
						  (0x20 << 19) | (0x7 << 14) |
						  (0x63 << 7) | 0x10);
	ctrl_iowrite32(dev, CX88SDR_INPUT_FORMAT, (1 << 16) | (dev->vctrl.input << 14) |
						  (1 << 13) | ((uint32_t)dev->vctrl.input_vsync << 7) |
						  (1 << 4) | 0x1);
}

int cx88sdr_adc_fmt_set(struct cx88sdr_dev *dev)
{
	s64 pll_frac, sconv_val, freq = dev->vctrl.freq;
	u32 pll_int, pll_freq;

	switch (dev->vctrl.pixelformat) {
	case V4L2_SDR_FMT_RU8:
		freq = clamp_t(s64, freq,
			       cx88sdr_bands[CX88SDR_BAND_RU08].rangelow,
			       cx88sdr_bands[CX88SDR_BAND_RU08].rangehigh);
		dev->vctrl.freq = (u32)freq;
		pll_freq = dev->vctrl.freq;
		ctrl_iowrite32(dev, CX88SDR_CAPTURE_CTRL, (1 << 6) | (3 << 1));
		break;
	case V4L2_SDR_FMT_RU16LE:
		freq = clamp_t(s64, freq,
			       cx88sdr_bands[CX88SDR_BAND_RU16].rangelow,
			       cx88sdr_bands[CX88SDR_BAND_RU16].rangehigh);
		dev->vctrl.freq = (u32)freq;
		pll_freq = dev->vctrl.freq * 2;
		ctrl_iowrite32(dev, CX88SDR_CAPTURE_CTRL, (1 << 6) | (1 << 5) | (3 << 1));
		break;
	default:
		return -EINVAL;
	}

	/* (Xtal / 4 / 8) * (pll_int + (pll_frac / 2^20)) = pll_freq */
	for (pll_int = 14; pll_int < 64; pll_int++) {
		pll_frac = div_s64(pll_freq * 0x2000000LL, CX88SDR_XTAL_FREQ) - (pll_int << 20);
		if (pll_frac <= 0xfffff)
			break;
	}

	if ((pll_frac < 0) || (pll_frac > 0xfffff)) {
		cx88sdr_pr_err("frequency %lldHz out of range, PLL frac = %lld\n",
				freq, pll_frac);
		return -EINVAL;
	}

	/* (Xtal / pll_freq) * 2^17 = sconv_val */
	sconv_val = div_s64(CX88SDR_XTAL_FREQ * 0x20000LL, (s32)pll_freq);
	if ((sconv_val < 0) || (sconv_val > 0x7ffff)) {
		cx88sdr_pr_err("frequency %lldHz out of range, SCONV val = %lld\n",
				freq, sconv_val);
		return -EINVAL;
	}

	ctrl_iowrite32(dev, CX88SDR_SCONV_REG, (u32)sconv_val);
	ctrl_iowrite32(dev, CX88SDR_PLL_REG, (2U << 26) | (pll_int << 20) | (u32)pll_frac);
	return 0;
}

static int cx88sdr_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct cx88sdr_dev *dev = container_of(ctrl->handler,
					       struct cx88sdr_dev, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		dev->vctrl.gain = ctrl->val;
		cx88sdr_gain_set(dev);
		break;
	case V4L2_CID_CX88SDR_GAIN_6DB:
		dev->vctrl.gain_6db = ctrl->val;
		cx88sdr_gain_set(dev);
		break;
	case V4L2_CID_CX88SDR_AGC_ADJ3:
		dev->vctrl.agc_adj3 = ctrl->val;
		cx88sdr_gain_set(dev);
		break;
	case V4L2_CID_CX88SDR_AGC_TIP3:
		dev->vctrl.agc_tip3 = ctrl->val;
		cx88sdr_gain_set(dev);
		break;
	case V4L2_CID_CX88SDR_INPUT:
		dev->vctrl.input = ctrl->val;
		cx88sdr_input_set(dev);
		break;
	case V4L2_CID_CX88SDR_AFC_PLL:
		dev->vctrl.afc_pll = ctrl->val;
		cx88sdr_input_set(dev);
		break;
	case V4L2_CID_CX88SDR_INPUT_VSYNC:
		dev->vctrl.input_vsync = ctrl->val;
		cx88sdr_input_set(dev);
		break;
	case V4L2_CID_CX88SDR_HTOTAL:
		dev->vctrl.htotal = ctrl->val;
		cx88sdr_input_set(dev);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

const struct v4l2_ctrl_ops cx88sdr_ctrl_ops = {
	.s_ctrl = cx88sdr_s_ctrl,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_gain_6db = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_GAIN_6DB,
	.name	= "Gain +6dB",
	.type	= V4L2_CTRL_TYPE_BOOLEAN,
	.min	= 0,
	.max	= 1,
	.step	= 1,
	.def	= CX88SDR_GAIN_6DB_DEFVAL,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_agc_adj3 = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_AGC_ADJ3,
	.name	= "Gain 2",
	.type	= V4L2_CTRL_TYPE_INTEGER,
	.min	= 0,
	.max	= 16,
	.step	= 1,
	.def	= CX88SDR_AGC_ADJ3_DEFVAL,
	.flags	= V4L2_CTRL_FLAG_SLIDER,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_agc_tip3 = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_AGC_TIP3,
	.name	= "DC Offset",
	.type	= V4L2_CTRL_TYPE_INTEGER,
	.min	= 0,
	.max	= 64,
	.step	= 1,
	.def	= CX88SDR_AGC_TIP3_DEFVAL,
	.flags	= V4L2_CTRL_FLAG_SLIDER,
};

static const char * const cx88sdr_ctrl_input_menu_strings[] = {
	"Input 1",
	"Input 2",
	"Input 3",
	"Input 4",
	NULL,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_input = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_INPUT,
	.name	= "Input",
	.type	= V4L2_CTRL_TYPE_MENU,
	.min	= CX88SDR_INPUT_00,
	.max	= CX88SDR_INPUT_03,
	.def	= CX88SDR_INPUT_DEFVAL,
	.qmenu	= cx88sdr_ctrl_input_menu_strings,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_afc_pll = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_AFC_PLL,
	.name	= "PLL AFC",
	.type	= V4L2_CTRL_TYPE_BOOLEAN,
	.min	= 0,
	.max	= 1,
	.step	= 1,
	.def	= CX88SDR_AFC_PLL_DEFVAL,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_input_vsync = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_INPUT_VSYNC,
	.name	= "Input Vsync",
	.type	= V4L2_CTRL_TYPE_BOOLEAN,
	.min	= 0,
	.max	= 1,
	.step	= 1,
	.def	= CX88SDR_INPUT_VSYNC_DEFVAL,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_htotal = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_HTOTAL,
	.name	= "Pixel Width",
	.type	= V4L2_CTRL_TYPE_INTEGER,
	.min	= 8,
	.max	= 2040,
	.step	= 1,
	.def	= CX88SDR_HTOTAL_DEFVAL,
	.flags	= V4L2_CTRL_FLAG_SLIDER,
};
