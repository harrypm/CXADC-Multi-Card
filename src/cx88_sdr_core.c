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

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/videodev2.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "cx88_sdr.h"

MODULE_DESCRIPTION("CX2388x SDR V4L2 Driver");
MODULE_AUTHOR("Jorge Maidana <jorgem.linux@gmail.com>");
MODULE_LICENSE("GPL");

static int latency = 248;
module_param(latency, int, 0);
MODULE_PARM_DESC(latency, "Set PCI latency timer");

static int cx88sdr_devcount;

static void cx88sdr_pci_lat_set(struct cx88sdr_dev *dev)
{
	u8 lat;

	latency = clamp(latency, 32, 248);
	pci_write_config_byte(dev->pdev, PCI_LATENCY_TIMER, latency);
	pci_read_config_byte(dev->pdev, PCI_LATENCY_TIMER, &lat);
	dev->pci_lat = lat;
}

static void cx88sdr_shutdown(struct cx88sdr_dev *dev)
{
	/* Disable RISC Controller and IRQs */
	ctrl_iowrite32(dev, CX88SDR_DEV_CNTRL2, 0);

	/* Stop DMA transfers */
	ctrl_iowrite32(dev, CX88SDR_VID_DMA_CNTRL, 0);

	/* Stop interrupts */
	ctrl_iowrite32(dev, CX88SDR_PCI_INT_MSK, CX88SDR_PCI_INT_MSK_CLEAR);
	ctrl_iowrite32(dev, CX88SDR_VID_INT_MSK, CX88SDR_VID_INT_MSK_CLEAR);

	/* Stop capturing */
	ctrl_iowrite32(dev, CX88SDR_CAPTURE_CTRL, 0);

	ctrl_iowrite32(dev, CX88SDR_VID_INT_STAT, CX88SDR_VID_INT_STAT_CLEAR);
}

static void cx88sdr_sram_setup(struct cx88sdr_dev *dev)
{
	u32 i;

	/* Write CDT */
	for (i = 0; i < CX88SDR_CDT_SIZE; i++)
		ctrl_iowrite32(dev, CX88SDR_CDT_ADDR + 16 * i,
			       CX88SDR_CLUSTER_BUF_ADDR + CX88SDR_VBI_PACKET_SIZE * i);

	/* Write CMDS */
	ctrl_iowrite32(dev, CX88SDR_DMA24_CMDS_ADDR +  0, dev->risc_buf_addr);
	ctrl_iowrite32(dev, CX88SDR_DMA24_CMDS_ADDR +  4, CX88SDR_CDT_ADDR);
	ctrl_iowrite32(dev, CX88SDR_DMA24_CMDS_ADDR +  8, (CX88SDR_CDT_SIZE * 16) >> 3);
	ctrl_iowrite32(dev, CX88SDR_DMA24_CMDS_ADDR + 12, CX88SDR_RISC_INST_QUEUE_ADDR);
	ctrl_iowrite32(dev, CX88SDR_DMA24_CMDS_ADDR + 16, CX88SDR_RISC_INST_QUEUE_SIZE);

	/* Fill registers */
	ctrl_iowrite32(dev, CX88SDR_DMA24_PTR2, CX88SDR_CDT_ADDR);
	ctrl_iowrite32(dev, CX88SDR_DMA24_CNT1, (CX88SDR_VBI_PACKET_SIZE >> 3) - 1);
	ctrl_iowrite32(dev, CX88SDR_DMA24_CNT2, (CX88SDR_CDT_SIZE * 16) >> 3);
}

