# zmk-rgb-indicator

A [ZMK](https://zmk.dev) module that shows the active output (USB / BLE profile)
as a **steady color on one pixel of your existing WS2812 underglow strip** —
while the rest of the strip keeps running your normal underglow animation.

It works as a `led_strip` **proxy**: ZMK underglow writes to this node, it
overrides one pixel with the connection color, then forwards the whole frame to
the real strip. So you don't need a separate status LED — one of your underglow
LEDs doubles as the indicator.

Default colors (all configurable in devicetree — see below):

| Output | Color |
|---|---|
| USB | red |
| BLE profile 0 | green |
| BLE profile 1 | blue |
| BLE profile 2 | yellow |
| BLE profile 3 | cyan |
| BLE profile 4 | magenta |

On a split keyboard the indicator is driven by the central; a peripheral just
forwards the strip untouched.

## How it differs from the status-LED widgets

If you want a **dedicated** status LED with battery + blink patterns, use one of
the excellent existing widgets instead:
[zmk-rgbled-widget](https://github.com/caksoylar/zmk-rgbled-widget) (3-GPIO RGB
LED) or [zmk-smart-rgbled-widget](https://github.com/awkannan/zmk-smart-rgbled-widget)
(WS2812).

This module is for the narrower case where you already have an **addressable
underglow strip** and just want to tint **one pixel** of it with a persistent
per-endpoint color, leaving the rest as normal animated underglow.

## Installation

### 1. Add the module to `config/west.yml`

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: tadakado
      url-base: https://github.com/tadakado
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-rgb-indicator
      remote: tadakado
      revision: main
  self:
    path: config
```

### 2. Insert the proxy in front of your strip

In your board/shield devicetree, define the proxy pointing at your real strip,
and point `chosen { zmk,underglow }` at the proxy instead of the strip:

```dts
/ {
    rgb_indicator: rgb_indicator {
        compatible = "zmk,led-strip-indicator";
        target = <&led_strip>;   /* your real WS2812 device */
        chain-length = <7>;      /* must match the strip */
        index = <0>;             /* which pixel is the indicator */

        /* optional: override the colors (0xRRGGBB) */
        /* usb-color = <0xff0000>; */
        /* profile-colors = <0x00ff00 0x0000ff 0xffff00 0x00ffff 0xff00ff>; */
    };

    chosen {
        zmk,underglow = &rgb_indicator;   /* was &led_strip */
    };
};
```

Enable underglow as usual (`CONFIG_ZMK_RGB_UNDERGLOW=y`). The module builds only
when a `zmk,led-strip-indicator` node is present, so it's safe to add the west
project unconditionally.

## Devicetree properties

| Property | Required | Default | Description |
|---|---|---|---|
| `target` | yes | — | phandle of the real LED strip to forward to. |
| `chain-length` | yes | — | number of pixels (match the target strip). |
| `index` | no | `0` | which pixel (0-based) shows the indicator. |
| `usb-color` | no | `0xff0000` | color (`0xRRGGBB`) when USB is the active endpoint. |
| `profile-colors` | no | `0x00ff00 0x0000ff 0xffff00 0x00ffff 0xff00ff` | colors (`0xRRGGBB`) per BLE profile; a profile past the end of the list shows off. |

## Notes

- The indicator overrides its pixel on **every** underglow frame, so it stays put
  regardless of the active underglow effect.
- `profile-colors` may hold as many entries as you have BLE profiles; extras are
  ignored and missing ones show off.

## Glitch recovery

WS2812 pixels hold their color until new data arrives, so a strip glitched by
data-line noise or a supply dip (seen on a split central when the other half
reconnects and the radio gets busy) would stay wrong while underglow is off.
The module therefore re-sends the cached frame shortly after every BLE
connect / disconnect and every `CONFIG_ZMK_LED_STRIP_INDICATOR_REFRESH_MS`
(default 2000 ms; 0 disables the periodic refresh).
`CONFIG_ZMK_LED_STRIP_INDICATOR_TRACE=y` logs every frame sent (debug).

## License

MIT — see [LICENSE](LICENSE).
