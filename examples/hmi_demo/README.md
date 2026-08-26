# Mosaico HMI demo

This example starts the BSP-managed 480 x 480 CO5300 display and CST9217 touch
controller, then runs the LVGL benchmark demo. It is the smallest complete
reference for `bsp_display_start()` and the LVGL display lock.

```bash
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

All LVGL object access outside the LVGL task must be protected by
`bsp_display_lock()` and `bsp_display_unlock()`.
