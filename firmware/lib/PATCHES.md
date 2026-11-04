# Patched Adafruit TinyUSB (vendored)

`Adafruit_TinyUSB_Arduino/` is a copy of [Adafruit_TinyUSB_Arduino](https://github.com/adafruit/Adafruit_TinyUSB_Arduino)
version 1.7.0 (as bundled with the Seeeduino nRF52 core 1.1.13; MIT, see its LICENSE; the TinyUSB stack inside is
[hathach/tinyusb](https://github.com/hathach/tinyusb)), with three small changes. Patched spots are marked `PATCH btbridge`. Build with `--library lib/Adafruit_TinyUSB_Arduino` so it overrides the core copy.

## Why it had to be changed

btbridge needs three USB HID interfaces (boot keyboard, boot mouse, consumer/media keys). The stock library cannot do that:

1. **Only one HID interface is compiled in.** `CFG_TUD_HID` is 1, so TinyUSB's HID class driver has a single instance.
2. **The Arduino HID wrapper is single-instance.** `Adafruit_USBD_HID` keeps one global pointer (`_hid_dev`, marked
   "TODO multiple instances" upstream). Consequences with two or more objects:
   * `ready()` / `sendReport()` always talk to HID instance 0, so mouse reports were sent on the keyboard endpoint
     (symptom: the status LED stayed red, "report pending but USB not ready").
   * `tud_hid_descriptor_report_cb()` returned the last registered descriptor for every interface, so the keyboard
     interface was announced with the mouse report descriptor.

## Files to modify (if you re-do the patch on a newer library)

| File | Change |
|---|---|
| `src/arduino/ports/nrf/tusb_config_nrf.h` | `#define CFG_TUD_HID 1` -> `3` |
| `src/arduino/hid/Adafruit_USBD_HID.h` | add private member `uint8_t _idx = 0;` (HID instance index) |
| `src/arduino/hid/Adafruit_USBD_HID.cpp` | replace the single `_hid_dev` pointer with `static Adafruit_USBD_HID *_hid_devs[3]` + a counter. In `begin()`: `_idx = count; _hid_devs[count++] = this;`. `ready()` -> `tud_hid_n_ready(_idx)`. `sendReport()` -> `tud_hid_n_report(_idx, id, buf, len)`. In `tud_hid_descriptor_report_cb`, `tud_hid_get_report_cb`, `tud_hid_set_report_cb` use `_hid_devs[itf]` (itf is the HID instance number, 0..2) instead of `_hid_dev`. |

The instance index equals the order of `begin()` calls, which equals the order of the interfaces in the USB configuration
(keyboard = 0, mouse = 1, consumer = 2). Keep that order in the sketch.

## Related gotcha (not a library change)

All `begin()` calls must happen at the very start of `setup()`. The host enumerates a few milliseconds after boot;
interfaces registered after a `delay()` never show up.
