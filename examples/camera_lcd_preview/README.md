# Camera LCD Preview

This example discovers and claims the Mosaico Camera subboard, captures UYVY
frames from a supported sensor, converts them to RGB565 with the PPA, and displays a live
480 x 480 preview on the onboard CO5300 LCD.

The example intentionally uses the high-level `mosaico_camera` API. Application
code does not need to initialize SCCB, XCLK, DVP, or the subboard manager
separately.

## Hardware

- ESP-Mosaico
- Camera subboard installed in the **LEFT** slot and aligned with the board
  silkscreen
- UART console; USB Serial/JTAG is disabled because its D- pad
  shares GPIO33 with camera D2

The example sets `allow_unidentified` so the sensor comes up even when the
subboard EEPROM carries no valid descriptor, which is the usual state on
bring-up hardware. Production code should keep it off and rely on the
descriptor.

## Build and run

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

After the camera is detected, the LCD continuously shows a centered square crop
of the camera image. The crop fits PPA's 1/16 scaling increments exactly: SC101IOT
1280 x 720 uses a 640 x 640 crop at 3/4 scale; OV3640 1024 x 768 uses a
768 x 768 crop at 5/8 scale. This fills the 480 x 480 output without unpainted borders.
If capture times out three times consecutively, the example
first restarts the stream. If frames still do not arrive, it releases the Camera
subboard and returns to automatic discovery so unplug/replug can recover.

## Software restart and DMA ownership

`app_main` owns the camera, PPA and LCD transactions. PPA runs asynchronously,
with a completion semaphore; LCD uses its transfer completion callback. The
camera frame is returned only after PPA has finished reading it, and the LCD
buffer is reused only after its previous transfer completes.

A shutdown handler requests the capture owner to exit at a frame boundary and
waits for acknowledgement. The owner finishes PPA/LCD work, returns the frame,
deletes the camera (stopping DVP capture), and unregisters PPA before acknowledging.
This prevents active capture DMA from being carried across a CPU software reset.
The context and callback semaphores remain valid until reboot.

An integrating application should call `esp_restart()` from its control task,
not from the capture owner, since the shutdown handler waits for that owner.
The example uses `ESP_SHUTDOWN_HANDLER_REGISTER` from the supported ESP-IDF 6.2
checkout. Shutdown waits up to 15 seconds. PPA/LCD waits log their stage after
each second without completion, and first-frame logs distinguish capture,
conversion and display. A timeout is diagnostic: it does not cancel DMA or make
its buffers safe to free, and a shutdown-handler error does not veto `esp_restart()`.

The main task has a 20 KiB stack for camera initialization and the PPA configuration.
Initialization failures release application allocations before the shutdown context
is published; the display panel/IO remain BSP-owned singletons.

## Validation

The crop arithmetic can be checked on the host, from this example directory:

```sh
cc -std=c11 -Wall -Wextra -Werror -I main tests/preview_geometry_test.c -o /tmp/preview_geometry_test
/tmp/preview_geometry_test
```

The camera component's [`test/`](../../components/mosaico_module_camera/test/)
contains hardware lifecycle regressions. For the complete preview pipeline, verify
repeated software restarts while streaming, a fresh first-frame log and increasing
frame counts after every boot, and a correctly filled LCD. Host geometry tests and
a successful build do not replace this hardware validation.

When using this example's code in ESP-Mosaico's retained-firmware workspace,
create/install an application with that workspace's `mosaico.py` workflow and
preserve its Vibe Mode integration. This standalone BSP example does not itself
provide that integration or the product's Iris screenshot/health services.
