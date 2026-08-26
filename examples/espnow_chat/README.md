# ESP-NOW chat (no magnetic dependency)

Minimal two-board chat over `mosaico_peer_link` (ESP-NOW broadcast of
`MOSAICO_PEER_MSG_APP_DATA`). Does **not** use magnetic contact / topology.

- AI button click → broadcast a short chat line
- Periodic hello beacon so nearby boards appear in the log
- Received APP_DATA / HELLO printed with source id and RSSI

## Hardware

- Two (or more) ESP-Mosaico boards within ESP-NOW range
- ESP-IDF >= 5.5 / 6.x with `esp32s31` target

## Build and run

Flash the same firmware to both boards:

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

Press the AI button on either board; the other should log the chat payload.

For magnetic edge negotiation and routed mesh messaging, see
`examples/magnetic_interaction_demo` and skill `add-esp-now-network`.
