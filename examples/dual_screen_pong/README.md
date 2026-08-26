# Dual-screen Pong

Two ESP-Mosaico boards form one 960×480 Pong court. Each board renders
one 480×480 half and uses a Joystick Board for its paddle. ESP-NOW is the game
transport in both layouts:

- **Wireless layout:** a glowing portal hides the physical gap between screens.
- **Seamless layout:** optional desktop magnetic contact confirms that the
  right edge of the left board touches the left edge of the right board.

Magnetic calibration/contact is never required to pair or play. If it is
unavailable or invalid, the game stays in wireless layout. Ferromagnetic
surfaces such as iron plates are not supported by magnetic layout detection.
For seamless detection, power both boards on while separated and keep them
still until joystick calibration finishes, then join the correct screen edges.

## Controls

- Joystick Y: move paddle.
- Joystick X: tilt paddle and shape spin.
- B: unused by this example.
- 1: confirm player or serve.
- 2: pause or resume.
- 3: restart after a match.
- 4: lightweight emote.

Paddle hits, wall rebounds, seam crossings, serves, goals, and match wins use
distinct non-blocking tones and haptic patterns. Pairing, ready, pause, resume,
disconnect, and emote actions also have short state cues. Paddle contact has
the strongest short impact pulse during normal play.

On first use, rotate the joystick through its full range, then release it at
the center. Calibration is non-blocking. Small joystick changes are smoothed,
while large movements and center release remain responsive. Removing either
Joystick Board pauses the match; reconnecting it resumes the normal flow.

## Pairing and synchronization

Nearby boards discover each other with ESP-NOW HELLO messages. With one
candidate, press button 1 on both devices. The lower stable device ID becomes the
authoritative host and the left-screen player. It runs fixed 120 Hz physics;
the client sends input at 50 Hz and receives complete snapshots at 30 Hz.
Old packets are discarded and discrete events are deduplicated.

An 800 ms receive outage pauses play. Reconnection within five seconds keeps
the score and starts a new countdown. A peer reboot safely returns both boards
to the lobby.

## Build and flash

Program each Joystick Board EEPROM with the `Joystick Board` profile first,
then flash the same application to both Mosaico boards:

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
```

The default configuration keeps logs on UART0. Use the board's UART serial
port for diagnostics.

## Validation

Run the deterministic desktop network model:

```sh
python3 tools/simulate_network.py
```

Hardware acceptance should cover wireless separation, correct and reversed
left/right docking, separation and reconnection during play, joystick hot-plug,
magnetic calibration failure, and 5–10% packet loss.
