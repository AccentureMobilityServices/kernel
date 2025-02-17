/* arch/arm/mach-goldfish/audio.c
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
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/interrupt.h>

#include <asm/types.h>
#include <asm/io.h>
#include <asm/uaccess.h>


MODULE_AUTHOR("Google, Inc.");
MODULE_DESCRIPTION("Android QEMU Audio Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

struct goldfish_audio {
	uint32_t reg_base;
	int irq;
	spinlock_t lock;
	wait_queue_head_t wait;
	
	char __iomem *buffer_virt;      /* combined buffer virtual address */
	unsigned long buffer_phys;      /* combined buffer physical address */

	char __iomem *write_buffer1;    /* write buffer 1 virtual address */
	char __iomem *write_buffer2;    /* write buffer 2 virtual address */
	char __iomem *read_buffer;      /* read buffer virtual address */
	int buffer_status;
	int read_supported;         /* true if we have audio input support */
};

/* We will allocate two read buffers and two write buffers.
   Having two read buffers facilitate stereo -> mono conversion.
   Having two write buffers facilitate interleaved IO.
*/
#define READ_BUFFER_SIZE        16384
#define WRITE_BUFFER_SIZE       16384
#define COMBINED_BUFFER_SIZE    ((2 * READ_BUFFER_SIZE) + (2 * WRITE_BUFFER_SIZE))

#define GOLDFISH_AUDIO_READ(data, addr)   (readl(data->reg_base + addr))
#define GOLDFISH_AUDIO_WRITE(data, addr, x)   (writel(x, data->reg_base + addr))

/* temporary variable used between goldfish_audio_probe() and goldfish_audio_open() */
static struct goldfish_audio* audio_data;

enum {
	/* audio status register */
	AUDIO_INT_STATUS	= 0x00, 
	/* set this to enable IRQ */
	AUDIO_INT_ENABLE	= 0x04,
	/* set these to specify buffer addresses */
	AUDIO_SET_WRITE_BUFFER_1 = 0x08,
	AUDIO_SET_WRITE_BUFFER_2 = 0x0C,
	/* set number of bytes in buffer to write */
	AUDIO_WRITE_BUFFER_1  = 0x10,
	AUDIO_WRITE_BUFFER_2  = 0x14,

	/* true if audio input is supported */
	AUDIO_READ_SUPPORTED = 0x18,
	/* buffer to use for audio input */
	AUDIO_SET_READ_BUFFER = 0x1C,
	
	/* driver writes number of bytes to read */
	AUDIO_START_READ  = 0x20,

	/* number of bytes available in read buffer */
	AUDIO_READ_BUFFER_AVAILABLE  = 0x24,

	/* AUDIO_INT_STATUS bits */
	
	/* this bit set when it is safe to write more bytes to the buffer */
	AUDIO_INT_WRITE_BUFFER_1_EMPTY	= 1U << 0,
	AUDIO_INT_WRITE_BUFFER_2_EMPTY	= 1U << 1,
	AUDIO_INT_READ_BUFFER_FULL      = 1U << 2,
	
	AUDIO_INT_MASK                  = AUDIO_INT_WRITE_BUFFER_1_EMPTY | 
	                                  AUDIO_INT_WRITE_BUFFER_2_EMPTY | 
	                                  AUDIO_INT_READ_BUFFER_FULL,
};


static atomic_t open_count = ATOMIC_INIT(0);


static ssize_t goldfish_audio_read(struct file *fp, char __user *buf,
							size_t count, loff_t *pos)
{
	struct goldfish_audio* data = fp->private_data;
	int length;
	int result = 0;
	
	if (!data->read_supported)
		return -ENODEV;

	while (count > 0) {
		length = (count > READ_BUFFER_SIZE ? READ_BUFFER_SIZE : count);
		GOLDFISH_AUDIO_WRITE(data, AUDIO_START_READ, length);

		wait_event_interruptible(data->wait, (data->buffer_status & AUDIO_INT_READ_BUFFER_FULL));

		length = GOLDFISH_AUDIO_READ(data, AUDIO_READ_BUFFER_AVAILABLE);
   
		/* copy data to user space */
		if (copy_to_user(buf, data->read_buffer, length))
		{
			printk("copy_from_user failed!\n");
			return -EFAULT;
		}
		
		result += length;
		buf += length;
		count -= length;
	}

	return result;
}

