# Mosaico boot splash handoff

The retained Recovery bootloader owns the first visible LCD frame.  After the
complete splash has been written it publishes a one-shot marker in LP STORE15.
The BSP consumes that marker before creating the CO5300 panel and adopts the
already-awake panel without resetting it or repeating the Sleep Out delay.

Splash failure remains non-fatal: without a valid marker the BSP follows its
normal full-reset initialization path.
