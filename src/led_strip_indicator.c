// SPDX-License-Identifier: MIT
// LED strip proxy that overlays a connection-status indicator on one pixel.
// ZMK underglow writes here (chosen zmk,underglow); on an endpoint we override
// pixels[index] with a color for the current USB/BLE endpoint, then forward the
// whole frame to the real strip. A split peripheral just forwards untouched.
//
// The last forwarded frame is cached, and the indicator also refreshes itself on
// endpoint / BLE-profile change events, shortly after any BLE connect/disconnect,
// and periodically (CONFIG_ZMK_LED_STRIP_INDICATOR_REFRESH_MS) so a strip glitched
// by noise recovers on its own. That way the indicator pixel keeps
// tracking the active endpoint even when underglow is OFF (ZMK stops writing
// frames then, so without this the indicator would freeze on its last color).

#define DT_DRV_COMPAT zmk_led_strip_indicator

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>

// Endpoint/profile status only exists on an endpoint (central or non-split).
#if !defined(CONFIG_ZMK_SPLIT) || defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define INDICATOR_ENABLED 1
#else
#define INDICATOR_ENABLED 0
#endif

#if INDICATOR_ENABLED
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
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

// Runtime state: the last frame we forwarded, so we can re-emit it (with a fresh
// indicator pixel) when the endpoint changes while underglow isn't writing.
struct ind_data {
    struct led_rgb *cache;   // buffer of `length` pixels
    uint16_t cache_len;      // pixels currently valid in `cache`
    struct k_mutex lock;     // serialize forwards from the underglow thread vs events
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

// Overlay the indicator pixel on the cached frame and push it to the real strip.
// `why` only labels the debug trace.
static int forward_frame(const struct device *dev, const char *why) {
    const struct ind_config *cfg = dev->config;
    struct ind_data *data = dev->data;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);
#if INDICATOR_ENABLED
    if (cfg->index < data->cache_len) {
        data->cache[cfg->index] = indicator_color(cfg);
    }
#endif
    ret = led_strip_update_rgb(cfg->target, data->cache, data->cache_len);
#if IS_ENABLED(CONFIG_ZMK_LED_STRIP_INDICATOR_TRACE)
    {
        struct led_rgb px = cfg->index < data->cache_len ? data->cache[cfg->index]
                                                          : (struct led_rgb){0};
#if INDICATOR_ENABLED
        struct zmk_endpoint_instance ep = zmk_endpoint_get_selected();
        int prof = -1;
#if IS_ENABLED(CONFIG_ZMK_BLE)
        prof = zmk_ble_active_profile_index();
#endif
        LOG_INF("fwd(%s) px%u=%02x%02x%02x len=%u ep.transport=%d prof=%d ret=%d", why,
                (unsigned)cfg->index, px.r, px.g, px.b, (unsigned)data->cache_len,
                (int)ep.transport, prof, ret);
#else
        LOG_INF("fwd(%s) px%u=%02x%02x%02x len=%u ret=%d", why, (unsigned)cfg->index, px.r,
                px.g, px.b, (unsigned)data->cache_len, ret);
#endif
    }
#endif
    k_mutex_unlock(&data->lock);
    return ret;
}

static int ind_update_rgb(const struct device *dev, struct led_rgb *pixels, size_t num) {
    const struct ind_config *cfg = dev->config;
    struct ind_data *data = dev->data;
    size_t n = MIN(num, (size_t)cfg->length);

    k_mutex_lock(&data->lock, K_FOREVER);
    memcpy(data->cache, pixels, n * sizeof(struct led_rgb));
    data->cache_len = n;
    k_mutex_unlock(&data->lock);

    return forward_frame(dev, "frame");
}

static size_t ind_length(const struct device *dev) {
    return ((const struct ind_config *)dev->config)->length;
}

