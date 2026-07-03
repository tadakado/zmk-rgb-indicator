// SPDX-License-Identifier: MIT
// LED strip proxy that overlays a connection-status indicator on one pixel.
// ZMK underglow writes here (chosen zmk,underglow); on an endpoint we override
// pixels[index] with a color for the current USB/BLE endpoint, then forward the
// whole frame to the real strip. A split peripheral just forwards untouched.

#define DT_DRV_COMPAT zmk_led_strip_indicator

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

// Endpoint/profile status only exists on an endpoint (central or non-split).
#if !defined(CONFIG_ZMK_SPLIT) || defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define INDICATOR_ENABLED 1
#else
#define INDICATOR_ENABLED 0
#endif

#if INDICATOR_ENABLED
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#endif
#endif

LOG_MODULE_REGISTER(rgb_indicator, CONFIG_LED_STRIP_LOG_LEVEL);

struct ind_config {
    const struct device *target;
    uint16_t length;
    uint8_t index;
    uint32_t usb_color;               // 0xRRGGBB
    const uint32_t *profile_colors;   // 0xRRGGBB per BLE profile
    uint8_t num_profile_colors;
};

#if INDICATOR_ENABLED
static inline struct led_rgb rgb_from_u32(uint32_t v) {
    return (struct led_rgb){.r = (v >> 16) & 0xff, .g = (v >> 8) & 0xff, .b = v & 0xff};
}

// Color for the current endpoint, from the devicetree usb-color / profile-colors.
static struct led_rgb indicator_color(const struct ind_config *cfg) {
    if (zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB) {
        return rgb_from_u32(cfg->usb_color);
    }
#if IS_ENABLED(CONFIG_ZMK_BLE)
    int p = zmk_ble_active_profile_index();
    if (p >= 0 && p < cfg->num_profile_colors) {
        return rgb_from_u32(cfg->profile_colors[p]);
    }
#endif
    return (struct led_rgb){0}; // off
}
#endif

static int ind_update_rgb(const struct device *dev, struct led_rgb *pixels, size_t num) {
    const struct ind_config *cfg = dev->config;
#if INDICATOR_ENABLED
    if (cfg->index < num) {
        pixels[cfg->index] = indicator_color(cfg);
    }
#endif
    return led_strip_update_rgb(cfg->target, pixels, num);
}

static size_t ind_length(const struct device *dev) {
    return ((const struct ind_config *)dev->config)->length;
}

static int ind_init(const struct device *dev) {
    const struct ind_config *cfg = dev->config;
    if (!device_is_ready(cfg->target)) {
        LOG_ERR("target LED strip not ready");
        return -ENODEV;
    }
#if INDICATOR_ENABLED
    if (cfg->index >= cfg->length) {
        LOG_WRN("index %u >= chain-length %u; indicator will not show",
                (unsigned)cfg->index, (unsigned)cfg->length);
    }
#endif
    return 0;
}

static const struct led_strip_driver_api ind_api = {
    .update_rgb = ind_update_rgb,
    .length = ind_length,
};

#define IND_DEFINE(n)                                                            \
    static const uint32_t profile_colors_##n[] = DT_INST_PROP(n, profile_colors); \
    static const struct ind_config ind_config_##n = {                            \
        .target = DEVICE_DT_GET(DT_INST_PHANDLE(n, target)),                     \
        .length = DT_INST_PROP(n, chain_length),                                 \
        .index  = DT_INST_PROP_OR(n, index, 0),                                  \
        .usb_color = DT_INST_PROP(n, usb_color),                                 \
        .profile_colors = profile_colors_##n,                                    \
        .num_profile_colors = DT_INST_PROP_LEN(n, profile_colors),               \
    };                                                                           \
    DEVICE_DT_INST_DEFINE(n, ind_init, NULL, NULL, &ind_config_##n,              \
                          POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY, &ind_api);

DT_INST_FOREACH_STATUS_OKAY(IND_DEFINE)
