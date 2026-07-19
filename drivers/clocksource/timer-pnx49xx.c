// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Clocksource driver for NXP PNX49xx timer
 *
 * Copyright (C) 2026 BHmsWare <bhmsgamexbox2010@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/sched_clock.h>
#include <linux/delay.h>

/* regs */
#define PNX49XX_TIMER0_READ			0x08
#define PNX49XX_TIMER0_CTRL_SET		0x80
#define PNX49XX_TIMER1_RELOAD		0x24
#define PNX49XX_TIMER1_CTRL_SET		0xa0
#define PNX49XX_TIMER1_CTRL_CLR		0xa4
#define PNX49XX_TIMER_INT_STATUS	0x54
#define PNX49XX_TIMER_INT_ENABLE	0x68
#define PNX49XX_TIMER_SYNC			0x40

/* control masks */
#define PNX49XX_TIMER_DIR_UP		BIT(16)
#define PNX49XX_TIMER_CYCLIC		BIT(18)
#define PNX49XX_TIMER_ENABLE_MASK	(BIT(19) | BIT(10))

static void __iomem *base;

static void pnx49xx_clkevt_sync_reload(void)
{
	writel_relaxed(0x08, base + PNX49XX_TIMER_SYNC);
}

static u64 notrace pnx49xx_sched_clock_read(void)
{
	return readl(base + PNX49XX_TIMER0_READ);
}

static irqreturn_t pnx49xx_clkevt_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	readl(base + PNX49XX_TIMER_INT_STATUS);
	writel_relaxed(PNX49XX_TIMER_ENABLE_MASK, base + PNX49XX_TIMER1_CTRL_CLR);

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static int pnx49xx_clkevt_set_next_event(unsigned long evt, struct clock_event_device *ced)
{
	writel_relaxed(evt, base + PNX49XX_TIMER1_RELOAD);
	pnx49xx_clkevt_sync_reload();

	writel_relaxed(PNX49XX_TIMER_ENABLE_MASK, base + PNX49XX_TIMER1_CTRL_SET);

	return 0;
}

static int pnx49xx_clkevt_shutdown(struct clock_event_device *ced)
{
	writel_relaxed(PNX49XX_TIMER_ENABLE_MASK, base + PNX49XX_TIMER1_CTRL_CLR);

	return 0;
}

static struct clock_event_device pnx49xx_clockevent = {
	.name				= "pnx49xx_clockevent",
	.features			= CLOCK_EVT_FEAT_ONESHOT,
	.rating				= 300,
	.set_next_event		= pnx49xx_clkevt_set_next_event,
	.set_state_shutdown	= pnx49xx_clkevt_shutdown,
};

static int __init pnx49xx_timer_init(struct device_node *np)
{
	u32 rate;
	int irq, ret;

	/* setting up sched clocksource (timer 0) */

	base = of_iomap(np, 0);
	if (!base) {
		pr_err("unable to map registers\n");
		return -ENXIO;
	}

	ret = of_property_read_u32(np, "clock-frequency", &rate);
	if (ret) {
		pr_err("cannot get clock-frequency value\n");
		return ret;
	}

	writel_relaxed(PNX49XX_TIMER_DIR_UP | PNX49XX_TIMER_CYCLIC | PNX49XX_TIMER_ENABLE_MASK,
			base + PNX49XX_TIMER0_CTRL_SET);

	ret = clocksource_mmio_init(base + PNX49XX_TIMER0_READ, "pnx49xx_clocksource",
			rate, 300, 32, clocksource_mmio_readl_up);
	if (ret) {
		pr_err("cannot register clocksource\n");
		return ret;
	}
	sched_clock_register(pnx49xx_sched_clock_read, 32, rate);

	/* setting up sched timer (timer 1) */

	writel_relaxed(0x10000, base + PNX49XX_TIMER_INT_ENABLE);

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("get irq failed\n");
		return -EINVAL;
	}

	ret = request_irq(irq,
			pnx49xx_clkevt_interrupt,
			IRQF_TIMER,
			"pnx49xx_clockevent",
			&pnx49xx_clockevent);

	if (ret) {
		pr_err("request irq failed\n");
		return ret;
	}

	pnx49xx_clockevent.cpumask = cpumask_of(0);
	clockevents_config_and_register(&pnx49xx_clockevent, rate, 1, -1);

	return 0;
}

TIMER_OF_DECLARE(pnx49xx_timer, "nxp,pnx49xx-timer", pnx49xx_timer_init);
