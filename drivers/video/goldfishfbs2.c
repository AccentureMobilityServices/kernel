/* drivers/video/goldfishfbs2.c
 *
 * Copyright (C) 2011 Accenture
 *
 * Modified from Google goldfish fb driver to provide a second display
 */

/* drivers/video/goldfishfb.c
**
** Copyright (C) 2007 Google, Inc.
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#ifdef CONFIG_ANDROID_POWER
#include <linux/android_power.h>
#endif


#define MDEBUG

#include <mach/hardware.h>

enum {
	FB_GET_WIDTH        = 0x00,
	FB_GET_HEIGHT       = 0x04,
	FB_INT_STATUS       = 0x08,
	FB_INT_ENABLE       = 0x0c,
	FB_SET_BASE         = 0x10,
	FB_SET_ROTATION     = 0x14,
	FB_SET_BLANK        = 0x18,
	FB_GET_PHYS_WIDTH   = 0x1c,
	FB_GET_PHYS_HEIGHT  = 0x20,

	FB_INT_VSYNC             = 1U << 0,
	FB_INT_BASE_UPDATE_DONE  = 1U << 1
};

struct goldfish_fb {
	uint32_t reg_base;
	int irq;
	spinlock_t lock;
	wait_queue_head_t wait;
	int base_update_count;
	int rotation;
	struct fb_info fb;
	u32			cmap[16];
#ifdef CONFIG_ANDROID_POWER
        android_early_suspend_t early_suspend;
#endif
};

static irqreturn_t
goldfish_fbs2_interrupt(int irq, void *dev_id)
{
	unsigned long irq_flags;
	struct goldfish_fb	*fb = dev_id;
	uint32_t status;

	spin_lock_irqsave(&fb->lock, irq_flags);
	status = readl(fb->reg_base + FB_INT_STATUS);
	if(status & FB_INT_BASE_UPDATE_DONE) {
		fb->base_update_count++;
		wake_up(&fb->wait);
	}
	spin_unlock_irqrestore(&fb->lock, irq_flags);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

static inline u32 convert_bitfield(int val, struct fb_bitfield *bf)
{
	unsigned int mask = (1 << bf->length) - 1;

	return (val >> (16 - bf->length) & mask) << bf->offset;
}

static int
goldfish_fbs2_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
		 unsigned int blue, unsigned int transp, struct fb_info *info)
{
	struct goldfish_fb *fb = container_of(info, struct goldfish_fb, fb);

	if (regno < 16) {
		fb->cmap[regno] = convert_bitfield(transp, &fb->fb.var.transp) |
				  convert_bitfield(blue, &fb->fb.var.blue) |
				  convert_bitfield(green, &fb->fb.var.green) |
				  convert_bitfield(red, &fb->fb.var.red);
		return 0;
	}
	else {
		return 1;
	}
}

static int goldfish_fbs2_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if((var->rotate & 1) != (info->var.rotate & 1)) {
		if((var->xres != info->var.yres) ||
		   (var->yres != info->var.xres) ||
		   (var->xres_virtual != info->var.yres) ||
		   (var->yres_virtual > info->var.xres * 2) ||
		   (var->yres_virtual < info->var.xres )) {
			return -EINVAL;
		}
	}
	else {
		if((var->xres != info->var.xres) ||
		   (var->yres != info->var.yres) ||
		   (var->xres_virtual != info->var.xres) ||
		   (var->yres_virtual > info->var.yres * 2) ||
		   (var->yres_virtual < info->var.yres )) {
			return -EINVAL;
		}
	}
	if((var->xoffset != info->var.xoffset) ||
	   (var->bits_per_pixel != info->var.bits_per_pixel) ||
	   (var->grayscale != info->var.grayscale)) {
		return -EINVAL;
	}
	return 0;
}

static int goldfish_fbs2_set_par(struct fb_info *info)
{
	struct goldfish_fb *fb = container_of(info, struct goldfish_fb, fb);
	if(fb->rotation != fb->fb.var.rotate) {
		info->fix.line_length = info->var.xres * 2;
		fb->rotation = fb->fb.var.rotate;
		writel(fb->rotation, fb->reg_base + FB_SET_ROTATION);
	}
	return 0;
}


static int goldfish_fbs2_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
	unsigned long irq_flags;
	int base_update_count;
	struct goldfish_fb *fb = container_of(info, struct goldfish_fb, fb);

	spin_lock_irqsave(&fb->lock, irq_flags);
	base_update_count = fb->base_update_count;
	writel(fb->fb.fix.smem_start + fb->fb.var.xres * 2 * var->yoffset, fb->reg_base + FB_SET_BASE);
	spin_unlock_irqrestore(&fb->lock, irq_flags);
	wait_event_timeout(fb->wait, fb->base_update_count != base_update_count, HZ / 15);
	if(fb->base_update_count == base_update_count)
		printk("goldfish_fb_pan_display: timeout wating for base update\n");
	return 0;
}

#ifdef CONFIG_ANDROID_POWER
static void goldfish_fbs2_early_suspend(android_early_suspend_t *h)
{
	struct goldfish_fb *fb = container_of(h, struct goldfish_fb, early_suspend);
	writel(1, fb->reg_base + FB_SET_BLANK);
}

static void goldfish_fbs2_late_resume(android_early_suspend_t *h)
{
	struct goldfish_fb *fb = container_of(h, struct goldfish_fb, early_suspend);
        writel(0, fb->reg_base + FB_SET_BLANK);
}
#endif

static struct fb_ops goldfish_fbs2_ops = {
	.owner          = THIS_MODULE,
	.fb_check_var   = goldfish_fbs2_check_var,
	.fb_set_par     = goldfish_fbs2_set_par,
	.fb_setcolreg   = goldfish_fbs2_setcolreg,
	.fb_pan_display = goldfish_fbs2_pan_display,
	.fb_fillrect    = cfb_fillrect,
	.fb_copyarea    = cfb_copyarea,
	.fb_imageblit   = cfb_imageblit,
};


static int goldfish_fbs2_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *r;
	struct goldfish_fb *fb;
	size_t framesize;
	uint32_t width, height;
	dma_addr_t fbpaddr;

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if(fb == NULL) {
		ret = -ENOMEM;
		goto err_fb_alloc_failed;
	}
	spin_lock_init(&fb->lock);
	init_waitqueue_head(&fb->wait);
	platform_set_drvdata(pdev, fb);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(r == NULL) {
		ret = -ENODEV;
		goto err_no_io_base;
	}
	fb->reg_base = IO_ADDRESS(r->start - IO_START);

	fb->irq = platform_get_irq(pdev, 0);
	if(fb->irq < 0) {
		ret = -ENODEV;
		goto err_no_irq;
	}

	width = readl(fb->reg_base + FB_GET_WIDTH);
	height = readl(fb->reg_base + FB_GET_HEIGHT);

//dm+++
#ifdef MDEBUG
	printk("goldfish_fbs2 --- SCREEN2--- IO_ADDRESS  reg_base %x %d * %d\n", fb->reg_base, width, height);
#endif

	fb->fb.fbops		= &goldfish_fbs2_ops;
	fb->fb.flags		= FBINFO_FLAG_DEFAULT;
	fb->fb.pseudo_palette	= fb->cmap;
	//strncpy(fb->fb.fix.id, clcd_name, sizeof(fb->fb.fix.id));
	fb->fb.fix.type		= FB_TYPE_PACKED_PIXELS;
	fb->fb.fix.visual = FB_VISUAL_TRUECOLOR;
	fb->fb.fix.line_length = width * 2;
	fb->fb.fix.accel	= FB_ACCEL_NONE;
	fb->fb.fix.ypanstep = 1;

	fb->fb.var.xres		= width;
	fb->fb.var.yres		= height;
	fb->fb.var.xres_virtual	= width;
	fb->fb.var.yres_virtual	= height * 2;
	fb->fb.var.bits_per_pixel = 16;
	fb->fb.var.activate	= FB_ACTIVATE_NOW;
	fb->fb.var.height	= readl(fb->reg_base + FB_GET_PHYS_HEIGHT);
	fb->fb.var.width	= readl(fb->reg_base + FB_GET_PHYS_WIDTH);

	fb->fb.var.red.offset = 11;
	fb->fb.var.red.length = 5;
	fb->fb.var.green.offset = 5;
	fb->fb.var.green.length = 6;
	fb->fb.var.blue.offset = 0;
	fb->fb.var.blue.length = 5;

	framesize = width * height * 2 * 2;
	fb->fb.screen_base = dma_alloc_writecombine(&pdev->dev, framesize,
	                                            &fbpaddr, GFP_KERNEL);
	printk("allocating frame buffer %d * %d, got %p\n", width, height, fb->fb.screen_base);
	if(fb->fb.screen_base == 0) {
		ret = -ENOMEM;
		goto err_alloc_screen_base_failed;
	}
	fb->fb.fix.smem_start = fbpaddr;
	fb->fb.fix.smem_len = framesize;

	ret = fb_set_var(&fb->fb, &fb->fb.var);
	if(ret)
		goto err_fb_set_var_failed;

	ret = request_irq(fb->irq, goldfish_fbs2_interrupt, IRQF_SHARED, pdev->name, fb);
	if(ret)
		goto err_request_irq_failed;

	writel(FB_INT_BASE_UPDATE_DONE, fb->reg_base + FB_INT_ENABLE);
	goldfish_fbs2_pan_display(&fb->fb.var, &fb->fb); // updates base

	ret = register_framebuffer(&fb->fb);
	if(ret)
		goto err_register_framebuffer_failed;

#ifdef CONFIG_ANDROID_POWER
	fb->early_suspend.suspend = goldfish_fbs2_early_suspend;
	fb->early_suspend.resume = goldfish_fbs2_late_resume;
	android_register_early_suspend(&fb->early_suspend);
#endif

	return 0;


err_register_framebuffer_failed:
	free_irq(fb->irq, fb);
err_request_irq_failed:
err_fb_set_var_failed:
	dma_free_writecombine(&pdev->dev, framesize, fb->fb.screen_base, fb->fb.fix.smem_start);
err_alloc_screen_base_failed:
err_no_irq:
err_no_io_base:
	kfree(fb);
err_fb_alloc_failed:
	return ret;
}

static int goldfish_fbs2_remove(struct platform_device *pdev)
{
	size_t framesize;
	struct goldfish_fb *fb = platform_get_drvdata(pdev);
	
	framesize = fb->fb.var.xres_virtual * fb->fb.var.yres_virtual * 2;

#ifdef CONFIG_ANDROID_POWER
        android_unregister_early_suspend(&fb->early_suspend);
#endif
	unregister_framebuffer(&fb->fb);
	free_irq(fb->irq, fb);
	dma_free_writecombine(&pdev->dev, framesize, fb->fb.screen_base, fb->fb.fix.smem_start);
	kfree(fb);
	return 0;
}


static struct platform_driver goldfish_fbs2_driver = {
	.probe		= goldfish_fbs2_probe,
	.remove		= goldfish_fbs2_remove,
	.driver = {
		.name = "goldfish_fbs2"
	}
};

static int __init goldfish_fbs2_init(void)
{
	printk("goldfish_fbs2_init\n");

	return platform_driver_register(&goldfish_fbs2_driver);
}

static void __exit goldfish_fbs2_exit(void)
{
	platform_driver_unregister(&goldfish_fbs2_driver);
}

module_init(goldfish_fbs2_init);
module_exit(goldfish_fbs2_exit);

