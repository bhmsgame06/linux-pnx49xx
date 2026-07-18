// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Serial port driver for NXP PNX49xx SoC
 *
 * Copyright (C) 2026 BHmsWare <bhmsgamexbox2010@gmail.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <linux/of.h>
#include <linux/console.h>
#include <linux/tty_flip.h>

#define PNX49XX_NR_UARTS			2

#define PNX49XX_UART_FIFO_SIZE		96

/* regs */
#define PNX49XX_UART_MODE_REG		0x04
#define PNX49XX_UART_DIVIDER_REG	0x08
#define PNX49XX_UART_STATUS_REG		0x0c
#define PNX49XX_UART_RXTX_REG		0x10
#define PNX49XX_UART_IRQ_ID_REG		0x20
#define PNX49XX_UART_CTRL_SET_REG	0x60
#define PNX49XX_UART_CTRL_CLR_REG	0x64

/* control masks */
#define PNX49XX_UART_CTRL_RX_INT_MASK	0x4110
#define PNX49XX_UART_CTRL_TX_INT_MASK	0x0020

/* status masks */
#define PNX49XX_UART_STATUS_REG_RX_FULL_MASK	0x00001
#define PNX49XX_UART_STATUS_REG_TX_BUSY_MASK	0x20000
#define PNX49XX_UART_STATUS_REG_TX_EMPTY_MASK	0x80000

/* irq ids */
#define PNX49XX_UART_RX1_IRQ_ID	0x1c
#define PNX49XX_UART_RX2_IRQ_ID	0x34
#define PNX49XX_UART_TX_IRQ_ID	0x38

#define PNX49XX_UART_OSC_FREQ	26000000

#define MODNAME "pnx49xx_uart"

static struct uart_driver pnx49xx_uart_driver;

static struct uart_port ports[PNX49XX_NR_UARTS];

/* --- Early console --- */

#ifdef CONFIG_SERIAL_EARLYCON
static void pnx49xx_early_console_putchar(struct uart_port *port, unsigned char ch)
{
	while (readl(port->membase + PNX49XX_UART_STATUS_REG) &
			PNX49XX_UART_STATUS_REG_TX_BUSY_MASK)
		cpu_relax();

	writeb(ch, port->membase + PNX49XX_UART_RXTX_REG);
}

static void pnx49xx_early_console_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, pnx49xx_early_console_putchar);
}

static int __init pnx49xx_early_console_setup(struct earlycon_device *device,
		const char *options)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = pnx49xx_early_console_write;
	return 0;
}

OF_EARLYCON_DECLARE(pnx49xx_uart, "nxp,pnx49xx-uart", pnx49xx_early_console_setup);
#endif /* CONFIG_SERIAL_EARLYCON */

/* --- General UART driver --- */

static unsigned int pnx49xx_uart_tx_empty(struct uart_port *port)
{
	return (readl(port->membase + PNX49XX_UART_STATUS_REG) &
			PNX49XX_UART_STATUS_REG_TX_EMPTY_MASK) ? 1 : 0;
}

static void pnx49xx_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int pnx49xx_uart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

static void pnx49xx_uart_start_tx(struct uart_port *port)
{
	writel(PNX49XX_UART_CTRL_TX_INT_MASK, port->membase + PNX49XX_UART_CTRL_SET_REG);
}

static void pnx49xx_uart_stop_tx(struct uart_port *port)
{
	writel(PNX49XX_UART_CTRL_TX_INT_MASK, port->membase + PNX49XX_UART_CTRL_CLR_REG);
}

static void pnx49xx_uart_stop_rx(struct uart_port *port)
{
	writel(PNX49XX_UART_CTRL_RX_INT_MASK, port->membase + PNX49XX_UART_CTRL_CLR_REG);
}

