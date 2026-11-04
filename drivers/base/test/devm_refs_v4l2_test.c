// SPDX-License-Identifier: GPL-2.0
/*
 * Test vehicle for CONFIG_DEBUG_DEVRES_REFS, V4L2 shape.
 *
 * Mirrors the layout of drivers such as cx231xx: the driver state, with the
 * struct v4l2_device and struct video_device embedded in it, is allocated
 * with devm_kzalloc(), so it is freed on unbind even while the video node is
 * open. An open V4L2 file does not point at that state directly; it points
 * at a separately allocated v4l2_fh, and reaches the video_device through
 * video_devdata(). The check must find it through the V4L2 holder hook.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

#define DEVM_REFS_V4L2_MAGIC	0x3256344c53464552ULL

struct devm_refs_v4l2 {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct mutex lock;
	u64 magic;
};

static int devm_refs_v4l2_querycap(struct file *file, void *fh,
				   struct v4l2_capability *cap)
{
	strscpy(cap->driver, "devm_refs_v4l2", sizeof(cap->driver));
	strscpy(cap->card, "devm_refs_v4l2", sizeof(cap->card));
	return 0;
}

static const struct v4l2_ioctl_ops devm_refs_v4l2_ioctl_ops = {
	.vidioc_querycap = devm_refs_v4l2_querycap,
};

static int devm_refs_v4l2_release(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct devm_refs_v4l2 *st = container_of(vdev, struct devm_refs_v4l2, vdev);

	/* After unbind this is a read of freed memory, as in cx231xx's close. */
	pr_info("devm_refs_v4l2: release, magic %s\n",
		st->magic == DEVM_REFS_V4L2_MAGIC ? "ok" : "CORRUPT");
	return v4l2_fh_release(file);
}

static const struct v4l2_file_operations devm_refs_v4l2_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = devm_refs_v4l2_release,
	.unlocked_ioctl = video_ioctl2,
};

static int devm_refs_v4l2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct devm_refs_v4l2 *st;
	int ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->magic = DEVM_REFS_V4L2_MAGIC;
	mutex_init(&st->lock);

	ret = v4l2_device_register(dev, &st->v4l2_dev);
	if (ret)
		return ret;

	strscpy(st->vdev.name, "devm_refs_v4l2", sizeof(st->vdev.name));
	st->vdev.v4l2_dev = &st->v4l2_dev;
	st->vdev.fops = &devm_refs_v4l2_fops;
	st->vdev.ioctl_ops = &devm_refs_v4l2_ioctl_ops;
	st->vdev.release = video_device_release_empty;
	st->vdev.lock = &st->lock;
	st->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE;
	video_set_drvdata(&st->vdev, st);

	ret = video_register_device(&st->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_device_unregister(&st->v4l2_dev);
		return ret;
	}
	platform_set_drvdata(pdev, st);
	dev_info(dev, "probed: %s\n", video_device_node_name(&st->vdev));
	return 0;
}

static void devm_refs_v4l2_remove(struct platform_device *pdev)
{
	struct devm_refs_v4l2 *st = platform_get_drvdata(pdev);

	video_unregister_device(&st->vdev);
	v4l2_device_unregister(&st->v4l2_dev);
	dev_info(&pdev->dev, "removed; devres release follows\n");
}

static struct platform_driver devm_refs_v4l2_driver = {
	.probe = devm_refs_v4l2_probe,
	.remove = devm_refs_v4l2_remove,
	.driver = { .name = "devm_refs_v4l2" },
};

static struct platform_device *devm_refs_v4l2_pdev;

static int __init devm_refs_v4l2_init(void)
{
	int ret;

	ret = platform_driver_register(&devm_refs_v4l2_driver);
	if (ret)
		return ret;
	devm_refs_v4l2_pdev = platform_device_register_simple("devm_refs_v4l2",
							       -1, NULL, 0);
	if (IS_ERR(devm_refs_v4l2_pdev)) {
		platform_driver_unregister(&devm_refs_v4l2_driver);
		return PTR_ERR(devm_refs_v4l2_pdev);
	}
	return 0;
}

static void __exit devm_refs_v4l2_exit(void)
{
	platform_device_unregister(devm_refs_v4l2_pdev);
	platform_driver_unregister(&devm_refs_v4l2_driver);
}

module_init(devm_refs_v4l2_init);
module_exit(devm_refs_v4l2_exit);
MODULE_DESCRIPTION("V4L2-shaped test vehicle for DEBUG_DEVRES_REFS");
MODULE_LICENSE("GPL");
