# Button and status LED

This minimal example initializes the on-board AI button and status LED. Button
press, release, click, repeat, and long-press events are logged; holding the AI
button turns the status LED on.

```bash
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

This is the **wired** closed-loop path: USB `MOSAICO_STATUS` only, no Wi-Fi.
For the disposable WLAN scaffold, copy `examples/agent_feedback`
`wlan_scaffold.*` and tear it down after UDP validation succeeds.

USB logs include `MOSAICO_STATUS` `boot` and a measured `startup` result
after the buttons and LED are ready. Collect with:

```bash
python ../../tools/collect_serial.py --duration 15 \
  --expect 'MOSAICO_STATUS .*"check":"startup".*"code":0' \
  --fail 'assert failed|Guru Meditation|panic'
```

This example uses the on-board button and LED through the BSP. For the
hot-pluggable Button LED subboard, use the `mosaico_button_led` component.
