// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Backlight driver for the Richtek RT9363
 * Based on Samsung GT-E2210 firmware source code
 *
 * Copyright (C) 2026 BHmsWare <bhmsgamexbox2010@gmail.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define RT9363_MIN_RATIO		1
#define RT9363_MAX_RATIO		16

#define RT9363_HIGH_NS			1000
#define RT9363_OFF_CRIT_NS		100000
#define RT9363_OFF_MS			2

struct rt9363_backlight {
	struct device *dev;
	struct backlight_device *bl;
	struct gpio_desc *gpiod;
	u16 ratio;
};

static void rt9363_backlight_set_max_ratio(struct rt9363_backlight *rt9363)
{
	gpiod_set_value_cansleep(rt9363->gpiod, 1);
	ndelay(RT9363_HIGH_NS);
}

static int rt9363_backlight_stepdown(struct rt9363_backlight *rt9363)
{
	u64 ns;

	ns = ktime_get_ns();
	gpiod_set_value(rt9363->gpiod, 0);
	gpiod_set_value(rt9363->gpiod, 1);
	ns = ktime_get_ns() - ns;
	if (ns >= RT9363_OFF_CRIT_NS) {
		dev_err(rt9363->dev, "PCM on backlight took too long (%llu ns)\n", ns);
		return -EAGAIN;
	}
	ndelay(RT9363_HIGH_NS);
	return 0;
}

static int rt9363_backlight_update_status(struct backlight_device *bl)
{
	struct rt9363_backlight *rt9363 = bl_get_data(bl);
	int brightness = backlight_get_brightness(bl);
	int start_ratio = bl->props.max_brightness + 1;
	int target_ratio;
	int current_ratio;
	int ret;

	dev_dbg(rt9363->dev, "new brightness/ratio: %d\n", brightness);

	target_ratio = brightness;

	/* the same brightness value */
	if (target_ratio == rt9363->ratio)
		return 0;

	/* now, backlight is off */
	if (target_ratio == 0) {
		gpiod_set_value_cansleep(rt9363->gpiod, 0);
		msleep(RT9363_OFF_MS);
		rt9363->ratio = 0;
		return 0;
	}

	/* adjusting the brightness */
	current_ratio = start_ratio;
	while (current_ratio != target_ratio) {

		ret = rt9363_backlight_stepdown(rt9363);
		if (ret == -EAGAIN) {
			gpiod_set_value_cansleep(rt9363->gpiod, 0);
			msleep(RT9363_OFF_MS);
			rt9363_backlight_set_max_ratio(rt9363);
			current_ratio = start_ratio;
		} else if (current_ratio == RT9363_MIN_RATIO) {
			current_ratio = start_ratio;
		} else {
			current_ratio--;
		}
	}
	rt9363->ratio = current_ratio;

	dev_dbg(rt9363->dev, "new ratio set to %d\n", target_ratio);

	return 0;
}

static const struct backlight_ops rt9363_backlight_ops = {
	.options		= BL_CORE_SUSPENDRESUME,
	.update_status	= rt9363_backlight_update_status,
};

static int rt9363_backlight_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_device *bl;
	struct rt9363_backlight *rt9363;
	u32 max_brightness;
	u32 brightness;
	int ret;

	rt9363 = devm_kzalloc(dev, sizeof(*rt9363), GFP_KERNEL);
	if (!rt9363)
		return -ENOMEM;
	rt9363->dev = dev;

	ret = device_property_read_u32(dev, "max-brightness", &max_brightness);
	if (ret)
		max_brightness = RT9363_MAX_RATIO;

	ret = device_property_read_u32(dev, "default-brightness", &brightness);
	if (ret)
		brightness = max_brightness / 2;
	if (brightness > max_brightness) {
		dev_err(dev, "default brightness exceeds max brightness\n");
		brightness = max_brightness;
	}

	rt9363->gpiod = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(rt9363->gpiod))
		return dev_err_probe(dev, PTR_ERR(rt9363->gpiod),
				     "gpio line missing or invalid.\n");
	gpiod_set_consumer_name(rt9363->gpiod, dev_name(dev));
	msleep(RT9363_OFF_MS);

	bl = devm_backlight_device_register(dev, dev_name(dev), dev, rt9363,
					    &rt9363_backlight_ops, NULL);
	if (IS_ERR(bl)) {
		dev_err(dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}

	bl->props.max_brightness = max_brightness;
	if (brightness) {
		bl->props.brightness = brightness;
		bl->props.power = BACKLIGHT_POWER_ON;
	} else {
		bl->props.brightness = 0;
		bl->props.power = BACKLIGHT_POWER_OFF;
	}

	rt9363->bl = bl;
	platform_set_drvdata(pdev, bl);
	backlight_update_status(bl);

	return 0;
}

static const struct of_device_id rt9363_backlight_of_match[] = {
	{ .compatible = "richtek,rt9363" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rt9363_backlight_of_match);

static struct platform_driver rt9363_backlight_driver = {
	.driver = {
		.name = "rt9363-backlight",
		.of_match_table = rt9363_backlight_of_match,
	},
	.probe		= rt9363_backlight_probe,
};
module_platform_driver(rt9363_backlight_driver);

MODULE_AUTHOR("BHmsWare <bhmsgamexbox2010@gmail.com>");
MODULE_DESCRIPTION("Richtek RT9363 Backlight Driver");
MODULE_LICENSE("GPL");
