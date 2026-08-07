// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for Samsung S6B33B controllers
 *
 * Copyright (C) 2026 BHmsWare <bhmsgamexbox2010@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_mipi_dbi.h>

/* display properties */
#define S6B33B_DISPLAY_WIDTH		128
#define S6B33B_DISPLAY_HEIGHT		128
#define S6B33B_DISPLAY_WIDTH_MM		27
#define S6B33B_DISPLAY_HEIGHT_MM	27
#define S6B33B_DISPLAY_DEPTH		16

#define S6B33B_DISPLAY_HFP			2
#define S6B33B_DISPLAY_HSYNC		1
#define S6B33B_DISPLAY_HTOTAL		132
#define S6B33B_DISPLAY_VFP			2
#define S6B33B_DISPLAY_VSYNC		1
#define S6B33B_DISPLAY_VTOTAL		132

#define S6B33B_DISPLAY_CLOCK		871

/* commands */
#define S6B33B_CMD_DRIVER_OUTPUT_MODE			0x10
#define S6B33B_CMD_DCDC_CLK_DIV					0x24
#define S6B33B_CMD_TEMP_COMPENSATION			0x28
#define S6B33B_CMD_CONTRAST_CTL					0x2a
#define S6B33B_CMD_STANDBY_MODE_OFF				0x2c
#define S6B33B_CMD_ADDR_MODE					0x30
#define S6B33B_CMD_ROW_VEC_MODE					0x32
#define S6B33B_CMD_N_BLK_INV					0x34
#define S6B33B_CMD_ENTRY_MODE					0x40
#define S6B33B_CMD_ROW_ADDR_AREA				0x42
#define S6B33B_CMD_COL_ADDR_AREA				0x43
#define S6B33B_CMD_RAM_SKIP_AREA				0x45
#define S6B33B_CMD_DISPLAY_OFF					0x50
#define S6B33B_CMD_DISPLAY_ON					0x51
#define S6B33B_CMD_SPEC_DISPLAY_PATTERN			0x53

/* bits */
#define S6B33B_BIT_DRIVER_OUTPUT_MODE_CDIR		BIT(0)
#define S6B33B_BIT_DRIVER_OUTPUT_MODE_SWP		BIT(1)
#define S6B33B_BIT_DRIVER_OUTPUT_MODE_MX		BIT(2)
#define S6B33B_BIT_DRIVER_OUTPUT_MODE_MY		BIT(3)

#define S6B33B_BIT_ADDR_MODE_SGM				BIT(0)
#define S6B33B_BIT_ADDR_MODE_SGP_1PX			BIT(1)
#define S6B33B_BIT_ADDR_MODE_DSG				BIT(4)

#define S6B33B_BIT_ROW_VEC_MODE_INC_SUBFRAME	(7 << 1)

#define S6B33B_BIT_ENTRY_MODE_8BIT_BUS			BIT(7)

struct s6b33b_device {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	void __iomem *cmd_base;
	void __iomem *data_base;

	struct gpio_desc *cs_gpio;
	struct gpio_desc *reset_gpio;
};

static const u32 s6b33b_formats[] = {
	DRM_FORMAT_RGB565,
};

static void s6b33b_command(struct s6b33b_device *s6b33b,
		u8 cmd)
{
	writew(cmd, s6b33b->cmd_base);
}

static void s6b33b_data(struct s6b33b_device *s6b33b,
		u16 data)
{
	writew(data >> 8, s6b33b->data_base);
	writew(data & 0xff, s6b33b->data_base);
}

static void s6b33b_enable(struct drm_simple_display_pipe *pipe,
		struct drm_crtc_state *crtc_state,
		struct drm_plane_state *plane_state)
{
	struct s6b33b_device *s6b33b = container_of(pipe->crtc.dev, struct s6b33b_device, drm);

	gpiod_set_value(s6b33b->reset_gpio, 1);
	msleep(20);
	gpiod_set_value(s6b33b->reset_gpio, 0);
	gpiod_set_value(s6b33b->cs_gpio, 1);

	s6b33b_command(s6b33b, S6B33B_CMD_STANDBY_MODE_OFF);

	msleep(20);

	s6b33b_command(s6b33b, S6B33B_CMD_TEMP_COMPENSATION);
	s6b33b_command(s6b33b, 0x01);

	s6b33b_command(s6b33b, S6B33B_CMD_RAM_SKIP_AREA);
	s6b33b_command(s6b33b, 0x00);

