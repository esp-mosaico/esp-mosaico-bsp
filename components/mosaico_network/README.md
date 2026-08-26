# Mosaico network service

`mosaico_network` combines the ESP-NOW transport, magnetic neighbor sessions,
link-state topology synchronization, shortest-path routing, and application
messages behind one API. Applications report confirmed physical contacts and
receive topology or application events; they do not need to run protocol tick
tasks or forward topology records themselves.

The service supports up to 16 devices. A valid topology is a connected,
non-overlapping arrangement on the cardinal grid with at most one neighbor on
each physical edge. Physical edge admission allows only same-axis seams, so
connected devices use 0 or 180 degree relative rotation. Pose-inconsistent
cycles and overlapping device coordinates produce a topology
conflict and block application sends.

## Minimal integration

```c
#include "mosaico_network.h"

#define APP_SERVICE_ID 1

static mosaico_network_handle_t network;

static void network_event(
    const mosaico_network_event_t *event,
    void *user_ctx)
{
    (void)user_ctx;
    if (event->type == MOSAICO_NETWORK_EVENT_MESSAGE_RECEIVED &&
        event->data.message.service_id == APP_SERVICE_ID) {
        /* Copy or consume event->data.message.payload before returning. */
    }
}

void app_network_start(void)
{
    mosaico_network_config_t config = MOSAICO_NETWORK_CONFIG_DEFAULT();
    config.network_id = 0x12345678;
    config.event_cb = network_event;
    ESP_ERROR_CHECK(mosaico_network_start(&config, &network));
}

void app_edge_changed(mosaico_edge_t edge, bool present, uint32_t timestamp_ms)
{
    ESP_ERROR_CHECK(mosaico_network_set_contact(
        network, edge, present, timestamp_ms));
}
```

Use `mosaico_network_send()` for a routed unicast and
`mosaico_network_broadcast()` for one delivery to every reachable node. A
service ID separates application protocols. The current application payload
limit is 64 bytes. Unicast uses end-to-end acknowledgement and bounded retries;
the sender receives `MOSAICO_NETWORK_EVENT_MESSAGE_DELIVERED` or
`MOSAICO_NETWORK_EVENT_MESSAGE_FAILED`. Broadcast delivery is best effort.

Every start creates a nonzero boot identity. Application duplicate suppression
uses the source device, boot identity, and message ID, so a restarted sender can
immediately reuse its message counter.

All devices in one assembly must use the same nonzero `network_id`, radio
channel, and firmware wire version. Different products or nearby independent
assemblies should use different network IDs. Public event callbacks are queued
and serialized on the internal protocol task and must not block. Event payload
storage is valid only for the duration of the callback. The raw-message callback
is a transport-context debug hook and may run concurrently.

The default `manage_wifi = true` starts Wi-Fi STA on the configured fixed
channel without connecting to an access point. Set `manage_wifi = false` when
the application already owns a started Wi-Fi driver; startup then verifies that
the existing primary channel matches `channel` and does not change Wi-Fi mode or
channel.

## Assembly behavior

Attach one new seam at a time and wait for the neighbor-attached event before
adding another seam to either device. Once identified, devices may form lines,
branches, cycles, grids, and rotated combinations. If several unidentified
neighbors are already present on one device, the magnetic edge observations do
not contain enough information to associate a radio identity with a physical
edge; the session manager rejects the ambiguous claim instead of guessing.

## Validation boundary

`tools/simulate_mosaico_topologies.py` exhaustively reconstructs all 9,910 fixed
nine-cell grid shapes with four rotation assignments per shape. This public
release does not include the internal protocol Unity test applications.
Nine-device RF capacity, enclosure variation, and visual continuity remain
unconfirmed until the nine-board procedure is completed.
