// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPIO driver for NXP PNX49xx.
 *
 * Copyright (C) 2026 BHmsWare <bhmsgamexbox2010@gmail.com>
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>

#define PNX49XX_NUM_PINS		58
#define PNX49XX_MAX_PORTS		2
#define PNX49XX_PINS_PER_PORT	32

#define PNX49XX_GPIO_DIR				0x00
#define PNX49XX_GPIO_IN					0x04
#define PNX49XX_GPIO_PAD_TYPE0			0x14
#define PNX49XX_GPIO_PAD_TYPE1			0x18
#define PNX49XX_GPIO_OUT_EN_SET			0x40
#define PNX49XX_GPIO_OUT_EN_CLR			0x44

#define PNX49XX_GPIO_PIN_IC_POLARITY	0x04
#define PNX49XX_GPIO_PIN_IC_EDGE_CVT	0x0c
#define PNX49XX_GPIO_PIN_IC_EDGE_CLR	0x10
#define PNX49XX_GPIO_PIN_IC_INT			0x24
#define PNX49XX_GPIO_PIN_IC_EN_SET		0x30
#define PNX49XX_GPIO_PIN_IC_EN_CLR		0x34

struct pnx49xx_gpio_pin_ic {
	struct gpio_chip *gpio;
	void __iomem *base;
	int irq[PNX49XX_MAX_PORTS];
};

struct pnx49xx_gpio_chip {
	struct gpio_chip gpio;
	void __iomem *base;
	struct pnx49xx_gpio_pin_ic *pin_ic;
	spinlock_t lock;
};

static void pnx49xx_gpio_pin_ic_mask(struct irq_data *d)
{
	struct pnx49xx_gpio_chip *gc = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	struct pnx49xx_gpio_pin_ic *ic = gc->pin_ic;
	int port = d->hwirq / PNX49XX_PINS_PER_PORT;
	int pin = d->hwirq % PNX49XX_PINS_PER_PORT;

	writel(BIT(pin), ic->base + (port * 0x40) + PNX49XX_GPIO_PIN_IC_EN_CLR);
}

static void pnx49xx_gpio_pin_ic_unmask(struct irq_data *d)
{
	struct pnx49xx_gpio_chip *gc = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	struct pnx49xx_gpio_pin_ic *ic = gc->pin_ic;
	int port = d->hwirq / PNX49XX_PINS_PER_PORT;
	int pin = d->hwirq % PNX49XX_PINS_PER_PORT;

	writel(BIT(pin), ic->base + (port * 0x40) + PNX49XX_GPIO_PIN_IC_EN_SET);
}

static int pnx49xx_gpio_pin_ic_set_type(struct irq_data *d, unsigned int type)
{
	struct pnx49xx_gpio_chip *gc = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	struct pnx49xx_gpio_pin_ic *ic = gc->pin_ic;
	int port = d->hwirq / PNX49XX_PINS_PER_PORT;
	int pin = d->hwirq % PNX49XX_PINS_PER_PORT;
	void __iomem *port_base;
	u32 polarity, edge_cvt;

	port_base = ic->base + (port * 0x40);

	polarity = readl(port_base + PNX49XX_GPIO_PIN_IC_POLARITY);
	edge_cvt = readl(port_base + PNX49XX_GPIO_PIN_IC_EDGE_CVT);

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_HIGH:
		polarity |= BIT(pin);
		edge_cvt &= ~BIT(pin);
		break;

	case IRQ_TYPE_LEVEL_LOW:
		polarity &= ~BIT(pin);
		edge_cvt &= ~BIT(pin);
		break;

	case IRQ_TYPE_EDGE_RISING:
		polarity |= BIT(pin);
		edge_cvt |= BIT(pin);
		break;

	case IRQ_TYPE_EDGE_FALLING:
		polarity &= ~BIT(pin);
		edge_cvt |= BIT(pin);
		break;

	case IRQ_TYPE_EDGE_BOTH:
		edge_cvt |= BIT(pin);
		break;

	default:
		return -EINVAL;
	}

	writel(polarity, port_base + PNX49XX_GPIO_PIN_IC_POLARITY);
	writel(edge_cvt, port_base + PNX49XX_GPIO_PIN_IC_EDGE_CVT);

	if (type & IRQ_TYPE_LEVEL_MASK)
		irq_set_handler_locked(d, handle_level_irq);
	else
		irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static void pnx49xx_gpio_pin_ic_ack(struct irq_data *d)
{
	struct pnx49xx_gpio_chip *gc = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	struct pnx49xx_gpio_pin_ic *ic = gc->pin_ic;
	int port = d->hwirq / PNX49XX_PINS_PER_PORT;
	int pin = d->hwirq % PNX49XX_PINS_PER_PORT;

	writel(BIT(pin), ic->base + (0x40 * port) + PNX49XX_GPIO_PIN_IC_EDGE_CLR);
}

