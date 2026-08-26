# Mosaico joystick

Application-facing driver for the ESP-Mosaico Joystick Board.

The component discovers an EEPROM programmed as `MOSAICO_BOARD_TYPE_HANDLE`,
claims its left or right slot, shares ADC oneshot units across simultaneous
handles, and reads two axes plus buttons B/1/2/3/4. Calibration is non-blocking:
each periodic `mosaico_joystick_read()` call samples the hardware, advances
calibration, and returns the latest snapshot.

```c
mosaico_joystick_handle_t joystick;
mosaico_joystick_config_t config = MOSAICO_JOYSTICK_DEFAULT_CONFIG();
config.slot = MOSAICO_MODULE_MGR_SLOT_AUTO;
ESP_ERROR_CHECK(mosaico_joystick_new(&config, &joystick));

while (true) {
    mosaico_joystick_data_t data;
    esp_err_t ret = mosaico_joystick_read(joystick, &data);
    if (ret == ESP_ERR_NOT_FOUND) {
        break;
    }
    ESP_ERROR_CHECK(ret);
    vTaskDelay(pdMS_TO_TICKS(40));
}

ESP_ERROR_CHECK(mosaico_joystick_del(joystick));
```
