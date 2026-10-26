// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Watchdog driver for PNX49xx board
 *
 * Copyright (C) 2026 BHmsWare <bhmsgamexbox2010@gmail.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>
#include <linux/io.h>

#define PNX49XX_WDT_DEFAULT_HEARTBEAT	10
#define PNX49XX_WDT_MAX_HEARTBEAT		32

/* registers */
#define PNX49XX_WDT_CTRL		0x00
#define PNX49XX_WDT_RELOAD		0x04
#define PNX49XX_WDT_COUNTER		0x08
#define PNX49XX_WDT_RESTART		0x0c

/* feed it to the restart counter register to kick a watchdog */
#define PNX49XX_WDT_RESTART_MAGIC	0xecd9f7bd

#define PNX49XX_WDT_DEFAULT_RESET_COUNTER	255
#define PNX49XX_WDT_REBOOT_RESET_COUNTER	2

/* 32 kHz, the same osc freq as in gptimer btw */
#define PNX49XX_WDT_COUNTER_RATE	32768
#define PNX49XX_WDT_MAX_COUNTER		0xfffff

static void __iomem *wdt_base;

static int pnx49xx_wdt_start(struct watchdog_device *wdd)
{
	writel(PNX49XX_WDT_DEFAULT_RESET_COUNTER, wdt_base + PNX49XX_WDT_CTRL);
	return 0;
}

static int pnx49xx_wdt_stop(struct watchdog_device *wdd)
{
	u32 val;

	val = readl(wdt_base + PNX49XX_WDT_CTRL);
	val |= BIT(31);
	writel(val, wdt_base + PNX49XX_WDT_CTRL);
	return 0;
}

static int pnx49xx_wdt_ping(struct watchdog_device *wdd)
{
	writel(PNX49XX_WDT_RESTART_MAGIC, wdt_base + PNX49XX_WDT_RESTART);
	return 0;
}

static int pnx49xx_wdt_set_timeout(struct watchdog_device *wdd,
		unsigned int new_timeout)
{
	u32 ticks;

	wdd->timeout = new_timeout;

	ticks = new_timeout * PNX49XX_WDT_COUNTER_RATE;
	if (ticks > PNX49XX_WDT_MAX_COUNTER)
		ticks = PNX49XX_WDT_MAX_COUNTER;

	writel(ticks, wdt_base + PNX49XX_WDT_RELOAD);
	writel(PNX49XX_WDT_RESTART_MAGIC, wdt_base + PNX49XX_WDT_RESTART);
	return 0;
}

static int pnx49xx_wdt_restart(struct watchdog_device *wdd,
		unsigned long action, void *data)
{
	writel(2, wdt_base + PNX49XX_WDT_RELOAD);
	writel(PNX49XX_WDT_RESTART_MAGIC, wdt_base + PNX49XX_WDT_RESTART);

	writel(PNX49XX_WDT_REBOOT_RESET_COUNTER, wdt_base + PNX49XX_WDT_CTRL);
	return 0;
}

static const struct watchdog_ops pnx49xx_wdt_ops = {
	.owner			= THIS_MODULE,
	.start			= pnx49xx_wdt_start,
	.stop			= pnx49xx_wdt_stop,
	.ping			= pnx49xx_wdt_ping,
	.set_timeout	= pnx49xx_wdt_set_timeout,
	.restart		= pnx49xx_wdt_restart,
};

static const struct watchdog_info pnx49xx_wdt_ident = {
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity	= "PNX49xx Watchdog",
};

static struct watchdog_device pnx49xx_wdd = {
	.info = &pnx49xx_wdt_ident,
	.ops = &pnx49xx_wdt_ops,
	.timeout = PNX49XX_WDT_DEFAULT_HEARTBEAT,
	.min_timeout = 1,
	.max_timeout = PNX49XX_WDT_MAX_HEARTBEAT,
};

static int pnx49xx_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	wdt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt_base))
		return PTR_ERR(wdt_base);

	pnx49xx_wdd.parent = dev;
	watchdog_set_restart_priority(&pnx49xx_wdd, 128);

	watchdog_stop_on_reboot(&pnx49xx_wdd);
	watchdog_stop_on_unregister(&pnx49xx_wdd);

	return devm_watchdog_register_device(dev, &pnx49xx_wdd);
}

static const struct of_device_id pnx49xx_wdt_match[] = {
	{ .compatible = "nxp,pnx49xx-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, pnx49xx_wdt_match);

static struct platform_driver platform_wdt_driver = {
	.driver = {
		.name = "pnx49xx-watchdog",
		.of_match_table = pnx49xx_wdt_match,
	},
	.probe = pnx49xx_wdt_probe,
};

module_platform_driver(platform_wdt_driver);

MODULE_AUTHOR("BHmsWare <bhmsgamexbox2010@gmail.com>");
MODULE_DESCRIPTION("PNX49xx Watchdog Driver");
MODULE_LICENSE("GPL");
