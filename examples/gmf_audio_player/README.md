# GMF audio simple player (embedded MP3)

Plays an embedded `alarm.mp3` through `espressif/esp_audio_simple_player`
(ESP-GMF) and writes PCM to the BSP ES8311 speaker via `esp_codec_dev_write`.

## Hardware

- ESP-Mosaico with speaker populated
- ESP-IDF >= 6.0

## Build and run

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

Press the board RESET or power-cycle to hear the tone again (loops until stop).

## Notes

- Prefer BSP + `esp_codec_dev` for raw PCM loopback (`examples/audio_loopback`).
- Use this example when you need GMF decode/pipelines (MP3/AAC/…) on Mosaico.
- Upstream patterns: [esp-gmf](https://github.com/espressif/esp-gmf)
  `packages/esp_audio_simple_player`.
