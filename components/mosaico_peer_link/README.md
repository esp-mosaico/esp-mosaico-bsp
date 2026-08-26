# Mosaico peer link

`mosaico_peer_link` provides the ESP-NOW transport, contact-session handshake,
and bounded 16-node topology model used by Mosaico interaction applications.
It depends on `mosaico_interaction` for physical edge types and topology rules.

Start with `MOSAICO_PEER_LINK_CONFIG_DEFAULT()`, call
`mosaico_peer_link_init()`, and release Wi-Fi/ESP-NOW resources with
`mosaico_peer_link_deinit()`. When `manage_wifi` is true, the component owns
the Wi-Fi lifecycle for the link.

The transport provides retries, session identifiers, topology propagation, and
application payload routing. It does not configure ESP-NOW encryption. Use
`examples/espnow_chat` for the minimal peer path and
`examples/magnetic_interaction_demo` for negotiated physical contacts.
