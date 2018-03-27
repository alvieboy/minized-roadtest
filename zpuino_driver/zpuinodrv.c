/*  zpuinodrv.c - ZPUino driver for Zynq devices

* Copyright (C) 2018 Alvaro Lopes
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.

*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License along
*   with this program. If not, see <http://www.gnu.org/licenses/>.

*/
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fcntl.h>
#include <linux/cdev.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <asm/uaccess.h>

#define DRIVER_NAME "zpuinodrv"
#define ZPUCFG_DEVICES 1

#define ZPUCTL_MINOR 129

#define ZPUREG_SIGNATURE    0
#define ZPUREG_ZPUCONFIG    1
#define ZPUREG_RSTCTL       3
#define ZPUREG_MADDR        4
#define ZPUREG_MACCESS      7


#define ZPU_IOCTL_SETRESET _IOW('Z', 0, unsigned)

struct zpuinodrv_drvdata {
	struct cdev cdev;
	dev_t devt;
	struct class *class;
	int irq;
	unsigned long mem_start;
	unsigned long mem_end;
	void __iomem *base_addr;
	uint32_t memsize;
	loff_t mem_offset;
        unsigned int is_open:1;
};

static DEFINE_MUTEX(zpuctl_mutex);

static inline void zpuinodrv_writereg(struct zpuinodrv_drvdata *lp, uint32_t regno, uint32_t val)
{
	void __iomem *regaddr = (&((uint32_t*)lp->base_addr)[regno]);
	iowrite32(val, regaddr);
}

static inline uint32_t zpuinodrv_readreg(struct zpuinodrv_drvdata *lp, uint32_t regno)
{
	void __iomem *regaddr = (&((uint32_t*)lp->base_addr)[regno]);
	return ioread32(regaddr);
}


static int zpuinodrv_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zpuinodrv_drvdata *drvdata = dev_get_drvdata(dev);
	//free_irq(drvdata->irq, drvdata);

	drvdata = platform_get_drvdata(pdev);

	if (!drvdata)
		return -ENODEV;

	unregister_chrdev_region(drvdata->devt, ZPUCFG_DEVICES);

	//sysfs_remove_group(&pdev->dev.kobj, &xdevcfg_attr_group);

	device_destroy(drvdata->class, drvdata->devt);
	class_destroy(drvdata->class);
	cdev_del(&drvdata->cdev);



	release_mem_region(drvdata->mem_start, drvdata->mem_end - drvdata->mem_start + 1);
	kfree(drvdata);
	dev_set_drvdata(dev, NULL);
	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id zpuinodrv_of_match[] = {
	{ .compatible = "xlnx,zynq-zpuino-top-1.0", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, zpuinodrv_of_match);
#else
# define zpuinodrv_of_match
#endif



static loff_t zpuctl_llseek(struct file *file, loff_t offset, int origin)
{
	struct zpuinodrv_drvdata *drvdata = file->private_data;
	loff_t new_offset;

	switch (origin) {
	case SEEK_SET:
		new_offset = offset;
		break;
	case SEEK_CUR:
		new_offset = drvdata->mem_offset + offset;
		break;
	case SEEK_END:
		new_offset = drvdata->memsize + offset;
		break;
	default:
		return -EINVAL;

	}

	if (new_offset<0 || new_offset>=drvdata->memsize) {
		return -EINVAL;
	}

	zpuinodrv_writereg( drvdata, ZPUREG_MADDR, new_offset);

	return new_offset;
}

static ssize_t zpuctl_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	int status = 0;
	struct zpuinodrv_drvdata *drvdata = file->private_data;
	u32 *kbuf, *kptr;
	loff_t cpos;
	size_t to_read;

	if ((count&3)!=0) { /* Allow only word-multiples (i.e., multiples of 4 bytes */
		status = -EINVAL;
		goto error;
	}

	cpos = drvdata->mem_offset + count;

	if (cpos > drvdata->memsize) {
		count -= (cpos-drvdata->memsize);
	}

	kbuf = kmalloc(count, GFP_KERNEL);

	if (kbuf==NULL) {
		status = -ENOMEM;
		goto error;
	}

	kptr = kbuf;
	to_read = count;
	while (to_read) {
		*kptr++ = zpuinodrv_readreg( drvdata, ZPUREG_MACCESS);
		to_read-=4;
	}

	if (copy_to_user(buf, kbuf, count)) {
		status = -EFAULT;
		goto error2;
	}
	drvdata->mem_offset += count;

	if (*ppos)
		*ppos =drvdata->mem_offset;

	status = count;
error2:
	kfree( kbuf );
error:
	return status;
}