static void cx88sdr_adc_setup(struct cx88sdr_dev *dev)
{
	ctrl_iowrite32(dev, CX88SDR_VID_INT_STAT, ctrl_ioread32(dev, CX88SDR_VID_INT_STAT));

	ctrl_iowrite32(dev, CX88SDR_OUTPUT_FORMAT, (1 << 3) | (1 << 2) | (1 << 1));
	ctrl_iowrite32(dev, CX88SDR_COLOR_CTRL, (0xe << 4) | 0xe);
	ctrl_iowrite32(dev, CX88SDR_VBI_PACKET, (CX88SDR_VBI_PACKET_SIZE << 17) | (2 << 11));

	/* Power down audio bandgap DAC+ADC */
	ctrl_iowrite32(dev, CX88SDR_AFE_CFG_IO, (1 << 4) | (1 << 1));

	/* Start DMA */
	ctrl_iowrite32(dev, CX88SDR_DEV_CNTRL2, (1 << 5));
	ctrl_iowrite32(dev, CX88SDR_VID_DMA_CNTRL, (1 << 7) | (1 << 3));
}

static int cx88sdr_alloc_risc_inst_buffer(struct cx88sdr_dev *dev)
{
	dev->risc_buf = dma_alloc_coherent(&dev->pdev->dev,
					   CX88SDR_RISC_BUF_SIZE,
					   &dev->risc_buf_addr,
					   GFP_KERNEL | __GFP_ZERO);
	if (!dev->risc_buf)
		return -ENOMEM;
	return 0;
}

static void cx88sdr_free_risc_inst_buffer(struct cx88sdr_dev *dev)
{
	if (dev->risc_buf) {
		dma_free_coherent(&dev->pdev->dev, CX88SDR_RISC_BUF_SIZE,
				  dev->risc_buf, dev->risc_buf_addr);
		dev->risc_buf = NULL;
		dev->risc_buf_addr = (dma_addr_t)0;
	}
}

static int cx88sdr_alloc_dma_buffer(struct cx88sdr_dev *dev)
{
	u32 page;

	dev->dma_pages_addr = kcalloc(CX88SDR_VBI_DMA_PAGES, sizeof(dma_addr_t), GFP_KERNEL);
	if (!dev->dma_pages_addr)
		return -ENOMEM;

	dev->dma_buf_pages = kcalloc(CX88SDR_VBI_DMA_PAGES, sizeof(void *), GFP_KERNEL);
	if (!dev->dma_buf_pages)
		goto free_dma_pages_addr;

	for (page = 0; page < CX88SDR_VBI_DMA_PAGES; page++) {
		dma_addr_t dma_handle;

		dev->dma_buf_pages[page] = dma_alloc_coherent(&dev->pdev->dev,
							      PAGE_SIZE, &dma_handle,
							      GFP_KERNEL | __GFP_ZERO);
		if (!dev->dma_buf_pages[page])
			goto free_dma_buf_pages;
		dev->dma_pages_addr[page] = dma_handle;
	}
	return 0;

free_dma_buf_pages:
	while (page) {
		page--;
		dma_free_coherent(&dev->pdev->dev, PAGE_SIZE,
				  dev->dma_buf_pages[page],
				  dev->dma_pages_addr[page]);
		dev->dma_buf_pages[page] = NULL;
		dev->dma_pages_addr[page] = (dma_addr_t)0;
	}
	kfree(dev->dma_buf_pages);
	dev->dma_buf_pages = NULL;
free_dma_pages_addr:
	kfree(dev->dma_pages_addr);
	dev->dma_pages_addr = NULL;
	return -ENOMEM;
}

static void cx88sdr_free_dma_buffer(struct cx88sdr_dev *dev)
{
	u32 page;

	for (page = 0; page < CX88SDR_VBI_DMA_PAGES; page++) {
		if (dev->dma_buf_pages[page]) {
			dma_free_coherent(&dev->pdev->dev, PAGE_SIZE,
					  dev->dma_buf_pages[page],
					  dev->dma_pages_addr[page]);
			dev->dma_buf_pages[page] = NULL;
			dev->dma_pages_addr[page] = (dma_addr_t)0;
		}
	}
	kfree(dev->dma_buf_pages);
	dev->dma_buf_pages = NULL;
	kfree(dev->dma_pages_addr);
	dev->dma_pages_addr = NULL;
}