static void pnx49xx_gpio_pin_ic_handler(struct irq_desc *desc)
{
	struct pnx49xx_gpio_pin_ic *ic = irq_desc_get_handler_data(desc);
	struct gpio_chip *gc = ic->gpio;
	struct irq_chip *parent_chip = irq_desc_get_chip(desc);
	void __iomem *port_base;
	unsigned int current_irq = irq_desc_get_irq(desc);
	unsigned long status;
	int port, pin, i;

	for (i = 0; i < ARRAY_SIZE(ic->irq); i++) {
		if (ic->irq[i] == current_irq)
			port = i;
	}

	port_base = ic->base + (0x40 * port);

	chained_irq_enter(parent_chip, desc);

	status = readl(port_base + PNX49XX_GPIO_PIN_IC_INT);

	for_each_set_bit(pin, &status, PNX49XX_PINS_PER_PORT) {
		int hwirq = pin + (port * PNX49XX_PINS_PER_PORT);
		int virq = irq_find_mapping(gc->irq.domain, hwirq);

		generic_handle_irq(virq);
	}

	chained_irq_exit(parent_chip, desc);
}

static int pnx49xx_gpio_pin_ic_init_handlers(struct platform_device *pdev,
		struct pnx49xx_gpio_pin_ic *ic)
{
	int port, hwirq;

	for (port = 0; port < PNX49XX_MAX_PORTS; port++) {
		hwirq = ic->irq[port] = platform_get_irq(pdev, port);
		if (hwirq < 0)
			return hwirq;

		ic->irq[port] = hwirq;
		irq_set_chained_handler_and_data(hwirq, pnx49xx_gpio_pin_ic_handler, ic);
	}

	return 0;
}

static void pnx49xx_gpio_pin_ic_remove_handlers(struct pnx49xx_gpio_pin_ic *ic)
{
	int port, hwirq;

	for (port = 0; port < PNX49XX_MAX_PORTS; port++) {
		hwirq = ic->irq[port];

		if (hwirq > 0) {
			irq_set_chained_handler_and_data(hwirq, NULL, NULL);
			ic->irq[port] = 0;
		}
	}
}

static const struct irq_chip pnx49xx_gpio_pin_ic = {
	.name			= "PNX49xx GPIO pin",
	.irq_mask		= pnx49xx_gpio_pin_ic_mask,
	.irq_unmask		= pnx49xx_gpio_pin_ic_unmask,
	.irq_set_type	= pnx49xx_gpio_pin_ic_set_type,
	.irq_ack		= pnx49xx_gpio_pin_ic_ack,
	.flags		= IRQCHIP_IMMUTABLE | IRQCHIP_SET_TYPE_MASKED,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int pnx49xx_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct pnx49xx_gpio_chip *gc = gpiochip_get_data(chip);
	int port = offset / PNX49XX_PINS_PER_PORT;
	int pin = offset % PNX49XX_PINS_PER_PORT;

	writel(BIT(pin), gc->base + (0x20 * port) + (value ?
				PNX49XX_GPIO_OUT_EN_SET : PNX49XX_GPIO_OUT_EN_CLR));

	return 0;
}

static int pnx49xx_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct pnx49xx_gpio_chip *gc = gpiochip_get_data(chip);
	int port = offset / PNX49XX_PINS_PER_PORT;
	int pin = offset % PNX49XX_PINS_PER_PORT;

	return (readl(gc->base + (0x20 * port) + PNX49XX_GPIO_IN) >> pin) & 1;
}

static int pnx49xx_gpio_direction(struct gpio_chip *chip, unsigned int offset, bool out)
{
	struct pnx49xx_gpio_chip *gc = gpiochip_get_data(chip);
	int port = offset / PNX49XX_PINS_PER_PORT;
	int pin = offset % PNX49XX_PINS_PER_PORT;
	unsigned long flags;
	u32 dir;

	spin_lock_irqsave(&gc->lock, flags);
	dir = readl(gc->base + (0x20 * port) + PNX49XX_GPIO_DIR);
	if (out)
		dir &= ~BIT(pin);
	else
		dir |= BIT(pin);
	writel(dir, gc->base + (0x20 * port) + PNX49XX_GPIO_DIR);
	spin_unlock_irqrestore(&gc->lock, flags);

	return 0;
}

static int pnx49xx_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	return pnx49xx_gpio_direction(chip, offset, false);
}

static int pnx49xx_gpio_direction_output(struct gpio_chip *chip, unsigned int offset, int value)
{
	int ret;

	ret = pnx49xx_gpio_direction(chip, offset, true);
	if (ret)
		return ret;

	return pnx49xx_gpio_set(chip, offset, value);
}

