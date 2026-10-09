# Mosaico boot splash handoff compatibility

The panel handoff contract now lives in `esp-mosaico-bsp/include/bsp/mosaico_boot_handoff.h`.
This component preserves the previous `mosaico_boot_handoff.h` include for existing applications.
BSP itself does not depend on this compatibility component.

The retained Recovery bootloader still draws the startup image and publishes the LP STORE15 marker.
BSP consumes it to adopt the already-awake panel; without a valid marker it performs normal initialization.