static void cx88sdr_make_risc_instructions(struct cx88sdr_dev *dev)
{
	uint32_t *risc_buf = dev->risc_buf;
	uint32_t risc_loop_addr = dev->risc_buf_addr + sizeof(uint32_t);
	uint32_t page, irq_cnt = 0;

	*risc_buf++ = CX88SDR_RISC_SYNC | CX88SDR_RISC_CNT_RESET;

	for (page = 0; page < CX88SDR_VBI_DMA_PAGES; page++) {
		uint32_t dma_addr = dev->dma_pages_addr[page];

		irq_cnt++;
		irq_cnt &= CX88SDR_RISC_IRQ1_CNT_MASK;

		*risc_buf++ = CX88SDR_RISC_WRITE_VBI_PACKET;
		*risc_buf++ = dma_addr;

		*risc_buf++ = CX88SDR_RISC_WRITE_VBI_PACKET | ((irq_cnt) ?
			      CX88SDR_RISC_IRQ1_NOOP : CX88SDR_RISC_IRQ1_TRIG) |
			      ((page < CX88SDR_VBI_DMA_PAGES - 1) ?
			      CX88SDR_RISC_CNT_INCR : CX88SDR_RISC_CNT_RESET);
		*risc_buf++ = dma_addr + CX88SDR_VBI_PACKET_SIZE;
	}
	*risc_buf++ = CX88SDR_RISC_JUMP;
	*risc_buf++ = risc_loop_addr;

	cx88sdr_pr_info("RISC memory usage: %u/%luK, DMA: %uM\n",
		       (uint32_t)(((void *)risc_buf - (void *)dev->risc_buf) / SZ_1K),
		       CX88SDR_RISC_BUF_SIZE / SZ_1K, CX88SDR_VBI_DMA_SIZE / SZ_1M);
}

static irqreturn_t cx88sdr_irq(int __always_unused irq, void *dev_id)
{
	struct cx88sdr_dev *dev = dev_id;
	int i, handled = 0;
	uint32_t mask, status;

	for (i = 0; i < 10; i++) {
		status = ctrl_ioread32(dev, CX88SDR_VID_INT_STAT);
		mask = ctrl_ioread32(dev, CX88SDR_VID_INT_MSK);
		if ((status & mask) == 0)
			break;
		ctrl_iowrite32(dev, CX88SDR_VID_INT_STAT, status);
		handled = 1;
	}
	return IRQ_RETVAL(handled);
}

