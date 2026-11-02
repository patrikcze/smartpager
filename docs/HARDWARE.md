# Hardware findings

Everything below was gathered on 2026-11-02 with read-only tools. The board was **not** flashed or erased during discovery, and its flash contents were not read out.

## What was detected

| Probe | Command | Result |
|---|---|---|
| Host | `sw_vers`, `uname -m` | macOS 27.2, arm64 |
| Serial port | `ls /dev/cu.*` | `/dev/cu.usbmodem1101` |
| USB descriptor | `ioreg -p IOUSB -l` | Vendor "Espressif", product "USB JTAG_serial debug unit", VID `0x303A`, PID `0x1001`, serial = the Wi-Fi MAC address (masked here: `DC:DA:0C:xx:xx:xx`) |
| Chip | `esptool --port /dev/cu.usbmodem1101 flash-id` (esptool 5.6.0) | **ESP32-S3 (QFN56) rev v0.2**, Wi-Fi + BLE 5, dual core 240 MHz, **embedded PSRAM 8 MB (AP_3v3)**, 40 MHz crystal, USB mode USB-Serial/JTAG |
| Flash | same | Manufacturer `0xEF` (Winbond), device `0x4018`, **16 MB**, quad mode, 3.3 V |
| Running firmware | passive serial capture after reset (115200 baud) | ROM boot banner, then `T-Display S3 Clock` and `Connecting to WiFi....` |

`flash-id` resets the chip into the ROM bootloader, reads the IDs and resets it again. It does not write to flash.

### USB interface

The board uses the ESP32-S3's **native USB-Serial/JTAG** controller (PID `0x1001`). There is no CH340 or CP210x bridge. Consequences:

- Flashing works over the same port without pressing buttons (esptool resets through the USB-JTAG controller).
- The serial console only exists while the firmware is running. If the firmware crashes early, hold **BOOT**, tap **RST**, then release BOOT to force the ROM bootloader (see [FLASHING.md](FLASHING.md)).
- The firmware is built with `ARDUINO_USB_CDC_ON_BOOT=1` and `ARDUINO_USB_MODE=1`, as in LILYGO's own configuration.

## Board assessment