static ssize_t goldfish_audio_write(struct file *fp, const char __user *buf,
							 size_t count, loff_t *pos)
{
	struct goldfish_audio* data = fp->private_data;
	unsigned long irq_flags;
	ssize_t result = 0;
	char __iomem *kbuf;

	while (count > 0)
	{
		ssize_t copy = count;
		if (copy > WRITE_BUFFER_SIZE)
			copy = WRITE_BUFFER_SIZE;
		wait_event_interruptible(data->wait, 
				(data->buffer_status & (AUDIO_INT_WRITE_BUFFER_1_EMPTY | AUDIO_INT_WRITE_BUFFER_2_EMPTY)));
		
		if ((data->buffer_status & AUDIO_INT_WRITE_BUFFER_1_EMPTY) != 0) {
			kbuf = data->write_buffer1;
		} else {
			kbuf = data->write_buffer2;
		}

		/* copy from user space to the appropriate buffer */
		if (copy_from_user(kbuf, buf, copy))
		{
			printk("copy_from_user failed!\n");
			result = -EFAULT;
			break;
		}
		else
		{
			spin_lock_irqsave(&data->lock, irq_flags);

			/* clear the buffer empty flag, and signal the emulator to start writing the buffer */
			if (kbuf == data->write_buffer1) {
				data->buffer_status &= ~AUDIO_INT_WRITE_BUFFER_1_EMPTY;
				GOLDFISH_AUDIO_WRITE(data, AUDIO_WRITE_BUFFER_1, copy);
			} else {
				data->buffer_status &= ~AUDIO_INT_WRITE_BUFFER_2_EMPTY;
				GOLDFISH_AUDIO_WRITE(data, AUDIO_WRITE_BUFFER_2, copy);
			}

			spin_unlock_irqrestore(&data->lock, irq_flags);
		}
 
		buf += copy;
		result += copy;
		count -= copy;
	}

	return result;
}

static int goldfish_audio_open(struct inode *ip, struct file *fp)
{
	if (!audio_data)
		return -ENODEV;

	if (atomic_inc_return(&open_count) == 1) 
	{
		fp->private_data = audio_data;
		audio_data->buffer_status = (AUDIO_INT_WRITE_BUFFER_1_EMPTY | AUDIO_INT_WRITE_BUFFER_2_EMPTY);
		GOLDFISH_AUDIO_WRITE(audio_data, AUDIO_INT_ENABLE, AUDIO_INT_MASK);
		return 0;
	} 
	else 
	{
		atomic_dec(&open_count);
		return -EBUSY;
	}
}

static int goldfish_audio_release(struct inode *ip, struct file* fp)
{
	atomic_dec(&open_count);
	GOLDFISH_AUDIO_WRITE(audio_data, AUDIO_INT_ENABLE, 0);
	return 0;
}
	   
static int goldfish_audio_ioctl(struct inode* ip, struct file* fp, unsigned int cmd, unsigned long arg)
{
	/* temporary workaround, until we switch to the ALSA API */
	if (cmd == 315)
		return -1;
	else
		return 0;
}

static irqreturn_t
goldfish_audio_interrupt(int irq, void *dev_id)
{
	unsigned long irq_flags;
	struct goldfish_audio	*data = dev_id;
	uint32_t status;

	spin_lock_irqsave(&data->lock, irq_flags);
	
	/* read buffer status flags */
	status = GOLDFISH_AUDIO_READ(data, AUDIO_INT_STATUS);
	status &= AUDIO_INT_MASK;
	/* if buffers are newly empty, wake up blocked goldfish_audio_write() call */
	if(status) {
		data->buffer_status = status;
		wake_up(&data->wait);
	}
	
	spin_unlock_irqrestore(&data->lock, irq_flags);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

/* file operations for /dev/eac */
static struct file_operations goldfish_audio_fops = {
	.owner = THIS_MODULE,
	.read = goldfish_audio_read,
	.write = goldfish_audio_write,
	.open = goldfish_audio_open,
	.release = goldfish_audio_release,
   .compat_ioctl = goldfish_audio_ioctl,

};
	
static struct miscdevice goldfish_audio_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "eac",
	.fops = &goldfish_audio_fops,
};

