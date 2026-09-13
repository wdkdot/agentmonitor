# AI Usage Monitor firmware

ESP-IDF + LVGL firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.64 V2.

The hardware path is derived from the validated `esp32-s3-1-64-amoled-x20`
graphics demo: SH8601 QSPI AMOLED, FT3168 touch, GPIO46 LCD CS, DMA-backed
double buffering, DCS brightness control, and a 456x280 landscape UI.

The firmware deliberately excludes Wi-Fi, Bluetooth, account credentials, and
provider APIs. It accepts normalized usage snapshots from the desktop host over
a 64-byte vendor-defined USB HID output report.

The four swipe pages are the overview, Codex daily activity, Claude daily
activity, and a phone-style device settings list. Each setting opens a detail
screen where swipe navigation is disabled. Brightness, auto-dim enable state
and timeout, and whether an idle display returns home are stored in NVS.
Auto-dim defaults to off; when enabled it lowers the AMOLED to 5% and restores
the configured brightness on the next touch. Its timeout can be set to 5 or 30
minutes, or 1 or 3 hours.

## Build

Use ESP-IDF 5.5.2:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

Managed dependencies are declared in `main/idf_component.yml`.

## USB identity

During development, the firmware uses Espressif's VID and TinyUSB's default
HID-only PID. The product string is `AI Usage Monitor`. Production hardware
must receive an assigned VID/PID before distribution.

## Protocol

The packet contract is documented in `../ai-usage-display-host/src/protocol.rs`.
Protocol v1 uses bytes 6 and 7 for detailed Codex and Claude status codes. Bytes
42 through 56 carry optional Monday-to-Sunday activity bars; older v1 senders
remain compatible and simply show an unavailable graph.
