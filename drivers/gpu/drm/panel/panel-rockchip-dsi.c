/*
 * drivers/gpu/drm/panel/panel-rockchip-dsi.c
 *
 * Generic MIPI DSI Panel Driver for Rockchip platforms
 * Parses init commands from Device Tree (Ported from RK 6.1 BSP)
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_modes.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>
#include <video/mipi_display.h>

struct panel_cmd_header {
	u8 data_type;
	u8 delay;
	u8 payload_length;
} __packed;

struct panel_cmd_desc {
	struct panel_cmd_header header;
	const u8 *payload;
};

struct panel_cmd_seq {
	struct panel_cmd_desc *cmds;
	unsigned int cmd_cnt;
};

struct rockchip_dsi_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct regulator *supply;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;

	struct panel_cmd_seq *init_seq;
	struct panel_cmd_seq *exit_seq;

	struct display_timing *timing;
	unsigned int bpc;
	
	struct {
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
		unsigned int reset;
		unsigned int init;
	} delay;

	bool prepared;
	bool enabled;
};

static inline struct rockchip_dsi_panel *to_rockchip_dsi_panel(struct drm_panel *panel)
{
	return container_of(panel, struct rockchip_dsi_panel, base);
}

static int panel_parse_cmd_seq(struct device *dev, const u8 *data, int length,
			       struct panel_cmd_seq *seq)
{
	struct panel_cmd_header *header;
	unsigned int i, cnt;
	const u8 *d = data;
	int len = length;

	if (!seq)
		return -EINVAL;

	/* First pass: count commands */
	cnt = 0;
	while (len > sizeof(*header)) {
		header = (struct panel_cmd_header *)d;
		d += sizeof(*header);
		len -= sizeof(*header);

		if (header->payload_length > len)
			return -EINVAL;

		d += header->payload_length;
		len -= header->payload_length;
		cnt++;
	}

	if (len)
		return -EINVAL;

	seq->cmd_cnt = cnt;
	seq->cmds = devm_kcalloc(dev, cnt, sizeof(struct panel_cmd_desc), GFP_KERNEL);
	if (!seq->cmds)
		return -ENOMEM;

	/* Second pass: populate commands */
	d = data;
	len = length;
	for (i = 0; i < cnt; i++) {
		header = (struct panel_cmd_header *)d;
		d += sizeof(*header);
		
		seq->cmds[i].header = *header;
		seq->cmds[i].payload = d;

		d += header->payload_length;
	}

	return 0;
}

static int panel_dsi_xfer_cmd_seq(struct rockchip_dsi_panel *panel,
				  struct panel_cmd_seq *seq)
{
	struct mipi_dsi_device *dsi = panel->dsi;
	struct device *dev = &dsi->dev;
	int i, err = 0;

	if (!seq)
		return 0;

	for (i = 0; i < seq->cmd_cnt; i++) {
		struct panel_cmd_desc *cmd = &seq->cmds[i];
		size_t len = cmd->header.payload_length;

		switch (cmd->header.data_type) {
		case MIPI_DSI_DCS_SHORT_WRITE:
		case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		case MIPI_DSI_DCS_LONG_WRITE:
			err = mipi_dsi_dcs_write_buffer(dsi, cmd->payload, len);
			break;
		case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
		case MIPI_DSI_GENERIC_LONG_WRITE:
			err = mipi_dsi_generic_write(dsi, cmd->payload, len);
			break;
		default:
			/* Fallback to generic if unsure, or ignore specific ones like delays if handled separately */
			err = mipi_dsi_generic_write(dsi, cmd->payload, len);
			break;
		}

		if (err < 0) {
			dev_err(dev, "failed to write cmd %d: %d\n", i, err);
			return err;
		}

		if (cmd->header.delay)
			msleep(cmd->header.delay);
	}

	return 0;
}

static int rockchip_dsi_panel_disable(struct drm_panel *panel)
{
	struct rockchip_dsi_panel *p = to_rockchip_dsi_panel(panel);

	if (!p->enabled)
		return 0;

	if (p->delay.disable)
		msleep(p->delay.disable);

	p->enabled = false;
	return 0;
}

static int rockchip_dsi_panel_unprepare(struct drm_panel *panel)
{
	struct rockchip_dsi_panel *p = to_rockchip_dsi_panel(panel);

	if (!p->prepared)
		return 0;

	panel_dsi_xfer_cmd_seq(p, p->exit_seq);

	gpiod_set_value_cansleep(p->reset_gpio, 1);
	gpiod_set_value_cansleep(p->enable_gpio, 0);
	
	if (p->supply)
		regulator_disable(p->supply);

	if (p->delay.unprepare)
		msleep(p->delay.unprepare);

	p->prepared = false;
	return 0;
}

static int rockchip_dsi_panel_prepare(struct drm_panel *panel)
{
	struct rockchip_dsi_panel *p = to_rockchip_dsi_panel(panel);
	int err;

	if (p->prepared)
		return 0;

	if (p->supply) {
		err = regulator_enable(p->supply);
		if (err < 0) {
			dev_err(panel->dev, "failed to enable supply: %d\n", err);
			return err;
		}
	}

	if (p->delay.prepare)
		msleep(p->delay.prepare);

	gpiod_set_value_cansleep(p->enable_gpio, 1);

	/* Toggling Reset */
	gpiod_set_value_cansleep(p->reset_gpio, 1);
	if (p->delay.reset)
		msleep(p->delay.reset);
	gpiod_set_value_cansleep(p->reset_gpio, 0);

	if (p->delay.init)
		msleep(p->delay.init);

	err = panel_dsi_xfer_cmd_seq(p, p->init_seq);
	if (err < 0) {
		dev_err(panel->dev, "failed to send init sequence\n");
		goto err_unprepare;
	}

	p->prepared = true;
	return 0;

err_unprepare:
	if (p->supply)
		regulator_disable(p->supply);
	return err;
}