static int cx88sdr_probe(struct pci_dev *pdev,
			 const struct pci_device_id __always_unused *pci_id)
{
	struct cx88sdr_dev *dev;
	struct v4l2_device *v4l2_dev;
	struct v4l2_ctrl_handler *hdl;
	int ret;

	if (cx88sdr_devcount >= CX88SDR_MAX_CARDS)
		return -ENODEV;

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(32))) {
		dev_err(&pdev->dev, "no suitable DMA support available\n");
		ret = -EFAULT;
		goto disable_device;
	}

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "can't allocate memory\n");
		goto disable_device;
	}

	dev->nr = cx88sdr_devcount;
	dev->pdev = pdev;

	cx88sdr_pci_lat_set(dev);

	ret = pci_request_regions(pdev, KBUILD_MODNAME);
	if (ret) {
		cx88sdr_pr_err("can't request memory regions\n");
		goto disable_device;
	}

	ret = cx88sdr_alloc_risc_inst_buffer(dev);
	if (ret) {
		cx88sdr_pr_err("can't alloc risc buffers\n");
		goto free_pci_regions;
	}

	ret = cx88sdr_alloc_dma_buffer(dev);
	if (ret) {
		cx88sdr_pr_err("can't alloc DMA buffers\n");
		goto free_risc_inst_buffer;
	}

	cx88sdr_make_risc_instructions(dev);

	dev->ctrl = pci_ioremap_bar(pdev, 0);
	if (dev->ctrl == NULL) {
		ret = -ENODEV;
		cx88sdr_pr_err("can't ioremap BAR 0\n");
		goto free_dma_buffer;
	}

	cx88sdr_shutdown(dev);

	cx88sdr_sram_setup(dev);

	ret = request_irq(pdev->irq, cx88sdr_irq, IRQF_SHARED, KBUILD_MODNAME, dev);
	if (ret) {
		cx88sdr_pr_err("failed to request IRQ\n");
		goto free_ctrl;
	}

	dev->irq = pdev->irq;
	synchronize_irq(dev->irq);

	/* Set initial values */
	dev->vctrl.gain        = CX88SDR_GAIN_DEFVAL;
	dev->vctrl.gain_6db    = CX88SDR_GAIN_6DB_DEFVAL;
	dev->vctrl.agc_adj3    = CX88SDR_AGC_ADJ3_DEFVAL;
	dev->vctrl.agc_tip3    = CX88SDR_AGC_TIP3_DEFVAL;
	dev->vctrl.input       = CX88SDR_INPUT_DEFVAL;

	/* Options for Raw Video (UltraLock ON, HLOCK = 1) */
	dev->vctrl.afc_pll     = CX88SDR_AFC_PLL_DEFVAL;
	dev->vctrl.input_vsync = CX88SDR_INPUT_VSYNC_DEFVAL;
	dev->vctrl.htotal      = CX88SDR_HTOTAL_DEFVAL;

	dev->vctrl.freq        = CX88SDR_ADC_FREQ_DEFVAL;
	dev->vctrl.pixelformat = V4L2_SDR_FMT_RU8;
	dev->vctrl.buffersize  = PAGE_SIZE;

	snprintf(dev->name, sizeof(dev->name), CX88SDR_DRV_NAME " [%d]", dev->nr);

	cx88sdr_adc_setup(dev);
	ret = cx88sdr_adc_fmt_set(dev);
	if (ret) {
		cx88sdr_pr_err("failed to config ADC\n");
		goto free_irq;
	}

	cx88sdr_gain_set(dev);
	cx88sdr_input_set(dev);

	mutex_init(&dev->vdev_mlock);
	mutex_init(&dev->vopen_mlock);

	v4l2_dev = &dev->v4l2_dev;
	ret = v4l2_device_register(&pdev->dev, v4l2_dev);
	if (ret) {
		v4l2_err(v4l2_dev, "can't register V4L2 device\n");
		goto free_irq;
	}

	hdl = &dev->ctrl_handler;
	v4l2_ctrl_handler_init(hdl, 8);
	v4l2_ctrl_new_std(hdl, &cx88sdr_ctrl_ops, V4L2_CID_GAIN, 0, 31, 1, dev->vctrl.gain);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_gain_6db, NULL);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_agc_adj3, NULL);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_agc_tip3, NULL);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_input, NULL);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_afc_pll, NULL);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_input_vsync, NULL);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_htotal, NULL);
	v4l2_dev->ctrl_handler = hdl;
	if (hdl->error) {
		ret = hdl->error;
		v4l2_err(v4l2_dev, "can't register V4L2 controls\n");
		goto free_v4l2;
	}

	/* Initialize the video_device structure */
	strscpy(v4l2_dev->name, dev->name, sizeof(v4l2_dev->name));
	dev->vdev = cx88sdr_template;
	dev->vdev.ctrl_handler = &dev->ctrl_handler;
	dev->vdev.lock = &dev->vdev_mlock;
	dev->vdev.v4l2_dev = v4l2_dev;
	video_set_drvdata(&dev->vdev, dev);

	ret = video_register_device(&dev->vdev, VFL_TYPE_SDR, -1);
	if (ret)
		goto free_v4l2;

	cx88sdr_pr_info("IRQ: %u, Control MMIO: 0x%p, PCI latency: %d, Xtal: %uHz\n",
			dev->pdev->irq, dev->ctrl, dev->pci_lat, (u32)CX88SDR_XTAL_FREQ);
	cx88sdr_pr_info("registered as %s\n",
			video_device_node_name(&dev->vdev));

	ctrl_iowrite32(dev, CX88SDR_VID_INT_MSK, CX88SDR_VID_INT_MSK_VAL);
	cx88sdr_devcount++;
	return 0;