**Model: LILYGO T-Display-S3 (ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM, 1.9" 170×320 ST7789V). Confidence: high.**

- ESP32-S3R8 with 16 MB flash and 8 MB PSRAM matches exactly the two products LILYGO lists in its repository (T-Display-S3 and T-Display-S3 Touch).
- The `T-Display S3 Clock` banner and the USB descriptor match a T-Display-S3 sketch. No published LILYGO example prints this exact string, so the clock is probably a community sketch, not the factory demo.
- The AMOLED variants (T-Display-S3 AMOLED) use a different SoC package and display (RM67162 over QSPI), and the long variant uses a different panel. Neither matches the PSRAM/flash combination together with a clock running on an LCD.

**Touch controller: confirmed CST816 at I²C `0x15`** (diag image on the device, 2026-11-02). This unit is the **Touch edition**.

**Verified with the `diag` image on the device:** chip, 16 MB flash, PSRAM 8189 KB usable (only after adding `-DBOARD_HAS_PSRAM`; PlatformIO's built-in board file omits it, and the first diag build reported 0 KB), CST816 present.

The CST816 sleeps between touches and NACKs polled reads. The driver therefore reads it only after a falling edge on INT (GPIO16), and keeps reading while a finger is reported, like LILYGO's `TOUCH_READ_FROM_INTERRUPT` mode.

Original pre-flash assessment, kept for reference: The Touch edition uses a CST816 (I²C `0x15`, current units) or a CST328 (`0x1A`, early units) on SDA 18 / SCL 17. This can't be checked from the host without running code on the board. Both the SmartPager firmware and the `diag` image probe both addresses at boot:

- **CST816 found:** touch is enabled (tap rows to open, swipe right to go back).
- **CST328 found:** this is logged, but the firmware does not support it yet. You get buttons only.
- **Nothing found:** buttons only. Every function is reachable with the two buttons.

## Pin map used

From `examples/factory/pin_config.h` in [Xinyuan-LilyGO/T-Display-S3](https://github.com/Xinyuan-LilyGO/T-Display-S3), mirrored in [`firmware/src/hardware/board.h`](../firmware/src/hardware/board.h):

| Function | GPIO |
|---|---|
| LCD data D0–D7 (8-bit i80) | 39, 40, 41, 42, 45, 46, 47, 48 |
| LCD WR / RD / DC / CS / RST | 8 / 9 / 7 / 6 / 5 |
| LCD backlight (16-step pulse-dimmed driver) | 38 |
| LCD power enable (needed on battery) | 15 |
| Button BOOT (also the ROM download strap) | 0 |
| Button KEY | 14 |
| Battery voltage (÷2 divider) | 4 |
| I²C SDA / SCL (touch) | 18 / 17 |
| Touch INT / RST | 16 / 21 |

The third button is **RST** (chip reset). Firmware can't read it.

## Display

- ST7789V, native 170×320 portrait, connected to the ESP32-S3 LCD peripheral as an 8-bit Intel-8080 bus at **16 MHz**. LILYGO warns that faster clocks cause "mosaic" artefacts.
- Initialization is the same `esp_lcd` path and vendor command table (`LCD_MODULE_CMD_1`) as the factory firmware: `invert_color(true)`, `swap_xy(true)`, `mirror(false, true)`, `set_gap(0, 35)`.
- **Chosen orientation: landscape, 320×170, USB connector on the left** (the factory default). On a 1.9" strip, landscape fits a pager best: a large clock and status on the left, notification cards on the right, and lists with 3 visible rows. A "Rotate 180°" option in setup flips it for USB-on-the-right mounting.
- RGB565 with byte swap (`LV_COLOR_16_SWAP 1`), as in LILYGO's `lv_conf.h`.
- LVGL draws into two partial buffers of 320×34 px in internal DMA RAM (2 × 21.8 KB). There is no full framebuffer, and only changed areas are redrawn.
- **Backlight:** GPIO38 drives a pulse-counting LED driver with 16 levels, not a PWM input. The firmware reuses LILYGO's `setBrightness()` pulse algorithm.

## Toolchain decision

| | Choice | Why |
|---|---|---|
| Build system | PlatformIO Core 6.2.0 (installed via `uv tool install platformio`) | Matches the official repo, gives reproducible pinned builds, and can run native unit tests |
| Platform | `espressif32@6.5.0` → arduino-esp32 **2.0.14** / ESP-IDF 4.4 | The version LILYGO pins and tests. Their README warns about TFT_eSPI on newer cores. We don't use TFT_eSPI, but staying on the tested core keeps `esp_lcd` behaviour identical |
| Board | `lilygo-t-display-s3` (PlatformIO built-in) | qio_opi memory type, 16 MB, PSRAM enabled |
| Display driver | ESP-IDF `esp_lcd` i80 + ST7789 (built into the core) | Same driver as the factory firmware. No extra library |
| GUI | LVGL **8.3.11** | Same major version as LILYGO's examples |
| JSON | ArduinoJson **7.2.1** | Filtered, bounded parsing |
| Touch | Small built-in CST816 reader (I²C register map taken from LILYGO's TouchLib) | Avoids pulling in TouchLib for a 7-byte read |

## Flashing safety assessment

- Flashing is a normal esptool write over USB-Serial/JTAG. The ROM bootloader can't be overwritten, so an image that crashes can always be replaced by entering download mode manually (BOOT + RST).
- `pio run -t upload` writes the bootloader, the partition table (`default_16MB.csv`: two 6.4 MB app slots plus a 3.4 MB data partition) and the app. It does **not** erase NVS or the data partition. On first boot, SmartPager formats the data partition as LittleFS if it doesn't mount, which erases whatever the clock sketch stored there.
- The current clock firmware can't be restored unless its source or binary is available. Back up the flash first if you want to keep it ([FLASHING.md](FLASHING.md#optional-back-up-the-current-firmware)). That backup reads all of flash, including any Wi-Fi credentials the clock sketch stored, so treat the file as sensitive.
- No eFuses are burned, and flash encryption and secure boot are **not** enabled. Both are irreversible and out of scope (see [SECURITY.md](SECURITY.md)).
