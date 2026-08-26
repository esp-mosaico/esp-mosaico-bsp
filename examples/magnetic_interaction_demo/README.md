# Magnetic interaction demo

This example runs the ESP-Mosaico V1 two-BMM150 calibration profile by
default. It recognizes simultaneous `TOP`, `RIGHT`, `BOTTOM`, and `LEFT`
presence and independently debounces contact and release on every edge.

Relative rotation, approach distance, field strength, and classifier confidence
are not calibrated by the current data set and are therefore not reported by
the hardware path.

## Mesh Energy Relay application

The hardware UI runs a link-state mesh application on top of committed
ESP-NOW sessions. The implementation stores up to 16 devices, so a 3 x 3
nine-device assembly fits without changing protocol limits. Every device has
up to four direct physical neighbors.

Each committed seam publishes two reciprocal directed link records. A link is
usable only after both records agree on device IDs, edge pairing, and relative
rotation. Newer records are flooded through the connected component with a
bounded TTL. Local links refresh every second; remote records expire after five
seconds, and detach publishes a tombstone immediately. The larger default
receive queue is sized for multi-device topology bursts.

Every device independently reconstructs the same graph, elects the lowest
device ID as root, and derives integer grid coordinates plus global display
rotation. `bsp_display_set_rotation()` applies 0/180-degree compensation
on a dedicated task, including same-edge seams such as `RIGHT`/`RIGHT`. Physical
edge admission limits negotiated rotation to 0 or 180 degrees, and the display
compensation makes every UI share one global reading direction. A cycle
whose edge constraints imply inconsistent coordinates or rotation is reported
as a topology conflict instead of silently choosing one interpretation. The
same conflict state is raised if two device IDs resolve to one grid coordinate.

After topology settles, the application finds a deterministic longest-path
endpoint and starts one Energy Relay traversal there. The opposite endpoint is
kept as the final destination; between them, the nearest unvisited node is
selected so a three-device chain always runs end-to-middle-to-end and a grid
forms a compact sweep. Every leg follows a shortest path. Intermediate devices
forward the same event one physical hop at a time; `HANDOFF` retries make a lost
`START` recoverable, while event ID plus hop number prevents an accepted hop
from restarting its animation. Each physical edge keeps an independent transfer
and retry state. Every routed event carries a canonical topology fingerprint.
Link changes cancel the previous traversal, and frames from an older topology
are rejected. Intermediate nodes clear their completed input edge after
forwarding, leaving the ball visible only at the final destination.

The session matcher negotiates both detected edges and supports the eight
same-axis physical pairings. TOP/BOTTOM edges cannot bind to LEFT/RIGHT edges;
same-edge pairs such as `TOP`/`TOP` and `RIGHT`/`RIGHT` remain valid. If more
than one uncommitted local edge is present, a broadcast claim is rejected as
ambiguous instead of being assigned to an arbitrary edge. Add devices
sequentially: wait for each new seam to show `PAIRED` before attaching the next
device.

## Nine-device validation

1. Flash the same `magnetic_interaction_demo` firmware to all nine devices and
   reset them while separated.
2. Wait until every display leaves `CALIBRATING`.
3. Assemble one seam at a time, waiting for both ends to show `PAIRED`. A 3 x 3
   grid needs 12 seams; complete a spanning tree first, then close the remaining
   cycle-forming seams.
4. Confirm every display reports 9 mesh nodes, the same root ID and no topology
   conflict. Same-edge seams should rotate the affected displays into the
   common global orientation.
5. Confirm one pulse visits all nine devices and crosses intermediate displays
   without restarting at their centers.
6. Open a non-bridge seam and confirm the graph stays connected and subsequent
   pulses take an alternate shortest path. Then open a bridge and confirm the
   separated component elects its own lowest-ID root after stale records expire.

The 3 x 3 graph, root election, coordinates, reciprocal-link admission, expiry,
and shortest path are covered by component tests. Nine-board RF behavior and
visual continuity still require the hardware procedure above; they are not
claimed from compile-time tests alone.

The topology model was also simulated across all 9,910 fixed connected
nine-cell grid shapes with four 0/180-degree assignments per shape. This
39,640-scenario host simulation checks pose reconstruction, not ESP-NOW RF or
magnetic classifier behavior.

Attaching both new neighbors at exactly the same time is not supported because
the magnetic classifier reports occupied local edges but cannot associate a
broadcast ESP-NOW identity with one of those edges.

## Hardware mode

The four edge buttons are read-only indicators. A green button means that the
corresponding edge has reached the confirmed contact state. The status area also
reports the energy role and progress together with filter warm-up, sample
validity, saturation, and the most recent edge event.

The V1 profile uses `BSP_MAGNETOMETER_0@0x11` as the right sensor and
`BSP_MAGNETOMETER_1@0x12` as the left sensor. Invalid synchronized samples clear
the filter history and drive confirmed edges through release debounce instead
of retaining stale detections.

The calibrated hardware path targets non-ferromagnetic desktop surfaces.
Ferromagnetic support, including operation on an iron plate, is intentionally
out of scope because surface coupling can overlap the measured contact fields.