static int ind_init(const struct device *dev) {
    const struct ind_config *cfg = dev->config;
    struct ind_data *data = dev->data;

    k_mutex_init(&data->lock);
    // Start from an all-off frame so the indicator can show even if underglow
    // never writes (e.g. it booted off): cache is already zero-initialized,
    // just make its whole length valid.
    data->cache_len = cfg->length;

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
    static struct led_rgb pixel_cache_##n[DT_INST_PROP(n, chain_length)];        \
    static struct ind_data ind_data_##n = {                                      \
        .cache = pixel_cache_##n,                                                \
        .cache_len = DT_INST_PROP(n, chain_length),                             \
    };                                                                           \
    static const struct ind_config ind_config_##n = {                            \
        .target = DEVICE_DT_GET(DT_INST_PHANDLE(n, target)),                     \
        .length = DT_INST_PROP(n, chain_length),                                 \
        .index  = DT_INST_PROP_OR(n, index, 0),                                  \
        .usb_color = DT_INST_PROP(n, usb_color),                                 \
        .profile_colors = profile_colors_##n,                                    \
        .num_profile_colors = DT_INST_PROP_LEN(n, profile_colors),               \
    };                                                                           \
    DEVICE_DT_INST_DEFINE(n, ind_init, NULL, &ind_data_##n, &ind_config_##n,     \
                          POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY, &ind_api);

DT_INST_FOREACH_STATUS_OKAY(IND_DEFINE)

// All instances, for re-emitting their cached frames.
#define IND_DEV_ENTRY(n) DEVICE_DT_INST_GET(n),
static const struct device *const ind_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(IND_DEV_ENTRY)
};

static void forward_all(const char *why) {
    for (size_t i = 0; i < ARRAY_SIZE(ind_devices); i++) {
        if (device_is_ready(ind_devices[i])) {
            (void)forward_frame(ind_devices[i], why);
        }
    }
}

// ---- periodic / post-connection refresh -----------------------------------

static void refresh_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(refresh_work, refresh_work_cb);

static void refresh_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    forward_all("refresh");
#if CONFIG_ZMK_LED_STRIP_INDICATOR_REFRESH_MS > 0
    k_work_reschedule(&refresh_work, K_MSEC(CONFIG_ZMK_LED_STRIP_INDICATOR_REFRESH_MS));
#endif
}

#if IS_ENABLED(CONFIG_BT_CONN)
#include <zephyr/bluetooth/conn.h>

// The radio burst around a (re)connection is when the strip tends to get
// glitched; re-send once things have settled.
static void refresh_soon(void) { k_work_reschedule(&refresh_work, K_MSEC(200)); }

static void ind_bt_connected(struct bt_conn *conn, uint8_t err) {
    ARG_UNUSED(conn);
    ARG_UNUSED(err);
    refresh_soon();
}

static void ind_bt_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(conn);
    ARG_UNUSED(reason);
    refresh_soon();
}

BT_CONN_CB_DEFINE(rgb_indicator_conn_cb) = {
    .connected = ind_bt_connected,
    .disconnected = ind_bt_disconnected,
};
#endif

static int ind_refresh_start(void) {
#if CONFIG_ZMK_LED_STRIP_INDICATOR_REFRESH_MS > 0
    k_work_schedule(&refresh_work, K_MSEC(CONFIG_ZMK_LED_STRIP_INDICATOR_REFRESH_MS));
#endif
    return 0;
}
SYS_INIT(ind_refresh_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if INDICATOR_ENABLED
// Re-emit every indicator instance's cached frame so the indicator pixel tracks
// the active endpoint even while underglow is off (no frames being written).
static int indicator_endpoint_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    forward_all("event");
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rgb_indicator_endpoint, indicator_endpoint_listener);
ZMK_SUBSCRIPTION(rgb_indicator_endpoint, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(rgb_indicator_endpoint, zmk_ble_active_profile_changed);
#endif
#endif // INDICATOR_ENABLED
