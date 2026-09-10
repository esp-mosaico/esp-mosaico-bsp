# Mosaico Interaction module

`mosaico_module_interact` discovers and claims an Interaction module through
`mosaico_module_mgr`. It provides button or touch input, PIR and light sensing,
six WS2812 LEDs, and NEC infrared transmission.

Include `mosaico_module_interact.h`. Pin mapping remains private to this
component and is derived from the selected BSP subboard slot.
