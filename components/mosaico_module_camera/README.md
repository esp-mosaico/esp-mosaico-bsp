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

The default is 1024x768 UYVY with four buffers. A frame remains owned by the
caller until `mosaico_camera_return_frame()` is called. Return all frames before
calling `mosaico_camera_stop_stream()`, `mosaico_camera_close()`, or
`mosaico_camera_restart()`. Closing capture keeps `/dev/video2` registered so
another consumer can open it.

JPEG camera frames can be decoded to a reusable RGB888 buffer with the ESP32-S31
hardware JPEG engine through `mosaico_camera_jpeg_decoder_new()` and
`mosaico_camera_jpeg_decode_rgb888()`. AI pipelines should use
`mosaico_camera_jpeg_decode_rgb888_ccw90()` so the PPA rotates the decoded frame
counter-clockwise by 90 degrees into the model-upright orientation before
inference.

OV3640-only tuning and flash exposure register operations are skipped for
SC101IOT. The current camera wiring supports the left slot only.
