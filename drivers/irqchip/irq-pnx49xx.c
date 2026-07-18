// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for NXP PNX49xx interrupt controller
 *
 * Copyright (C) 2026 BHmsWare <bhmsgamexbox2010@gmail.com>
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <asm/exception.h>

#define PNX49XX_IRQCTRL_ENABLE_SET		0x14
#define PNX49XX_IRQCTRL_ENABLE_CLR		0x18
#define PNX49XX_IRQCTRL_RAW_IRQ_INT		0x24
#define PNX49XX_IRQCTRL_HWI_EDGE_CLR	0x10
#define PNX49XX_IRQCTRL_RI_CLR			0x58

static void __iomem *base;
static struct irq_domain *pnx49xx_domain;

static void pnx49xx_irq_unmask(struct irq_data *d)
{
	writel(1 << d->hwirq, base + PNX49XX_IRQCTRL_ENABLE_SET);
}

static void pnx49xx_irq_mask(struct irq_data *d)
{
	writel(1 << d->hwirq, base + PNX49XX_IRQCTRL_ENABLE_CLR);
}

static struct irq_chip pnx49xx_irq_chip = {
	.name		= "pnx49xx-intc",
	.irq_mask	= pnx49xx_irq_mask,
	.irq_unmask	= pnx49xx_irq_unmask,
};

static int pnx49xx_irq_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &pnx49xx_irq_chip, handle_level_irq);
	return 0;
}

static const struct irq_domain_ops pnx49xx_domain_ops = {
	.map	= pnx49xx_irq_map,
	.xlate	= irq_domain_xlate_onecell,
};

static void __exception_irq_entry pnx49xx_handle_irq(struct pt_regs *regs)
{
	uint32_t status;

	status = readl_relaxed(base + PNX49XX_IRQCTRL_RAW_IRQ_INT);
	if (!status)
		return;

	generic_handle_domain_irq(pnx49xx_domain, __ffs(status));
}

static int __init pnx49xx_irq_init(struct device_node *node, struct device_node *parent)
{
	base = of_iomap(node, 0);
	if (!base)
		return -ENXIO;

	writel(0xffffff00, base + PNX49XX_IRQCTRL_HWI_EDGE_CLR);
	writel(0xff, base + PNX49XX_IRQCTRL_RI_CLR);
	writel(0xffffffff, base + PNX49XX_IRQCTRL_ENABLE_CLR);

	pnx49xx_domain = irq_domain_add_linear(node, 32, &pnx49xx_domain_ops, NULL);

	set_handle_irq(pnx49xx_handle_irq);
	return 0;
}

IRQCHIP_DECLARE(pnx49xx_intc, "nxp,pnx49xx-intc", pnx49xx_irq_init);