static ssize_t zpuctl_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct zpuinodrv_drvdata *drvdata = file->private_data;
	loff_t cpos = drvdata->mem_offset + count;
	u32 *kbuf, *kptr;
	int status;
	size_t to_copy;

	if ((count&3)!=0) { /* Allow only word-multiples (i.e., multiples of 4 bytes */
		status = -EIO;
		goto error;
	}

	if (cpos > drvdata->memsize) {
		count -= (cpos-drvdata->memsize);
	}

	kbuf = kmalloc(count, GFP_KERNEL);

	if (kbuf==NULL) {
		status = -ENOMEM;
		goto error;
	}

	if (copy_from_user(kbuf, buf,count)) {
		status = -EFAULT;
		goto error;
	}
	kptr = kbuf;
	to_copy = count;
	while (to_copy) {
		zpuinodrv_writereg( drvdata, ZPUREG_MACCESS, *kptr++);
		to_copy-=4;
	}
	status = count;
error:
	kfree(kbuf);
	return status;
}

static int zpuctl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int status;
	uint32_t prev, now;

	struct zpuinodrv_drvdata *drvdata = file->private_data;

	switch (cmd) {
	case ZPU_IOCTL_SETRESET:
		prev = zpuinodrv_readreg( drvdata, ZPUREG_RSTCTL);

		zpuinodrv_writereg( drvdata, ZPUREG_RSTCTL, (uint32_t)arg);

		now = zpuinodrv_readreg( drvdata, ZPUREG_RSTCTL);

		status = 0;
		break;
	default:
		status = -EINVAL;
	}

	return status;
}


static int zpuctl_release(struct inode *inode, struct file *file)
{
	struct zpuinodrv_drvdata *drvdata = file->private_data;

	drvdata->is_open = 0;

        return 0;
}

static int zpuctl_open(struct inode *inode, struct file *file)
{
	struct zpuinodrv_drvdata *drvdata;
	int status = -EIO;

        drvdata = container_of(inode->i_cdev, struct zpuinodrv_drvdata, cdev);

	if (NULL==drvdata) {
		status = -ENODEV;
		goto error;
	}
	if (drvdata->is_open) {
		printk(KERN_INFO "Device busy");
		status = -EBUSY;
		goto error;
	}

	drvdata->is_open = 1;
	drvdata->mem_offset = 0;

	zpuinodrv_writereg( drvdata, ZPUREG_MADDR, 0);

	file->private_data = drvdata;

	status = 0;
error:
        return status;
}

static long zpuctl_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret;

	mutex_lock(&zpuctl_mutex);
	ret = zpuctl_ioctl(file, cmd, arg);
	mutex_unlock(&zpuctl_mutex);

	return ret;
}


const struct file_operations zpuctl_fops = {
	.owner		= THIS_MODULE,
	.llseek		= zpuctl_llseek,
        .read		= zpuctl_read,
        .open           = zpuctl_open,
	.write		= zpuctl_write,
	.release      	= zpuctl_release,
	.unlocked_ioctl	= zpuctl_unlocked_ioctl,
};

/*static struct miscdevice zpuctl_dev = {
	ZPUCTL_MINOR,
	"zpuctl",
	&zpuctl_fops
};*/


