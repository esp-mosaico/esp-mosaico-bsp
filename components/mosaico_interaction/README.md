# Mosaico interaction primitives

`mosaico_interaction` contains the hardware-independent state machines used by
Mosaico magnetic applications:

- filtered two-BMM150 edge classification;
- approach, contact, release, and orientation events;
- local neighbor topology and display-rotation helpers;
- small game and energy-transfer primitives.

The checked-in S31 magnetic profile is a prototype tied to the tested
mechanical arrangement. Recalibrate and revalidate it after changing magnets,
sensor orientation, enclosure, or assembly tolerances.

Use `examples/magnetic_interaction_demo` for the complete classifier, ESP-NOW,
topology, and LVGL path. See `skill.add-magnetic-interaction` for layer
selection, ownership, calibration, and validation boundaries.
