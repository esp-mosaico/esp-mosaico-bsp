# Local changes

Baseline: `espressif/spi_nand_flash` **1.4.4**, commit `cc69ba1ff55bafd9cc39d0694984e72e0517cb08`.
This is a modified local copy; the manifest version denotes the upstream baseline. Original licenses are retained.

- Prepare and verify QE before quad pin routing using the detected chip descriptor.
- Opt-in, per-device polling for page-load and READY commands; page data keeps interrupt/DMA.
  The appended `poll_short_commands` config field defaults to false for legacy callers.
- Clear and sync the logical map only for confirmed formatting (BDL command `0x95`).
- Remove forced GC after logical erase; retain TRIM, erased reads as FF, and Dhara automatic GC.
- Sample the READY deadline before each STATUS read. Upstream 1.4.3 checked it after a BUSY read,
  so a 10 ms preemption at 100 Hz falsely timed out 25 us page loads during downloads.
- Drop the always-`ESP_OK` `ret` from ID-read error logs; silent-check builds warned it was unused.
- Omit upstream test apps/helpers; normalize whitespace. ECC, bad-block policy and geometry are unchanged.

Interfaces: [mosaico_nand_extension.h](include/mosaico_nand_extension.h).
Selected by the BSP/storage manifests with same-repository `override_path` dependencies. Reapply patches and rerun storage tests/builds when upgrading;
80MHz requires validation for each chip and board.

Original SHA-256:

- `src/dhara_glue.c`: `d528d949eb76e22bb61cd2da5fbf9ac0709517bf40c84ccc876eda6f9bc324dc`
- `src/nand_wl_blockdev.c`: `02b9d043fcb7bfd6038dcc067e682acf939dbad1240026d4e48617482082053d`
- `src/nand_impl.c`: `a6f0c575c9a0f184fc24c66aaa604f3bb7b5cb88666aaf7636efec57e12cdb3e`
- `src/devices/nand_alliance.c`: `bf1f34c2fa10ca936b79f26b758f205ab9011111ac397ad2a08337dc1826219b`
- `src/devices/nand_fm.c`: `1f04893922b57b4ace7d1fc04bf7c963a83f43d48c7e99a09ff015c3604bd19f`
- `src/devices/nand_gigadevice.c`: `5c34333b0d801c2e4e6b737798d5406eeea66e218ab530fd8a6dd46a34e7d4fd`
- `src/devices/nand_macronix.c`: `7896ae8a607498900c149bc10b843f6c2f58680a42d359ff87ffd5bc2daf48ea`
- `src/devices/nand_micron.c`: `b895fca318684dd8de28a466df6a20eb8711dfdccec777a8c4de7083d77b59d4`
- `src/devices/nand_winbond.c`: `48f321ac047579906e0caa65da59f7928dac96318ce0cdda9e30c21655672f10`
- `src/devices/nand_xtx.c`: `35c8b212d9e71e8683596a43556b6abca621bc80fd312e4ffcd7e06cfe44934a`
- `src/devices/nand_zetta.c`: `024722e7672eddd5be12384c858dcd9149d68eca47266051639664ebaaf06206`

- `include/spi_nand_flash.h`: `3bcccbdd8b4a0e82b1d9c78fecd03141c7df34f5c362b767ef8bb18203e733d5`
- `src/spi_nand_oper.c`: `51a9e6a6171c4697857b0c8b1ad58b8e28e6ca91d50049cde1276c6326d16903`
