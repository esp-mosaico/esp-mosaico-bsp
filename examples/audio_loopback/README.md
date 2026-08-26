# ES8311 audio loopback

This example opens the BSP microphone and speaker at the default sample format
and continuously writes captured microphone audio to the speaker. The loopback
buffer is allocated from internal RAM.

```bash
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

The example sets microphone gain to 30 dB and speaker volume to 50. Keep the
speaker away from the microphone or lower the volume before testing to avoid
acoustic feedback.
