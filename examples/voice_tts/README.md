# Chinese TTS (esp-sr Xiaole)

Speak Chinese prompts through the Mosaico ES8311 speaker using the Xiaole voice
data supplied by `espressif/esp-sr`. The example maps a `voice_data` partition
at runtime; `idf.py flash` writes the component's
`esp_tts_voice_data_xiaole.dat` image to that partition automatically.

Companion to `examples/voice_wake_cmd` and skill `add-voice-interaction`.

## Build and run

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

Expect audible prompts after boot. Type is optional; this example loops a fixed
set of phrases.
