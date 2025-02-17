/* drivers/char/goldfish_tty.c
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

#include <linux/console.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#include <mach/hardware.h>
#include <asm/io.h>

enum {
	GOLDFISH_TTY_PUT_CHAR       = 0x00,
	GOLDFISH_TTY_BYTES_READY    = 0x04,
	GOLDFISH_TTY_CMD            = 0x08,

	GOLDFISH_TTY_DATA_PTR       = 0x10,
	GOLDFISH_TTY_DATA_LEN       = 0x14,

	GOLDFISH_TTY_CMD_INT_DISABLE    = 0,
	GOLDFISH_TTY_CMD_INT_ENABLE     = 1,
	GOLDFISH_TTY_CMD_WRITE_BUFFER   = 2,
	GOLDFISH_TTY_CMD_READ_BUFFER    = 3,
};

struct goldfish_tty {
	spinlock_t lock;
	uint32_t base;
	uint32_t irq;
	int opencount;
	struct tty_struct *tty;
	struct console console;
};

static DEFINE_MUTEX(goldfish_tty_lock);
static struct tty_driver *goldfish_tty_driver;
static uint32_t goldfish_tty_line_count = 8;
static uint32_t goldfish_tty_current_line_count;
static struct goldfish_tty *goldfish_ttys;

static void goldfish_tty_do_write(int line, const char *buf, unsigned count)
{
	unsigned long irq_flags;
	struct goldfish_tty *qtty = &goldfish_ttys[line];
	uint32_t base = qtty->base;
	spin_lock_irqsave(&qtty->lock, irq_flags);
	writel(buf, base + GOLDFISH_TTY_DATA_PTR);
	writel(count, base + GOLDFISH_TTY_DATA_LEN);
	writel(GOLDFISH_TTY_CMD_WRITE_BUFFER, base + GOLDFISH_TTY_CMD);
	spin_unlock_irqrestore(&qtty->lock, irq_flags);
}

static irqreturn_t goldfish_tty_interrupt(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct goldfish_tty *qtty = &goldfish_ttys[pdev->id];
	uint32_t base = qtty->base;
	unsigned long irq_flags;
	unsigned char *buf;
	uint32_t count;

	count = readl(base + GOLDFISH_TTY_BYTES_READY);
	if(count == 0) {
		return IRQ_NONE;
	}
	count = tty_prepare_flip_string(qtty->tty, &buf, count);
	spin_lock_irqsave(&qtty->lock, irq_flags);
	writel(buf, base + GOLDFISH_TTY_DATA_PTR);
	writel(count, base + GOLDFISH_TTY_DATA_LEN);
	writel(GOLDFISH_TTY_CMD_READ_BUFFER, base + GOLDFISH_TTY_CMD);
	spin_unlock_irqrestore(&qtty->lock, irq_flags);
	tty_schedule_flip(qtty->tty);
	return IRQ_HANDLED;
}

static int goldfish_tty_open(struct tty_struct * tty, struct file * filp)
{
	int ret;
	struct goldfish_tty *qtty = &goldfish_ttys[tty->index];

	mutex_lock(&goldfish_tty_lock);
	if(qtty->tty == NULL || qtty->tty == tty) {
		if(qtty->opencount++ == 0) {
			qtty->tty = tty;
			writel(GOLDFISH_TTY_CMD_INT_ENABLE, qtty->base + GOLDFISH_TTY_CMD);
		}
		ret = 0;
	}
	else
		ret = -EBUSY;
	mutex_unlock(&goldfish_tty_lock);
	return ret;
}

static void goldfish_tty_close(struct tty_struct * tty, struct file * filp)
{
	struct goldfish_tty *qtty = &goldfish_ttys[tty->index];

	mutex_lock(&goldfish_tty_lock);
	if(qtty->tty == tty) {
		if(--qtty->opencount == 0) {
			writel(GOLDFISH_TTY_CMD_INT_DISABLE, qtty->base + GOLDFISH_TTY_CMD);
			qtty->tty = NULL;
		}
	}
	mutex_unlock(&goldfish_tty_lock);
}

static int goldfish_tty_write(struct tty_struct * tty, const unsigned char *buf, int count)
{
	goldfish_tty_do_write(tty->index, buf, count);
	return count;
}

static int goldfish_tty_write_room(struct tty_struct *tty)
{
	return 0x10000;
}

static int goldfish_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct goldfish_tty *qtty = &goldfish_ttys[tty->index];
	uint32_t base = qtty->base;
	return readl(base + GOLDFISH_TTY_BYTES_READY);
}

static void goldfish_tty_console_write(struct console *co, const char *b, unsigned count)
{
	goldfish_tty_do_write(co->index, b, count);
}

static struct tty_driver *goldfish_tty_console_device(struct console *c, int *index)
{
	*index = c->index;
	return goldfish_tty_driver;
}

static int goldfish_tty_console_setup(struct console *co, char *options)
{
	if((unsigned)co->index > goldfish_tty_line_count)
		return -ENODEV;
	if(goldfish_ttys[co->index].base == 0)
		return -ENODEV;
	return 0;
}

static struct tty_operations goldfish_tty_ops = {
	.open = goldfish_tty_open,
	.close = goldfish_tty_close,
	.write = goldfish_tty_write,
	.write_room = goldfish_tty_write_room,
	.chars_in_buffer = goldfish_tty_chars_in_buffer,
};

static int __devinit goldfish_tty_create_driver(void)
{
	int ret;
	struct tty_driver *tty;

	goldfish_ttys = kzalloc(sizeof(*goldfish_ttys) * goldfish_tty_line_count, GFP_KERNEL);
	if(goldfish_ttys == NULL) {
		ret = -ENOMEM;
		goto err_alloc_goldfish_ttys_failed;
	}

	tty = alloc_tty_driver(goldfish_tty_line_count);
	if(tty == NULL) {
		ret = -ENOMEM;
		goto err_alloc_tty_driver_failed;
	}
	tty->driver_name = "goldfish";
	tty->name = "ttyS";
	tty->type = TTY_DRIVER_TYPE_SERIAL;
	tty->subtype = SERIAL_TYPE_NORMAL;
	tty->init_termios = tty_std_termios;
	tty->flags = TTY_DRIVER_RESET_TERMIOS | TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV;
	tty_set_operations(tty, &goldfish_tty_ops);
	ret = tty_register_driver(tty);
	if(ret)
		goto err_tty_register_driver_failed;

	goldfish_tty_driver = tty;
	return 0;

err_tty_register_driver_failed:
	put_tty_driver(tty);
err_alloc_tty_driver_failed:
	kfree(goldfish_ttys);
	goldfish_ttys = NULL;
err_alloc_goldfish_ttys_failed:
	return ret;
}

static void goldfish_tty_delete_driver(void)
{
	tty_unregister_driver(goldfish_tty_driver);
	put_tty_driver(goldfish_tty_driver);
	goldfish_tty_driver = NULL;
	kfree(goldfish_ttys);
	goldfish_ttys = NULL;
}

static int __devinit goldfish_tty_probe(struct platform_device *pdev)
{
	int ret;
	int i;
	struct resource *r;
	struct device *ttydev;
	uint32_t base;
	uint32_t irq;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(r == NULL)
		return -EINVAL;
	base = IO_ADDRESS(r->start - IO_START);
	r = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if(r == NULL)
		return -EINVAL;
	irq = r->start;

	if(pdev->id >= goldfish_tty_line_count)
		return -EINVAL;

	mutex_lock(&goldfish_tty_lock);
	if(goldfish_tty_current_line_count == 0) {
		ret = goldfish_tty_create_driver();
		if(ret)
			goto err_create_driver_failed;
	}
	goldfish_tty_current_line_count++;

	spin_lock_init(&goldfish_ttys[pdev->id].lock);
	goldfish_ttys[pdev->id].base = base;
	goldfish_ttys[pdev->id].irq = irq;

	writel(GOLDFISH_TTY_CMD_INT_DISABLE, base + GOLDFISH_TTY_CMD);

	ret = request_irq(irq, goldfish_tty_interrupt, IRQF_SHARED, "goldfish_tty", pdev);
	if(ret)
		goto err_request_irq_failed;


	ttydev = tty_register_device(goldfish_tty_driver, pdev->id, NULL);
	if(IS_ERR(ttydev)) {
		ret = PTR_ERR(ttydev);
		goto err_tty_register_device_failed;
	}

	strcpy(goldfish_ttys[pdev->id].console.name, "ttyS");
	goldfish_ttys[pdev->id].console.write		= goldfish_tty_console_write;
	goldfish_ttys[pdev->id].console.device		= goldfish_tty_console_device;
	goldfish_ttys[pdev->id].console.setup		= goldfish_tty_console_setup;
	goldfish_ttys[pdev->id].console.flags		= CON_PRINTBUFFER;
	goldfish_ttys[pdev->id].console.index		= pdev->id;
	register_console(&goldfish_ttys[pdev->id].console);


	mutex_unlock(&goldfish_tty_lock);

	return 0;

	tty_unregister_device(goldfish_tty_driver, i);
err_tty_register_device_failed:
	free_irq(irq, pdev);
err_request_irq_failed:
	goldfish_tty_current_line_count--;
	if(goldfish_tty_current_line_count == 0) {
		goldfish_tty_delete_driver();
	}
err_create_driver_failed:
	mutex_unlock(&goldfish_tty_lock);
	return ret;
}

static int __devexit goldfish_tty_remove(struct platform_device *pdev)
{
	mutex_lock(&goldfish_tty_lock);
	unregister_console(&goldfish_ttys[pdev->id].console);
	tty_unregister_device(goldfish_tty_driver, pdev->id);
	goldfish_ttys[pdev->id].base = 0;
	free_irq(goldfish_ttys[pdev->id].irq, pdev);
	goldfish_tty_current_line_count--;
	if(goldfish_tty_current_line_count == 0) {
		goldfish_tty_delete_driver();
	}
	mutex_unlock(&goldfish_tty_lock);
	return 0;
}

static struct platform_driver goldfish_tty_platform_driver = {
	.probe = goldfish_tty_probe,
	.remove = __devexit_p(goldfish_tty_remove),
	.driver = {
		.name = "goldfish_tty"
	}
};

static int __init goldfish_tty_init(void)
{
	return platform_driver_register(&goldfish_tty_platform_driver);
}

static void __exit goldfish_tty_exit(void)
{
	platform_driver_unregister(&goldfish_tty_platform_driver);
}

module_init(goldfish_tty_init);
module_exit(goldfish_tty_exit);