Keep neighboring devices separated while the demo starts. The UI remains in
`CALIBRATING` until an eight-sample window passes range, stability, and
pre-existing-edge checks. Repeated rejected windows show `CAL FAILED`; the
classifier continues looking for a clean window, or `RECALIBRATE` can restart
the process manually. Standalone `TOP` and `BOTTOM` detection then uses the
change from the accepted baseline.

Diagnostics use the same fixed-channel ESP-NOW transport as the interaction
protocol and never connect to an access point. Each device prints its own
`MAGDIAG` calibration, contact, session, and energy records, then broadcasts a
fragmented copy. Connect USB to any one device to see complete relayed records
from the other devices as `mag_diag: relay source=... rssi=... MAGDIAG,...`.
The source ID in each record identifies the originating device, so a multi-node
setup remains observable without attaching a cable to every node. Diagnostic
traffic is best effort and uses dedicated send and receive queues. The shared
receive worker always drains protocol control frames before one diagnostic
frame, so telemetry bursts cannot consume the interaction queue.

## Mock mode

Disable `CONFIG_MAGNETIC_INTERACTION_USE_HARDWARE` to restore the touch-driven
source:

- `TOP`, `RIGHT`, `BOTTOM`, `LEFT`: hold the corresponding simulated magnetic
  observation. Selecting adjacent edges in sequence emits clockwise or
  counter-clockwise orbit gestures.
- `CLEAR`: simulate removing the neighboring device.
- `ROTATE`: change the simulated relative device orientation through
  0/180 degrees.
- `RESET GAME`: clear the four-token sequence.

The demo also starts the ESP-NOW transport. Radio startup failure is treated as
degraded mode so local magnetic interaction remains usable. Real hardware and
mock contacts now enter the same `CLAIM -> ACK -> COMMIT` handshake. A device
shows `NEGOTIATING` after local magnetic confirmation and changes to `PAIRED`
only after both devices commit the same negotiated edge pair and session.

The lower device ID leads simultaneous claims. Logical target IDs prevent
unrelated listeners from accepting acknowledgements or commits. Claims and
acknowledgements retry, committed sessions exchange heartbeats, stale peers are
removed and renegotiated, and releases are repeated before the session slot is
cleared. The wire format is protocol version 5 and requires all devices to run
the updated firmware with the same network ID and channel.

The magnetic classifier reports only the local contacted edge. After both
devices negotiate their local edges, the mesh derives a 0/180-degree relative
pose from that physically valid edge pair. It does not claim an independently
measured continuous magnetic angle.

The protocol-v2 hardware path was validated on 2026-08-04 with two same-model
boards. A complementary TOP/BOTTOM contact committed on both displays,
separation cleared the contact masks and session, and a second contact committed
a new session on both peers. The protocol-driven Energy Relay handoff was also
visually accepted on the same two-board setup. Packet-loss and stale-peer
recovery remain covered by session tests rather than this hardware run.
Protocol v5 covers every cardinal edge pairing in component tests. The mesh
topology payload carries a per-origin boot identity and the routed Energy Relay
payload is version 5. The receiver acknowledges a handoff as soon as it accepts
the ball; the sender keeps the ball visible at the seam until that acknowledgement
arrives. Handoff and acknowledgement loss are recovered by retransmission.
Same-edge hardware display rotation, three-board routing, and nine-board mesh
behavior remain pending hardware validation.

## Calibration status

The cardinal rules are a prototype profile derived from the available
`mag_tile_collect` captures. The standalone cardinal directions have been
replayed against two same-model devices; both devices' stable windows classify
correctly after per-device startup baseline normalization. Adjacent L-shaped
and opposed straight three-device arrangements were replayed on one device.
Broader assembly, temperature, distance, rotation, and hardware-population
behavior remains unconfirmed.
One captured `LEFT+TOP` trial overlaps the `LEFT+BOTTOM` rule and remains a
known ambiguity.

The no-neighbor guard also includes the ACM4 baseline capture. Standalone
`BOTTOM` has now been contact-validated on ACM4 without the USB cable. Its
stable signature uses right-sensor Y around 222 to 249 together with
left-sensor Y around 556 to 603; the left-Y condition keeps it separate from
the weak right-Y transient observed during a slow RIGHT approach.

In the TOP+BOTTOM arrangement, the two vertical fields cancel most of the
right-sensor Y response. The classifier accepts the wider combined Y band only
when right-sensor Z also rises by more than 50 from the startup baseline. This
keeps the retained slow BOTTOM transitions from fabricating TOP contact.

The TOP-versus-LEFT saturation guard uses the shared gap between the original
captures and ACM4: TOP left-sensor Z remains below 650, while confirmed LEFT
captures remain above 650.

Slow LEFT approach captures also showed left-sensor Y passing through the old
BOTTOM band. Adding BOTTOM while LEFT is present therefore requires the
independent positive right-sensor Y signature as well.

## Build

All devices in one assembly must use the same
`CONFIG_MAGNETIC_INTERACTION_NETWORK_ID` and
`CONFIG_MAGNETIC_INTERACTION_ESPNOW_CHANNEL`. Give nearby independent
assemblies different network IDs.

```bash
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```
