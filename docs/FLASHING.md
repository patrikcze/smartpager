# Flashing

Prerequisites: PlatformIO Core (`uv tool install platformio`, or the VS Code extension) and the board connected over USB-C. On this machine the port is `/dev/cu.usbmodem1101`. Check yours with `pio device list`.

```bash
cd firmware
```

## Optional: back up the current firmware

Reads all 16 MB of flash into a file. Do this if you want to be able to restore the clock sketch. The image also holds the clock's stored settings (for example its Wi-Fi password), so keep it private.

```bash
esptool --port /dev/cu.usbmodem1101 read-flash 0 0x1000000 tdisplay-s3-backup.bin
```

To restore it later:

```bash
esptool --port /dev/cu.usbmodem1101 write-flash 0 tdisplay-s3-backup.bin
```

## Step 1: hardware verification image (recommended first flash)

`diag` uses no Wi-Fi and stores nothing. It shows colour bars (red, green, blue, white from left to right), orientation markers, chip/flash/PSRAM sizes, an I²C scan (touch controller), Czech text, and live button and touch readouts.

```bash
pio run -e diag -t upload
```

```bash
pio device monitor
```

Check:
- the bars are red, green, blue, white, and "TOP-LEFT" is in the top-left corner with the USB connector on your left
- the I²C line shows `0x15 (CST816 touch OK)` on a Touch edition
- both buttons report short and long presses

## Step 2: SmartPager

```bash
pio run -e tdisplays3 -t upload
```

```bash
pio device monitor
```

On first boot the device has no configuration and starts **setup mode** (see [GUIDE.md §6.3](GUIDE.md#63-enter-the-settings)).

## Later updates: over the air

Once a pager runs firmware built with your OTA key (`make ota-keygen`, then `make fw-flash`), new releases don't need USB:
1. Bump `SMARTPAGER_VERSION` in `firmware/src/app_state.h`.
2. Run `make ota-publish`.
3. On the pager: Settings → Update → hold KEY.

See [GUIDE.md](GUIDE.md#9-firmware-updates).

## Recovering a board that does not enumerate

If a crash loop keeps the USB port from appearing: hold **BOOT**, tap **RST**, release **BOOT**. The ROM bootloader then enumerates and you can upload again. Press RST afterwards to run the new firmware.

## Erase only SmartPager's settings

Use **Settings → Setup mode → Factory reset settings** in the portal. This clears the `smartpager` NVS namespace and the message cache, and leaves the firmware in place. `esptool erase-flash` is never needed for this.
