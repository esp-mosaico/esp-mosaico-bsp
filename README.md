# ESP32-S31 Mosaico BSP

## USB-OTG console and download

Applications must use this BSP component to enable automatic download through
the board USB-OTG port. The feature is enabled by default and requires no USB
initialization call in `app_main()`.

The BSP provides:

- A USB CDC console for application logs from `app_main()` onward.
- Standard esptool DTR/RTS entry into ROM download mode.
- Automatic reset and USB re-enumeration after downloading.
- A per-chip USB serial number so multiple connected boards remain distinct.

ESP32-S31 rev0 re-enumerates from the application CDC device to the ROM
downloader. The BSP includes an ESP-IDF command extension that reopens the
matching board after this transition. Run `idf.py build` once after adding the
BSP so ESP-IDF discovers the extension, then use:

```bash
idf.py -p USB_PORT mosaico-flash --monitor
```

The command builds incrementally, enters ROM when needed, flashes with the
active ESP-IDF environment, waits for the application USB port, and optionally
starts IDF Monitor. Global `-b` and `-B` options remain available. It does not
modify ESP-IDF or esptool. Automatic board tracking supports Linux USB CDC
ports (`/dev/ttyACM*`) and macOS callout devices (`/dev/cu.usbmodem*`). Pass
`/dev/tty.usbmodem*` on macOS if needed; the extension opens the matching
`cu.` device. Windows COM ports are not tracked yet.

When the board is already in ROM download mode, one flash command is enough:

```bash
idf.py build
idf.py -p PORT flash
```

USB devices temporarily disconnect while the chip resets. IDF Monitor
reconnects automatically, so seeing one disconnect message before logs resume
is expected.

The standard command remains available. When starting from the application
USB port, its first attempt may only enter ROM; retrying the same command then
flashes the ROM port:

```bash
idf.py -p USB_PORT flash || idf.py -p USB_PORT flash
```

The related options are `CONFIG_BSP_USB_CONSOLE`,
`CONFIG_BSP_USB_CONSOLE_AUTO_INIT`, `CONFIG_BSP_USB_AUTO_DOWNLOAD`, and
`CONFIG_BSP_USB_CONSOLE_HOST_WAIT_MS`. The host wait defaults to 1500 ms to
preserve early application logs during monitor reconnection. Set it to `0` for
applications that must start immediately without a USB host.

## Subboard hardware resources

`bsp/subboard.h` describes the two connector slots and arbitrates board-level
resources such as shared I2C address pins, DVP pins, power rails, and the USB
Serial/JTAG pin conflict. Applications should normally use a concrete
subboard component such as `mosaico_camera` instead of configuring these
resources directly.
