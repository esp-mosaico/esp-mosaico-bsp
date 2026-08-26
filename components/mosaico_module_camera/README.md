# Mosaico camera

`mosaico_camera` is the application-facing driver for the OV3640 DVP camera
subboard. It automatically discovers the board, claims the left slot,
configures all BSP and V4L2 resources, applies module tuning, and starts
streaming:

```c
mosaico_camera_handle_t camera;
ESP_ERROR_CHECK(mosaico_camera_new(NULL, &camera));

mosaico_camera_frame_t frame;
ESP_ERROR_CHECK(mosaico_camera_get_frame(camera, &frame));
/* Consume frame.data before returning it. */
ESP_ERROR_CHECK(mosaico_camera_return_frame(camera, &frame));

ESP_ERROR_CHECK(mosaico_camera_del(camera));
```

The default is 1024x768 UYVY with four buffers. A frame remains owned by the
caller until `mosaico_camera_return_frame()` is called. Return all frames before
calling `mosaico_camera_restart()`.

JPEG camera frames can be decoded to a reusable RGB888 buffer with the ESP32-S31
hardware JPEG engine through `mosaico_camera_jpeg_decoder_new()` and
`mosaico_camera_jpeg_decode_rgb888()`. AI pipelines should use
`mosaico_camera_jpeg_decode_rgb888_ccw90()` so the PPA rotates the decoded frame
counter-clockwise by 90 degrees into the model-upright orientation before
inference.

The current camera wiring supports the left slot only.
