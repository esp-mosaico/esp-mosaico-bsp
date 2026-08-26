# Voice wake word and speech commands (esp-sr)

Hands-free wake-word (WakeNet9) plus Chinese MultiNet7 command recognition on
the Mosaico ES8311 mic/speaker path, using `espressif/esp-sr`, with Xiaole TTS
feedback on wake and recognized commands.

This is the board-local reference for skill `add-voice-interaction`. Upstream
examples live in [esp-skainet](https://github.com/espressif/esp-skainet)
(`examples/cn_speech_commands_recognition`). Pure TTS without wake/command is
`examples/voice_tts`.

## Hardware

- ESP-Mosaico with ES8311 populated
- ESP-IDF 5.5+ (esp-sr ships `lib/esp32s31`)
- Enough flash for the ~6 MB `model` and ~3.8 MB `voice_data` partitions
  (16 MB default)

## Build and run

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

`idf.py flash` writes both the generated speech-recognition model image and the
Xiaole TTS voice data supplied by `espressif/esp-sr`.

Say the selected wake word (default Hi,乐鑫 / `wn9_hilexin`), then a command
such as `da kai deng`. Logs print wake and command detections.
