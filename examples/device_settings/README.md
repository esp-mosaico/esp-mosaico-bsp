# Device settings

Product-shaped NVS settings layer for ESP-Mosaico. It demonstrates:

- one application-owned namespace;
- schema versioning and migration/repair;
- validated typed values and defaults;
- commit/error handling;
- a factory reset that erases only the application settings namespace.

The example intentionally does not store Wi-Fi passwords, keys, or tokens. Apply the product security policy before adding secrets.

## Build

```bash
idf.py set-target esp32s31
idf.py build flash monitor
```

On first boot the default schema is written. Later boots load it without rewriting flash. Integrate `app_settings_save()` behind a debounced UI/configuration action, not every input event.