static irqreturn_t pnx49xx_uart_interrupt(int irq, void *dev_id)
{
	struct uart_port *port = dev_id;
	unsigned int status;
	int irq_id;
	u8 ch;

	irq_id = readl(port->membase + PNX49XX_UART_IRQ_ID_REG);
	status = readl(port->membase + PNX49XX_UART_STATUS_REG);

	if (irq_id == PNX49XX_UART_TX_IRQ_ID) {

		uart_port_tx(port, ch,
				!(readl(port->membase + PNX49XX_UART_STATUS_REG) &
					PNX49XX_UART_STATUS_REG_TX_BUSY_MASK),
				writeb(ch, port->membase + PNX49XX_UART_RXTX_REG)
		);

	}

	if (irq_id == PNX49XX_UART_RX1_IRQ_ID || irq_id == PNX49XX_UART_RX2_IRQ_ID) {

		writel(PNX49XX_UART_CTRL_RX_INT_MASK,
				port->membase + PNX49XX_UART_CTRL_CLR_REG);

		do {
			ch = readb(port->membase + PNX49XX_UART_RXTX_REG);

			port->icount.rx++;

			tty_insert_flip_char(&port->state->port, ch, TTY_NORMAL);
			tty_flip_buffer_push(&port->state->port);

		} while (readl(port->membase + PNX49XX_UART_STATUS_REG) & 1);

		writel(PNX49XX_UART_CTRL_RX_INT_MASK,
				port->membase + PNX49XX_UART_CTRL_SET_REG);

	}

	return IRQ_HANDLED;
}

static int pnx49xx_uart_startup(struct uart_port *port)
{
	int ret;

	if (port->state->port.tty && (port->state->port.tty->termios.c_cflag & CREAD))
		writel(PNX49XX_UART_CTRL_RX_INT_MASK, port->membase + PNX49XX_UART_CTRL_SET_REG);

	ret = request_irq(port->irq, pnx49xx_uart_interrupt, 0,
			dev_name(port->dev), port);
	return 0;
}

static void pnx49xx_uart_shutdown(struct uart_port *port)
{
	free_irq(port->irq, port);
}

static void pnx49xx_uart_set_termios(struct uart_port *port,
		struct ktermios *termios,
		const struct ktermios *old)
{
	unsigned int mode, baud;

	mode = readl(port->membase + PNX49XX_UART_MODE_REG);
	switch (termios->c_cflag & CSIZE) {
	case CS7:
		mode |= 1;
		break;

	default:
		mode &= ~1;
		break;
	}

	writel(mode, port->membase + PNX49XX_UART_MODE_REG);

	baud = uart_get_baud_rate(port, termios, NULL, 0, port->uartclk);
	writel(((port->uartclk / 16) / baud) - 1, port->membase + PNX49XX_UART_DIVIDER_REG);
}

static void pnx49xx_uart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_PNX49XX;
}

static const char *pnx49xx_uart_type(struct uart_port *port)
{
	return MODNAME;
}

static const struct uart_ops pnx49xx_uart_ops = {
	.tx_empty	= pnx49xx_uart_tx_empty,
	.set_mctrl	= pnx49xx_uart_set_mctrl,
	.get_mctrl	= pnx49xx_uart_get_mctrl,
	.start_tx	= pnx49xx_uart_start_tx,
	.stop_tx	= pnx49xx_uart_stop_tx,
	.stop_rx	= pnx49xx_uart_stop_rx,
	.startup	= pnx49xx_uart_startup,
	.shutdown	= pnx49xx_uart_shutdown,
	.set_termios	= pnx49xx_uart_set_termios,
	.config_port	= pnx49xx_uart_config_port,
	.type		= pnx49xx_uart_type,
};

#ifdef CONFIG_SERIAL_PNX49XX_CONSOLE
static void pnx49xx_console_putchar(struct uart_port *port, unsigned char ch)
{
	while (readl_relaxed(port->membase + PNX49XX_UART_STATUS_REG) &
			PNX49XX_UART_STATUS_REG_TX_BUSY_MASK)
		cpu_relax();

	writeb(ch, port->membase + PNX49XX_UART_RXTX_REG);
}