static int pnx49xx_gpio_set_config(struct gpio_chip *chip, unsigned int offset,
		unsigned long config)
{
	struct pnx49xx_gpio_chip *gc = gpiochip_get_data(chip);
	int port = offset / PNX49XX_PINS_PER_PORT;
	int pin = offset % PNX49XX_PINS_PER_PORT;
	enum pin_config_param param = pinconf_to_config_param(config);
	u32 padt0, padt1;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&gc->lock, flags);

	padt0 = readl(gc->base + (0x20 * port) + PNX49XX_GPIO_PAD_TYPE0);
	padt1 = readl(gc->base + (0x20 * port) + PNX49XX_GPIO_PAD_TYPE1);

	switch (param) {
	case PIN_CONFIG_BIAS_PULL_UP:
		padt0 |= BIT(pin);
		padt1 &= ~BIT(pin);
		break;

	case PIN_CONFIG_DRIVE_PUSH_PULL:
		fallthrough;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		padt0 &= ~BIT(pin);
		padt1 &= ~BIT(pin);
		break;

	case PIN_CONFIG_BIAS_DISABLE:
		padt0 &= ~BIT(pin);
		padt1 |= BIT(pin);
		break;

	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		padt0 |= BIT(pin);
		padt1 |= BIT(pin);
		break;

	default:
		ret = -EOPNOTSUPP;
		goto err;
	}

	writel(padt0, gc->base + (0x20 * port) + PNX49XX_GPIO_PAD_TYPE0);
	writel(padt1, gc->base + (0x20 * port) + PNX49XX_GPIO_PAD_TYPE1);

err:
	spin_unlock_irqrestore(&gc->lock, flags);

	return ret;
}

static const struct gpio_chip pnx49xx_chip = {
	.label		= "pnx49xx-gpio",
	.request	= gpiochip_generic_request,
	.free		= gpiochip_generic_free,
	.direction_input	= pnx49xx_gpio_direction_input,
	.direction_output	= pnx49xx_gpio_direction_output,
	.set		= pnx49xx_gpio_set,
	.get		= pnx49xx_gpio_get,
	.set_config	= pnx49xx_gpio_set_config,
	.ngpio		= PNX49XX_NUM_PINS,
	.owner		= THIS_MODULE,
};

static int pnx49xx_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pnx49xx_gpio_chip *gc;
	struct pnx49xx_gpio_pin_ic *ic;
	int ret;

	/* gpio controller */

	gc = devm_kzalloc(dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	platform_set_drvdata(pdev, gc);

	gc->gpio = pnx49xx_chip;
	gc->gpio.base = -1;
	gc->gpio.parent = dev;

	gc->base = devm_platform_ioremap_resource_byname(pdev, "gpio");
	if (IS_ERR(gc->base))
		return PTR_ERR(gc->base);

	spin_lock_init(&gc->lock);

	/* gpio interrupt controller */

	ic = devm_kzalloc(dev, sizeof(*ic), GFP_KERNEL);
	if (!ic)
		return -ENOMEM;

	ic->gpio = &gc->gpio;
	ic->base = devm_platform_ioremap_resource_byname(pdev, "gpio-pin-ic");
	if (IS_ERR(ic->base))
		return PTR_ERR(ic->base);

	gpio_irq_chip_set_chip(&gc->gpio.irq, &pnx49xx_gpio_pin_ic);
	gc->gpio.irq.default_type = IRQ_TYPE_NONE;
	gc->gpio.irq.handler = handle_edge_irq;
	gc->pin_ic = ic;

	ret = pnx49xx_gpio_pin_ic_init_handlers(pdev, ic);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init irq handlers\n");

	ret = devm_gpiochip_add_data(dev, &gc->gpio, gc);
	if (ret)
		goto err;

	return 0;

err:
	pnx49xx_gpio_pin_ic_remove_handlers(ic);
	return dev_err_probe(dev, ret, "failed to add gpio chip\n");
}

static void pnx49xx_gpio_remove(struct platform_device *pdev)
{
	struct pnx49xx_gpio_chip *gc = platform_get_drvdata(pdev);
	struct pnx49xx_gpio_pin_ic *ic = gc->pin_ic;

	pnx49xx_gpio_pin_ic_remove_handlers(ic);
}

static const struct of_device_id pnx49xx_gpio_match[] = {
	{ .compatible = "nxp,pnx49xx-gpio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pnx49xx_gpio_match);

static struct platform_driver pnx49xx_gpio_driver = {
	.probe = pnx49xx_gpio_probe,
	.remove = pnx49xx_gpio_remove,
	.driver = {
		.name = "pnx49xx-gpio",
		.of_match_table = pnx49xx_gpio_match,
	},
};
module_platform_driver(pnx49xx_gpio_driver);

MODULE_AUTHOR("BHmsWare <bhmsgamexbox2010@gmail.com>");
MODULE_DESCRIPTION("GPIO driver for PNX49xx");
MODULE_LICENSE("GPL");