free_v4l2:
	v4l2_ctrl_handler_free(hdl);
	v4l2_device_unregister(v4l2_dev);
free_irq:
	free_irq(dev->irq, dev);
free_ctrl:
	iounmap(dev->ctrl);
free_dma_buffer:
	cx88sdr_free_dma_buffer(dev);
free_risc_inst_buffer:
	cx88sdr_free_risc_inst_buffer(dev);
free_pci_regions:
	pci_release_regions(pdev);
disable_device:
	pci_disable_device(pdev);
	return ret;
}

static void cx88sdr_remove(struct pci_dev *pdev)
{
	struct v4l2_device *v4l2_dev = pci_get_drvdata(pdev);
	struct cx88sdr_dev *dev = container_of(v4l2_dev, struct cx88sdr_dev, v4l2_dev);

	cx88sdr_shutdown(dev);

	cx88sdr_pr_info("removing %s\n", video_device_node_name(&dev->vdev));

	cx88sdr_devcount--;

	video_unregister_device(&dev->vdev);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	v4l2_device_unregister(&dev->v4l2_dev);

	/* Release resources */
	free_irq(dev->irq, dev);
	iounmap(dev->ctrl);
	cx88sdr_free_dma_buffer(dev);
	cx88sdr_free_risc_inst_buffer(dev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static int __maybe_unused cx88sdr_suspend(struct device *dev_d)
{
	struct v4l2_device *v4l2_dev = dev_get_drvdata(dev_d);
	struct cx88sdr_dev *dev = container_of(v4l2_dev, struct cx88sdr_dev, v4l2_dev);

	cx88sdr_shutdown(dev);
	return 0;
}

static int __maybe_unused cx88sdr_resume(struct device *dev_d)
{
	struct v4l2_device *v4l2_dev = dev_get_drvdata(dev_d);
	struct cx88sdr_dev *dev = container_of(v4l2_dev, struct cx88sdr_dev, v4l2_dev);
	int ret;

	cx88sdr_shutdown(dev);
	cx88sdr_sram_setup(dev);
	cx88sdr_adc_setup(dev);
	ret = cx88sdr_adc_fmt_set(dev);
	if (ret)
		return ret;
	cx88sdr_gain_set(dev);
	cx88sdr_input_set(dev);
	ctrl_iowrite32(dev, CX88SDR_VID_INT_MSK, CX88SDR_VID_INT_MSK_VAL);

	mutex_lock(&dev->vopen_mlock);
	if (dev->vopen)
		ctrl_iowrite32(dev, CX88SDR_PCI_INT_MSK, CX88SDR_PCI_INT_MSK_VAL);
	mutex_unlock(&dev->vopen_mlock);
	return 0;
}

static const struct pci_device_id cx88sdr_pci_tbl[] = {
	{ PCI_DEVICE(0x14f1, 0x8800) },
	{ }
};
MODULE_DEVICE_TABLE(pci, cx88sdr_pci_tbl);

static SIMPLE_DEV_PM_OPS(cx88sdr_pm_ops, cx88sdr_suspend, cx88sdr_resume);

static struct pci_driver cx88sdr_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= cx88sdr_pci_tbl,
	.probe		= cx88sdr_probe,
	.remove		= cx88sdr_remove,
	.driver.pm	= &cx88sdr_pm_ops,
};

module_pci_driver(cx88sdr_pci_driver);