static void pnx49xx_console_write(struct console *co, const char *s, unsigned int count)
{
	struct uart_port *port = &ports[co->index];

	uart_console_write(port, s, count, pnx49xx_console_putchar);
}

static int pnx49xx_console_setup(struct console *co, char *options)
{
	struct uart_port *port;
	int baud = 9600;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index < 0 || co->index >= PNX49XX_NR_UARTS)
		return -ENODEV;

	port = &ports[co->index];

	if (!port->membase)
		return -ENOMEM;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct console pnx49xx_console = {
	.name		= "ttyS",
	.setup		= pnx49xx_console_setup,
	.device		= uart_console_device,
	.write		= pnx49xx_console_write,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &pnx49xx_uart_driver,
};

static int __init pnx49xx_console_init(void)
{
	register_console(&pnx49xx_console);
	return 0;
}
console_initcall(pnx49xx_console_init);

#define PNX49XX_SERIAL_CONSOLE (&pnx49xx_console)

#else
#define PNX49XX_SERIAL_CONSOLE NULL
#endif /* CONFIG_SERIAL_PNX49XX_CONSOLE */

static struct uart_driver pnx49xx_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= MODNAME,
	.dev_name	= "ttyS",
	.major		= TTY_MAJOR,
	.minor		= 64,
	.nr			= PNX49XX_NR_UARTS,
	.cons		= PNX49XX_SERIAL_CONSOLE,
};

static int pnx49xx_uart_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct uart_port *port;
	int ret;

	pdev->id = of_alias_get_id(pdev->dev.of_node, "serial");

	if (pdev->id < 0 || pdev->id >= PNX49XX_NR_UARTS)
		return -EINVAL;

	port = &ports[pdev->id];
	if (port->membase)
		return -EBUSY;
	memset(port, 0, sizeof(*port));

	port->membase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);
	port->mapbase = res->start;

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	port->irq = ret;

	port->iotype = UPIO_MEM32;
	port->regshift = 0;
	port->dev = &pdev->dev;
	port->type = PORT_PNX49XX;
	port->ops = &pnx49xx_uart_ops;
	port->fifosize = PNX49XX_UART_FIFO_SIZE;
	port->uartclk = PNX49XX_UART_OSC_FREQ * 16;
	port->line = pdev->id;
	port->flags = UPF_BOOT_AUTOCONF;
	spin_lock_init(&port->lock);

	ret = uart_add_one_port(&pnx49xx_uart_driver, port);
	if (ret) {
		ports[pdev->id].membase = NULL;
		return ret;
	}

	platform_set_drvdata(pdev, port);
	return 0;
}

static void pnx49xx_uart_remove(struct platform_device *pdev)
{
	struct uart_port *port;

	port = platform_get_drvdata(pdev);
	if (port) {
		uart_remove_one_port(&pnx49xx_uart_driver, port);
		ports[pdev->id].membase = NULL;
	}
}

static const struct of_device_id pnx49xx_uart_of_match[] = {
	{ .compatible = "nxp,pnx49xx-uart", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pnx49xx_uart_of_match);

static struct platform_driver pnx49xx_platform_driver = {
	.probe		= pnx49xx_uart_probe,
	.remove		= pnx49xx_uart_remove,
	.driver		= {
		.name	= MODNAME,
		.of_match_table	= pnx49xx_uart_of_match,
	},
};

static int __init pnx49xx_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&pnx49xx_uart_driver);
	if (ret)
		return ret;

	return platform_driver_register(&pnx49xx_platform_driver);
}

static void __exit pnx49xx_uart_exit(void)
{
	platform_driver_unregister(&pnx49xx_platform_driver);
	uart_unregister_driver(&pnx49xx_uart_driver);
}

module_init(pnx49xx_uart_init);
module_exit(pnx49xx_uart_exit);

MODULE_AUTHOR("BHmsWare <bhmsgamexbox2010@gmail.com>");
MODULE_DESCRIPTION("NXP PNX49XX UART driver");
MODULE_LICENSE("GPL");