static int zpuinodrv_probe(struct platform_device *pdev)
{
	struct resource *r_irq; /* Interrupt resources */
	struct resource *r_mem; /* IO mem resources */
	struct device *dev = &pdev->dev;
	struct zpuinodrv_drvdata *drvdata = NULL;
	uint32_t signature;
	uint32_t revision;
        uint32_t memval;
        uint32_t addr = 0x100;
        int rc = 0;

	/* Get iospace for the device */
	r_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r_mem) {
		dev_err(dev, "invalid address\n");
		return -ENODEV;
	}
	drvdata = (struct zpuinodrv_drvdata *) kmalloc(sizeof(struct zpuinodrv_drvdata), GFP_KERNEL);
	if (!drvdata) {
		dev_err(dev, "Cound not allocate zpuinodrv device\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, drvdata);
	drvdata->mem_start = r_mem->start;
	drvdata->mem_end = r_mem->end;

	if (!request_mem_region(drvdata->mem_start,
				drvdata->mem_end - drvdata->mem_start + 1,
				DRIVER_NAME)) {
		dev_err(dev, "Couldn't lock memory region at %p\n",
			(void *)drvdata->mem_start);
		rc = -EBUSY;
		goto error1;
	}

	drvdata->base_addr = ioremap(drvdata->mem_start, drvdata->mem_end - drvdata->mem_start + 1);
	if (!drvdata->base_addr) {
		dev_err(dev, "zpuinodrv: Could not allocate iomem\n");
		rc = -EIO;
		goto error2;
        }
        drvdata->irq = -1;
#if 0
	/* Get IRQ for the device */
	r_irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!r_irq) {
		dev_info(dev, "no IRQ found\n");
		dev_info(dev, "zpuinodrv at 0x%08x mapped to 0x%08x\n",
			(unsigned int __force)drvdata->mem_start,
			(unsigned int __force)drvdata->base_addr);
		return 0;
	}
	drvdata->irq = r_irq->start;
	rc = request_irq(drvdata->irq, &zpuinodrv_irq, 0, DRIVER_NAME, drvdata);
	if (rc) {
		dev_err(dev, "zpuinodrv: Could not allocate interrupt %d.\n",
			drvdata->irq);
		goto error3;
	}
#endif
        // Probe
	signature = zpuinodrv_readreg( drvdata, ZPUREG_SIGNATURE );

	if ((signature&0xFFFFFF00)!=0x5A505500) {
            dev_err(dev,"Invalid signature\n");
            goto error2;
        }

	revision = zpuinodrv_readreg( drvdata, ZPUREG_ZPUCONFIG );

	/* Detect memory size */

	/* Place ZPU under reset */
	zpuinodrv_writereg( drvdata, ZPUREG_RSTCTL, 1);
        /* Detect memory size */

        zpuinodrv_writereg( drvdata, ZPUREG_MADDR,   0x00000000);
        zpuinodrv_writereg( drvdata, ZPUREG_MACCESS, 0x00000000);
        zpuinodrv_writereg( drvdata, ZPUREG_MADDR,   0x00000000);

        memval = zpuinodrv_readreg( drvdata, ZPUREG_MACCESS );
	do {
		zpuinodrv_writereg( drvdata, ZPUREG_MADDR, addr);
		zpuinodrv_writereg( drvdata, ZPUREG_MACCESS, 0x5A5AA5A5);
		zpuinodrv_writereg( drvdata, ZPUREG_MADDR, 0x00000000);
		memval = zpuinodrv_readreg( drvdata, ZPUREG_MACCESS );
		if (memval==0x5A5AA5A5) {
			break;
		}
		addr<<=1;
	} while (addr!=0x40000000);

	if (addr==0x40000000) {
		dev_err(dev,"Cannot determine ZPUino memory size");
		rc = -EIO;
		goto error2;
                }

	drvdata->memsize = addr;

	dev_info(dev,"Found ZPUino at 0x%08x, rev %d. %d cores, 0x%08x bytes memory.\n",
		 drvdata->mem_start,
		 revision & 0xFFFF,
		 1+((revision>>16)&0xFF),
		 drvdata->memsize);

	drvdata->is_open = 0;

	dev_t devt;

	rc = alloc_chrdev_region(&devt, 0, ZPUCFG_DEVICES, DRIVER_NAME);
	if (rc < 0)
		goto error2;

	drvdata->devt = devt;

	cdev_init(&drvdata->cdev, &zpuctl_fops);

	drvdata->cdev.owner = THIS_MODULE;

	rc = cdev_add(&drvdata->cdev, devt, 1);
	if (rc) {
		dev_err(dev, "cdev_add() failed\n");
		goto error3;
	}

	drvdata->class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(drvdata->class)) {
		dev_err(dev, "failed to create class\n");
		goto error3;
	}

	dev = device_create(drvdata->class, &pdev->dev, devt, drvdata,
			    DRIVER_NAME);
	if (IS_ERR(dev)) {
		dev_err(dev, "unable to create device\n");
		goto error4;
	}


	return 0;
error4:
	class_destroy(drvdata->class);

error3:
	unregister_chrdev_region(devt, ZPUCFG_DEVICES);

	//free_irq(lp->irq, drvdata);
error2:
	release_mem_region(drvdata->mem_start, drvdata->mem_end - drvdata->mem_start + 1);
error1:
	kfree(drvdata);
	dev_set_drvdata(dev, NULL);
	return rc;
}


static struct platform_driver zpuinodrv_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table	= zpuinodrv_of_match,
	},
	.probe		= zpuinodrv_probe,
	.remove		= zpuinodrv_remove,
};

static int __init zpuinodrv_init(void)
{
	int ret;
	printk(KERN_INFO "ZPUino ZYNQ driver (C) Alvaro Lopes 2018\n");

	ret = platform_driver_register(&zpuinodrv_driver);
	return ret;
}


static void __exit zpuinodrv_exit(void)
{
	platform_driver_unregister(&zpuinodrv_driver);
}

module_init(zpuinodrv_init);
module_exit(zpuinodrv_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alvaro Lopes");
MODULE_DESCRIPTION("zpuinodrv - ZPUino driver for Zynq devices");
