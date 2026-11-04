// SPDX-License-Identifier: GPL-2.0
/*
 * Test glue: instantiate gpio-sloppy-logic-analyzer on two lines of a tiny
 * non-sleeping fake GPIO chip, without device tree, so DEBUG_DEVRES_REFS
 * can be exercised against a real in-tree driver in a VM. (gpio-sim cannot
 * be used: its lines can sleep and the analyzer rejects them with -EREMOTE.)
 */
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define FAKE_CHIP_LABEL	"devm-refs-sim"

static int fake_get(struct gpio_chip *gc, unsigned int offset)
{
	return 0;
}

static int fake_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	return 0;
}

static int fake_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	return GPIO_LINE_DIRECTION_IN;
}

static int fake_chip_probe(struct platform_device *pdev)
{
	struct gpio_chip *gc;

	gc = devm_kzalloc(&pdev->dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;
	gc->label = FAKE_CHIP_LABEL;
	gc->parent = &pdev->dev;
	gc->owner = THIS_MODULE;
	gc->base = -1;
	gc->ngpio = 4;
	gc->can_sleep = false;
	gc->get = fake_get;
	gc->direction_input = fake_direction_input;
	gc->get_direction = fake_get_direction;
	return devm_gpiochip_add_data(&pdev->dev, gc, NULL);
}

static struct platform_driver fake_chip_driver = {
	.probe = fake_chip_probe,
	.driver = { .name = "devm-refs-fake-gpio" },
};

static struct gpiod_lookup_table devm_refs_la_lookup = {
	.dev_id = "gpio-sloppy-logic-analyzer",
	.table = {
		GPIO_LOOKUP_IDX(FAKE_CHIP_LABEL, 0, "probe", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(FAKE_CHIP_LABEL, 1, "probe", 1, GPIO_ACTIVE_HIGH),
		{ }
	},
};

static const char * const devm_refs_la_names[] = { "sim0", "sim1" };

static const struct property_entry devm_refs_la_props[] = {
	PROPERTY_ENTRY_STRING_ARRAY("probe-names", devm_refs_la_names),
	{ }
};

static int __init devm_refs_la_glue_init(void)
{
	struct platform_device_info info = {
		.name = "gpio-sloppy-logic-analyzer",
		.id = PLATFORM_DEVID_NONE,
		.properties = devm_refs_la_props,
	};
	struct platform_device *pdev;
	int ret;

	ret = platform_driver_register(&fake_chip_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("devm-refs-fake-gpio", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	gpiod_add_lookup_table(&devm_refs_la_lookup);
	pdev = platform_device_register_full(&info);
	if (IS_ERR(pdev)) {
		gpiod_remove_lookup_table(&devm_refs_la_lookup);
		return PTR_ERR(pdev);
	}
	pr_info("devm_refs_la_glue: fake chip and analyzer device registered\n");
	return 0;
}
late_initcall(devm_refs_la_glue_init);