static int goldfish_audio_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *r;
	struct goldfish_audio *data;
	dma_addr_t buf_addr;

printk("goldfish_audio_probe\n");
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if(data == NULL) {
		ret = -ENOMEM;
		goto err_data_alloc_failed;
	}
	spin_lock_init(&data->lock);
	init_waitqueue_head(&data->wait);
	platform_set_drvdata(pdev, data);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(r == NULL) {
		printk("platform_get_resource failed\n");
		ret = -ENODEV;
		goto err_no_io_base;
	}
	data->reg_base = IO_ADDRESS(r->start - IO_START);

	data->irq = platform_get_irq(pdev, 0);
	if(data->irq < 0) {
		printk("platform_get_irq failed\n");
		ret = -ENODEV;
		goto err_no_irq;
	}

	data->buffer_virt = dma_alloc_writecombine(&pdev->dev, COMBINED_BUFFER_SIZE,
												&buf_addr, GFP_KERNEL);
	if(data->buffer_virt == 0) {
		ret = -ENOMEM;
		goto err_alloc_write_buffer_failed;
	}
	data->buffer_phys = buf_addr;
	data->write_buffer1 = data->buffer_virt;
	data->write_buffer2 = data->buffer_virt + WRITE_BUFFER_SIZE;
	data->read_buffer = data->buffer_virt + 2 * WRITE_BUFFER_SIZE;
	
	ret = request_irq(data->irq, goldfish_audio_interrupt, IRQF_SHARED, pdev->name, data);
	if(ret)
		goto err_request_irq_failed;

	if((ret = misc_register(&goldfish_audio_device))) 
	{
		printk("misc_register returned %d in goldfish_audio_init\n", ret);
		goto err_misc_register_failed;
	}

	
	GOLDFISH_AUDIO_WRITE(data, AUDIO_SET_WRITE_BUFFER_1, buf_addr);
	GOLDFISH_AUDIO_WRITE(data, AUDIO_SET_WRITE_BUFFER_2, buf_addr + WRITE_BUFFER_SIZE);

	data->read_supported = GOLDFISH_AUDIO_READ(data, AUDIO_READ_SUPPORTED);
	if (data->read_supported)
		GOLDFISH_AUDIO_WRITE(data, AUDIO_SET_READ_BUFFER, buf_addr + 2 * WRITE_BUFFER_SIZE);

	audio_data = data;
	return 0;

err_misc_register_failed:
err_request_irq_failed:
	dma_free_writecombine(&pdev->dev, COMBINED_BUFFER_SIZE, data->buffer_virt, data->buffer_phys);
err_alloc_write_buffer_failed:
err_no_irq:
err_no_io_base:
	kfree(data);
err_data_alloc_failed:
	return ret;
}

static int goldfish_audio_remove(struct platform_device *pdev)
{
	struct goldfish_audio *data = platform_get_drvdata(pdev);

	misc_deregister(&goldfish_audio_device);
	free_irq(data->irq, data);
	dma_free_writecombine(&pdev->dev, COMBINED_BUFFER_SIZE, data->buffer_virt, data->buffer_phys);
	kfree(data);
	audio_data = NULL;
	return 0;
}

static struct platform_driver goldfish_audio_driver = {
	.probe		= goldfish_audio_probe,
	.remove		= goldfish_audio_remove,
	.driver = {
		.name = "goldfish_audio"
	}
};

static int __init goldfish_audio_init(void)
{
	int ret;

	ret = platform_driver_register(&goldfish_audio_driver);
	if (ret < 0)
	{
		printk("platform_driver_register returned %d\n", ret);
		return ret;
	}

	return ret;
}

static void __exit goldfish_audio_exit(void)
{
	platform_driver_unregister(&goldfish_audio_driver);
}

module_init(goldfish_audio_init);
module_exit(goldfish_audio_exit);