	s6b33b_command(s6b33b, S6B33B_CMD_SPEC_DISPLAY_PATTERN);
	s6b33b_command(s6b33b, 0x00);

	s6b33b_command(s6b33b, S6B33B_CMD_DRIVER_OUTPUT_MODE);
	s6b33b_command(s6b33b,
			S6B33B_BIT_DRIVER_OUTPUT_MODE_CDIR |
			S6B33B_BIT_DRIVER_OUTPUT_MODE_SWP |
			S6B33B_BIT_DRIVER_OUTPUT_MODE_MX |
			S6B33B_BIT_DRIVER_OUTPUT_MODE_MY);

	s6b33b_command(s6b33b, S6B33B_CMD_DCDC_CLK_DIV);
	s6b33b_command(s6b33b, 0x03);

	s6b33b_command(s6b33b, S6B33B_CMD_ADDR_MODE);
	s6b33b_command(s6b33b,
			S6B33B_BIT_ADDR_MODE_SGM |
			S6B33B_BIT_ADDR_MODE_SGP_1PX |
			S6B33B_BIT_ADDR_MODE_DSG);

	s6b33b_command(s6b33b, S6B33B_CMD_ROW_VEC_MODE);
	s6b33b_command(s6b33b, S6B33B_BIT_ROW_VEC_MODE_INC_SUBFRAME);

	s6b33b_command(s6b33b, S6B33B_CMD_N_BLK_INV);
	s6b33b_command(s6b33b, 0x0d);

	s6b33b_command(s6b33b, S6B33B_CMD_ENTRY_MODE);
	s6b33b_command(s6b33b, S6B33B_BIT_ENTRY_MODE_8BIT_BUS);

	s6b33b_command(s6b33b, S6B33B_CMD_CONTRAST_CTL);
	s6b33b_command(s6b33b, 0x43);

	s6b33b_command(s6b33b, S6B33B_CMD_DISPLAY_OFF);
}

static void s6b33b_disable(struct drm_simple_display_pipe *pipe)
{
	struct s6b33b_device *s6b33b = container_of(pipe->crtc.dev, struct s6b33b_device, drm);

	gpiod_set_value(s6b33b->cs_gpio, 0);
	gpiod_set_value(s6b33b->reset_gpio, 1);
}

static void s6b33b_update(struct drm_simple_display_pipe *pipe,
		struct drm_plane_state *old_plane_state)
{
	struct s6b33b_device *s6b33b = container_of(pipe->crtc.dev, struct s6b33b_device, drm);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_framebuffer *fb = state->fb;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	struct drm_rect rect;
	int ret, x, y;
	u16 *vaddr;

	if (!drm_atomic_helper_damage_merged(old_plane_state, pipe->plane.state, &rect))
		return;

	ret = drm_gem_fb_vmap(fb, map, NULL);
	if (ret)
		return;

	vaddr = map[0].vaddr;

	s6b33b_command(s6b33b, S6B33B_CMD_COL_ADDR_AREA);
	s6b33b_command(s6b33b, rect.x1 + 2);
	s6b33b_command(s6b33b, rect.x2 + 1);
	s6b33b_command(s6b33b, S6B33B_CMD_ROW_ADDR_AREA);
	s6b33b_command(s6b33b, rect.y1 + 2);
	s6b33b_command(s6b33b, rect.y2 + 1);

	for (y = rect.y1; y < rect.y2; y++) {
		for (x = rect.x1; x < rect.x2; x++) {
			int offset = (y * (fb->pitches[0] / 2)) + x;
			u16 pixel_data = vaddr[offset];

			s6b33b_data(s6b33b, pixel_data);
		}
	}

	drm_gem_fb_vunmap(fb, map);

	s6b33b_command(s6b33b, S6B33B_CMD_DISPLAY_ON);
}

static const struct drm_simple_display_pipe_funcs s6b33b_pipe_funcs = {
	.enable  = s6b33b_enable,
	.disable = s6b33b_disable,
	.update  = s6b33b_update,
};

