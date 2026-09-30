# Mosaico camera

`mosaico_module_camera` manages OV3640 and SC101IOT DVP camera modules. It
discovers and claims the left slot, registers `/dev/video2`, and leaves device
opening and streaming under explicit application control:

```c
mosaico_camera_handle_t camera;
ESP_ERROR_CHECK(mosaico_camera_new(NULL, &camera));
ESP_ERROR_CHECK(mosaico_camera_open(camera));
ESP_ERROR_CHECK(mosaico_camera_start_stream(camera));

mosaico_camera_frame_t frame;
ESP_ERROR_CHECK(mosaico_camera_get_frame(camera, &frame));
/* Consume frame.data before returning it. */
ESP_ERROR_CHECK(mosaico_camera_return_frame(camera, &frame));

ESP_ERROR_CHECK(mosaico_camera_stop_stream(camera));
ESP_ERROR_CHECK(mosaico_camera_close(camera));
ESP_ERROR_CHECK(mosaico_camera_del(camera));
```

By default, the camera uses the sensor's Kconfig-selected resolution in UYVY
format with two buffers. A frame remains owned by the caller until
`mosaico_camera_return_frame()` is called. Return all frames before calling
`mosaico_camera_stop_stream()`, `mosaico_camera_close()`, or
`mosaico_camera_restart()`, or `mosaico_camera_del()`. Closing capture keeps `/dev/video2` registered so
another consumer can open it.

## Frame ownership and shutdown

The caller owns a frame loan, while the component owns its storage. Finish all
CPU and DMA consumers, including PPA operations, before returning a frame; after
returning it the capture engine may immediately overwrite it. A DMA completion
timeout does not cancel that consumer and is not permission to return the frame.

Stop, close, restart and delete reject outstanding frame loans with
`ESP_ERR_INVALID_STATE`. They do not wait for another task to return them.
Stopping an already stopped stream succeeds. After stop, use `start_stream()`;
after close, use `open()` then `start_stream()`. A successful delete invalidates
the handle and releases the module lease so another instance can claim it.
If delete fails, retain the handle and resolve the failure before retrying.

Use one capture owner or explicitly coordinate tasks around these lifecycle
operations. The internal mutex serializes API calls, but cannot protect a loan
while application code or another peripheral is using it. In particular, stop
concurrent API users before deleting the handle.

The component deliberately leaves system restart coordination to the application,
which knows its downstream consumers. Before `esp_restart()`, request the capture
owner to stop at a frame boundary, finish downstream DMA, return outstanding
frames, and stop/delete the camera. The
[camera LCD preview example](../../examples/camera_lcd_preview/README.md)
demonstrates this sequence with PPA and LCD completion callbacks. CPU software
reset must not be assumed to stop every peripheral DMA engine.

## Lifecycle regression tests

The Unity test component in [`test/`](test/) uses the real camera API and requires
an ESP-Mosaico with a supported camera in the LEFT slot, PSRAM, the camera/video
Kconfig settings from the preview example, and no other camera consumer. It enables
`allow_unidentified` for bring-up modules. Include it in an ESP-IDF Unity test
application and run the `[mosaico_camera][hardware]` cases:

- Stop twice, resume, close/reopen, and verify new frames after each transition.
- Delete and recreate the camera, verifying the module lease can be reclaimed.
- Keep a frame borrowed, verify stop/close/restart/delete reject it without changing
  the loan, then return it and verify capture resumes.

These are hardware tests; compiling them alone does not establish passing behavior.

JPEG camera frames can be decoded to a reusable RGB888 buffer with the ESP32-S31
hardware JPEG engine through `mosaico_camera_jpeg_decoder_new()` and
`mosaico_camera_jpeg_decode_rgb888()`. AI pipelines should use
`mosaico_camera_jpeg_decode_rgb888_ccw90()` so the PPA rotates the decoded frame
counter-clockwise by 90 degrees into the model-upright orientation before
inference.

OV3640-only tuning and flash exposure register operations are skipped for
SC101IOT. The current camera wiring supports the left slot only.

## ESP-IDF compatibility

During CMake configuration, this component attempts to apply its bundled DVP
frame-capture stability patch to `$IDF_PATH`. The operation is idempotent: an
already-applied patch is left unchanged. Git, a writable ESP-IDF checkout, and
an applicable IDF source revision are required. If the patch cannot be applied,
CMake reports a warning and continues the build; in that case the SC101IOT
timing and frame-tail workarounds are not active.
