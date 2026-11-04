// SPDX-License-Identifier: GPL-2.0
/*
 * Test vehicle for CONFIG_DEBUG_DEVRES_REFS.
 *
 * Registers one platform device bound to a driver that exposes two misc
 * devices:
 *
 *   /dev/devm_refs_bad  - driver state allocated with devm_kzalloc(), so
 *                         it is freed on unbind even while the node is open.
 *                         file->private_data (set by the misc core to the
 *                         embedded struct miscdevice) points into it.
 *   /dev/devm_refs_good - driver state refcounted and freed on last put,
 *                         so unbind with the node open is safe.
 *
 * Unbinding the device with both nodes held open must produce exactly one
 * DEBUG_DEVRES_REFS warning, for the bad node. Closing the bad node after
 * unbind is a use-after-free that KASAN reports independently.
 */
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEVM_REFS_MAGIC	0x5245465344455644ULL	/* "DVEDSFER" */

struct devm_refs_bad {
	struct miscdevice misc;
	u64 magic;
};

struct devm_refs_good {
	struct miscdevice misc;
	struct kref kref;
	u64 magic;
};

struct devm_refs_state {
	struct devm_refs_bad *bad;
	struct devm_refs_good *good;
};

static ssize_t devm_refs_bad_read(struct file *file, char __user *buf,
				  size_t len, loff_t *ppos)
{
	struct devm_refs_bad *bad =
		container_of(file->private_data, struct devm_refs_bad, misc);
	char tmp[32];
	int n;

	n = scnprintf(tmp, sizeof(tmp), "%s\n",
		      bad->magic == DEVM_REFS_MAGIC ? "ok" : "CORRUPT");
	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static int devm_refs_bad_release(struct inode *inode, struct file *file)
{
	struct devm_refs_bad *bad =
		container_of(file->private_data, struct devm_refs_bad, misc);

	/* After unbind this is a read of freed memory. */
	pr_info("devm_refs_test: bad release, magic %s\n",
		bad->magic == DEVM_REFS_MAGIC ? "ok" : "CORRUPT");
	return 0;
}

static const struct file_operations devm_refs_bad_fops = {
	.owner = THIS_MODULE,
	.read = devm_refs_bad_read,
	.release = devm_refs_bad_release,
};

static void devm_refs_good_free(struct kref *kref)
{
	struct devm_refs_good *good =
		container_of(kref, struct devm_refs_good, kref);

	pr_info("devm_refs_test: good freed on last put\n");
	kfree(good);
}

static int devm_refs_good_open(struct inode *inode, struct file *file)
{
	struct devm_refs_good *good =
		container_of(file->private_data, struct devm_refs_good, misc);

	kref_get(&good->kref);
	return 0;
}

static ssize_t devm_refs_good_read(struct file *file, char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct devm_refs_good *good =
		container_of(file->private_data, struct devm_refs_good, misc);
	char tmp[32];
	int n;

	n = scnprintf(tmp, sizeof(tmp), "%s\n",
		      good->magic == DEVM_REFS_MAGIC ? "ok" : "CORRUPT");
	return simple_read_from_buffer(buf, len, ppos, tmp, n);
}

static int devm_refs_good_release(struct inode *inode, struct file *file)
{
	struct devm_refs_good *good =
		container_of(file->private_data, struct devm_refs_good, misc);

	pr_info("devm_refs_test: good release, magic %s\n",
		good->magic == DEVM_REFS_MAGIC ? "ok" : "CORRUPT");
	kref_put(&good->kref, devm_refs_good_free);
	return 0;
}

static const struct file_operations devm_refs_good_fops = {
	.owner = THIS_MODULE,
	.open = devm_refs_good_open,
	.read = devm_refs_good_read,
	.release = devm_refs_good_release,
};

static int devm_refs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct devm_refs_state *st;
	int ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->bad = devm_kzalloc(dev, sizeof(*st->bad), GFP_KERNEL);
	if (!st->bad)
		return -ENOMEM;
	st->bad->magic = DEVM_REFS_MAGIC;
	st->bad->misc.minor = MISC_DYNAMIC_MINOR;
	st->bad->misc.name = "devm_refs_bad";
	st->bad->misc.fops = &devm_refs_bad_fops;
	st->bad->misc.parent = dev;

	st->good = kzalloc(sizeof(*st->good), GFP_KERNEL);
	if (!st->good)
		return -ENOMEM;
	kref_init(&st->good->kref);
	st->good->magic = DEVM_REFS_MAGIC;
	st->good->misc.minor = MISC_DYNAMIC_MINOR;
	st->good->misc.name = "devm_refs_good";
	st->good->misc.fops = &devm_refs_good_fops;
	st->good->misc.parent = dev;

	ret = misc_register(&st->bad->misc);
	if (ret)
		goto err_free_good;
	ret = misc_register(&st->good->misc);
	if (ret)
		goto err_dereg_bad;

	platform_set_drvdata(pdev, st);
	dev_info(dev, "probed: /dev/devm_refs_bad and /dev/devm_refs_good\n");
	return 0;

err_dereg_bad:
	misc_deregister(&st->bad->misc);
err_free_good:
	kfree(st->good);
	return ret;
}

static void devm_refs_remove(struct platform_device *pdev)
{
	struct devm_refs_state *st = platform_get_drvdata(pdev);

	misc_deregister(&st->good->misc);
	misc_deregister(&st->bad->misc);
	kref_put(&st->good->kref, devm_refs_good_free);
	dev_info(&pdev->dev, "removed; devres release follows\n");
}

static struct platform_driver devm_refs_driver = {
	.probe = devm_refs_probe,
	.remove = devm_refs_remove,
	.driver = {
		.name = "devm_refs_test",
	},
};

static struct platform_device *devm_refs_pdev;

static int __init devm_refs_test_init(void)
{
	int ret;

	ret = platform_driver_register(&devm_refs_driver);
	if (ret)
		return ret;

	devm_refs_pdev = platform_device_register_simple("devm_refs_test", -1,
							  NULL, 0);
	if (IS_ERR(devm_refs_pdev)) {
		platform_driver_unregister(&devm_refs_driver);
		return PTR_ERR(devm_refs_pdev);
	}
	return 0;
}

static void __exit devm_refs_test_exit(void)
{
	platform_device_unregister(devm_refs_pdev);
	platform_driver_unregister(&devm_refs_driver);
}

module_init(devm_refs_test_init);
module_exit(devm_refs_test_exit);
MODULE_DESCRIPTION("Test vehicle for DEBUG_DEVRES_REFS");
MODULE_LICENSE("GPL");