static const struct drm_display_mode s6b33b_display_mode = {
	.clock			= S6B33B_DISPLAY_CLOCK,
	.hdisplay		= S6B33B_DISPLAY_WIDTH,
	.hsync_start	= S6B33B_DISPLAY_WIDTH + S6B33B_DISPLAY_HFP,
	.hsync_end		= S6B33B_DISPLAY_WIDTH + S6B33B_DISPLAY_HFP + S6B33B_DISPLAY_HSYNC,
	.htotal			= S6B33B_DISPLAY_HTOTAL,
	.vdisplay		= S6B33B_DISPLAY_HEIGHT,
	.vsync_start	= S6B33B_DISPLAY_HEIGHT + S6B33B_DISPLAY_VFP,
	.vsync_end		= S6B33B_DISPLAY_HEIGHT + S6B33B_DISPLAY_VFP + S6B33B_DISPLAY_VSYNC,
	.vtotal			= S6B33B_DISPLAY_VTOTAL,
	.width_mm		= S6B33B_DISPLAY_WIDTH_MM,
	.height_mm		= S6B33B_DISPLAY_HEIGHT_MM,
	.type			= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int s6b33b_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &s6b33b_display_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_helper_funcs s6b33b_conn_helper = {
	.get_modes = s6b33b_get_modes,
};

static const struct drm_connector_funcs s6b33b_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs s6b33b_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(s6b33b_fops);

static const struct drm_driver s6b33b_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops				= &s6b33b_fops,
	.name				= "s6b33b",
	.desc				= "Samsung S6B33B",
	.major				= 1,
	.minor				= 0,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
};

static int s6b33b_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s6b33b_device *s6b33b;
	int ret;

	s6b33b = devm_drm_dev_alloc(dev, &s6b33b_driver, struct s6b33b_device, drm);
	if (IS_ERR(s6b33b))
		return PTR_ERR(s6b33b);

	s6b33b->cmd_base = devm_platform_ioremap_resource_byname(pdev, "cmd");
	if (IS_ERR(s6b33b->cmd_base))
		return PTR_ERR(s6b33b->cmd_base);

	s6b33b->data_base = devm_platform_ioremap_resource_byname(pdev, "data");
	if (IS_ERR(s6b33b->data_base))
		return PTR_ERR(s6b33b->data_base);

	s6b33b->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	s6b33b->cs_gpio = devm_gpiod_get(dev, "cs", GPIOD_OUT_LOW);

	ret = drmm_mode_config_init(&s6b33b->drm);
	if (ret)
		return ret;

	s6b33b->drm.mode_config.min_width = S6B33B_DISPLAY_WIDTH;
	s6b33b->drm.mode_config.max_width = S6B33B_DISPLAY_WIDTH;
	s6b33b->drm.mode_config.min_height = S6B33B_DISPLAY_HEIGHT;
	s6b33b->drm.mode_config.max_height = S6B33B_DISPLAY_HEIGHT;
	s6b33b->drm.mode_config.preferred_depth = S6B33B_DISPLAY_DEPTH;
	s6b33b->drm.mode_config.funcs = &s6b33b_mode_config_funcs;

	ret = drm_connector_init(&s6b33b->drm,
			&s6b33b->connector,
			&s6b33b_conn_funcs,
			DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;

	drm_connector_helper_add(&s6b33b->connector, &s6b33b_conn_helper);

	ret = drm_simple_display_pipe_init(&s6b33b->drm,
			&s6b33b->pipe,
			&s6b33b_pipe_funcs,
			s6b33b_formats, ARRAY_SIZE(s6b33b_formats),
			NULL,
			&s6b33b->connector);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&s6b33b->pipe.plane);

	drm_mode_config_reset(&s6b33b->drm);

	ret = drm_dev_register(&s6b33b->drm, 0);
	if (ret)
		return ret;

	drm_client_setup(&s6b33b->drm, NULL);

	platform_set_drvdata(pdev, s6b33b);

	return 0;
}

static void s6b33b_remove(struct platform_device *pdev)
{
	struct s6b33b_device *s6b33b = platform_get_drvdata(pdev);

	drm_dev_unplug(&s6b33b->drm);
	drm_atomic_helper_shutdown(&s6b33b->drm);
}

static const struct of_device_id s6b33b_of_match[] = {
	{ .compatible = "samsung,s6b33b" },
	{ },
};
MODULE_DEVICE_TABLE(of, s6b33b_of_match);

static struct platform_driver s6b33b_platform_driver = {
	.probe = s6b33b_probe,
	.remove = s6b33b_remove,
	.driver = {
		.name = "s6b33b",
		.of_match_table = s6b33b_of_match,
	},
};
module_platform_driver(s6b33b_platform_driver);

MODULE_AUTHOR("BHmsWare <bhmsgamexbox2010@gmail.com>");
MODULE_DESCRIPTION("Samsung S6B33B DRM driver");
MODULE_LICENSE("GPL");