static int rockchip_dsi_panel_enable(struct drm_panel *panel)
{
	struct rockchip_dsi_panel *p = to_rockchip_dsi_panel(panel);

	if (p->enabled)
		return 0;

	if (p->delay.enable)
		msleep(p->delay.enable);

	p->enabled = true;
	return 0;
}

static int rockchip_dsi_panel_get_modes(struct drm_panel *panel,
					struct drm_connector *connector)
{
	struct rockchip_dsi_panel *p = to_rockchip_dsi_panel(panel);
	struct drm_display_mode *mode;
	struct videomode vm;

	if (!p->timing)
		return 0;

	videomode_from_timing(p->timing, &vm);
	
	mode = drm_mode_create(connector->dev);
	if (!mode) {
		dev_err(panel->dev, "failed to create mode\n");
		return 0;
	}

	drm_display_mode_from_videomode(&vm, mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.bpc = p->bpc;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs rockchip_dsi_panel_funcs = {
	.disable = rockchip_dsi_panel_disable,
	.unprepare = rockchip_dsi_panel_unprepare,
	.prepare = rockchip_dsi_panel_prepare,
	.enable = rockchip_dsi_panel_enable,
	.get_modes = rockchip_dsi_panel_get_modes,
};

static int rockchip_dsi_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rockchip_dsi_panel *panel;
	const void *data;
	int len, err;
	u32 val;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, panel);

	/* 1. Regulators & GPIOs */
	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply))
		return dev_err_probe(dev, PTR_ERR(panel->supply), "failed to get power regulator\n");

	panel->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(panel->reset_gpio), "failed to get reset gpio\n");

	panel->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(panel->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(panel->enable_gpio), "failed to get enable gpio\n");

	/* 2. Delays */
	of_property_read_u32(dev->of_node, "prepare-delay-ms", &panel->delay.prepare);
	of_property_read_u32(dev->of_node, "enable-delay-ms", &panel->delay.enable);
	of_property_read_u32(dev->of_node, "disable-delay-ms", &panel->delay.disable);
	of_property_read_u32(dev->of_node, "unprepare-delay-ms", &panel->delay.unprepare);
	of_property_read_u32(dev->of_node, "reset-delay-ms", &panel->delay.reset);
	of_property_read_u32(dev->of_node, "init-delay-ms", &panel->delay.init);

	/* 3. DSI Config */
	if (!of_property_read_u32(dev->of_node, "dsi,flags", &val))
		dsi->mode_flags = val;
	if (!of_property_read_u32(dev->of_node, "dsi,format", &val))
		dsi->format = val;
	if (!of_property_read_u32(dev->of_node, "dsi,lanes", &val))
		dsi->lanes = val;
	
	/* 4. Init/Exit Sequences */
	data = of_get_property(dev->of_node, "panel-init-sequence", &len);
	if (data) {
		panel->init_seq = devm_kzalloc(dev, sizeof(*panel->init_seq), GFP_KERNEL);
		if (!panel->init_seq) return -ENOMEM;
		err = panel_parse_cmd_seq(dev, data, len, panel->init_seq);
		if (err) return dev_err_probe(dev, err, "failed to parse init seq\n");
	}

	data = of_get_property(dev->of_node, "panel-exit-sequence", &len);
	if (data) {
		panel->exit_seq = devm_kzalloc(dev, sizeof(*panel->exit_seq), GFP_KERNEL);
		if (!panel->exit_seq) return -ENOMEM;
		err = panel_parse_cmd_seq(dev, data, len, panel->exit_seq);
		if (err) return dev_err_probe(dev, err, "failed to parse exit seq\n");
	}

	/* 5. Timings */
	panel->timing = devm_kzalloc(dev, sizeof(*panel->timing), GFP_KERNEL);
	if (!panel->timing) return -ENOMEM;
	err = of_get_display_timing(dev->of_node, "dsi1_timing0", panel->timing); // Search for label or default
	if (err) {
		// Try generic node name
		err = of_get_display_timing(dev->of_node, "panel-timing", panel->timing);
		if (err) return dev_err_probe(dev, err, "failed to get timing\n");
	}
	
	/* 6. Register Panel */
	drm_panel_init(&panel->base, dev, &rockchip_dsi_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	
	err = drm_panel_of_backlight(&panel->base);
	if (err)
		return dev_err_probe(dev, err, "failed to find backlight\n");

	drm_panel_add(&panel->base);

	err = mipi_dsi_attach(dsi);
	if (err) {
		drm_panel_remove(&panel->base);
		return dev_err_probe(dev, err, "failed to attach dsi\n");
	}

	return 0;
}

static void rockchip_dsi_panel_remove(struct mipi_dsi_device *dsi)
{
	struct rockchip_dsi_panel *panel = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&panel->base);
}

static const struct of_device_id rockchip_dsi_panel_of_match[] = {
	{ .compatible = "simple-panel-dsi", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rockchip_dsi_panel_of_match);

static struct mipi_dsi_driver rockchip_dsi_panel_driver = {
	.driver = {
		.name = "panel-rockchip-dsi-generic",
		.of_match_table = rockchip_dsi_panel_of_match,
	},
	.probe = rockchip_dsi_panel_probe,
	.remove = rockchip_dsi_panel_remove,
};
module_mipi_dsi_driver(rockchip_dsi_panel_driver);

MODULE_AUTHOR("Generic Rockchip DSI Panel Adapter");
MODULE_DESCRIPTION("DRM Driver for Simple DSI Panels with DT Init Seq");
MODULE_LICENSE("GPL");
