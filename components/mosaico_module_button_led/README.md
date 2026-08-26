# Mosaico Button LED subboard

`mosaico_button_led` discovers and claims a Button LED subboard, reads its two
keys, and controls its three WS2812 LEDs. Pass `NULL` to use automatic slot
discovery and the default brightness.

```c
mosaico_button_led_handle_t board = NULL;
ESP_ERROR_CHECK(mosaico_button_led_new(NULL, &board));

mosaico_button_led_color_t green = {.r = 0, .g = 32, .b = 0};
ESP_ERROR_CHECK(mosaico_button_led_set_all(board, green));

bool key1 = false;
bool key2 = false;
ESP_ERROR_CHECK(mosaico_button_led_read_keys(board, &key1, &key2));

ESP_ERROR_CHECK(mosaico_button_led_del(board));
```

The handle owns the claimed subboard slot until `mosaico_button_led_del()` is
called. Use `examples/button_led_test` for the complete application flow.
