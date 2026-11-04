# Mosaico NAND storage

Optional LittleFS storage for the board NAND. Enable
`CONFIG_MOSAICO_NAND_STORAGE_ENABLE=y` and depend on `mosaico_nand_storage`.
Git consumers can select `components/mosaico_nand_storage` in this repository;
its manifest resolves the BSP and patched drivers from the same revision.

```c
#include "mosaico_nand_storage.h"

ESP_ERROR_CHECK(mosaico_nand_storage_mount(NULL)); // Retains /nand; never formats.
// Use normal VFS file IO. Close all files/directories before maintenance.
esp_err_t maintenance = mosaico_nand_storage_reclaim();
```

- Clock defaults to the BSP selection (80 MHz, with 40 MHz available). A nonzero
  storage clock override or explicit config takes priority. QIO bootstrap uses
  single IO at up to 20 MHz and the driver's detected-chip QE policy.
- Default read cache: 64 KiB PSRAM; allocation failure uses uncached IO.
  Lookahead: 8 KiB using LittleFS's allocation policy. Reclamation uses two
  bitmaps of about 8 KiB PSRAM each for a 1 Gbit device, plus cache metadata.
- Data writes remain synchronous. Geometry, ECC, bad-block handling and existing
  filesystems are retained. Mount failure never formats or grows a volume.
- Call `reclaim()` after committed changes. It synchronizes the root and trims
  only unreachable pages under the VFS lock, rejecting any open file/directory.
  A cold sweep can take seconds; yielding CPU does not release the VFS lock.
  Failed maintenance does not roll back a committed application transaction.
- Stop and drain every VFS user before `unmount()` or explicit `format()`.
  Formatting clears logical mappings and recreates LittleFS; obtain user
  confirmation in the application. Lifecycle/maintenance calls are serialized;
  cancellation callbacks must not perform IO or reenter storage APIs.
- One volume uses the whole board NAND. The legacy page path and the optional
  block-device path are mutually exclusive. Keep the legacy configuration with
  `NAND_FLASH_ENABLE_BDL` disabled for existing page-API applications.

The patched SPI NAND, Dhara and LittleFS components are maintained alongside
this component. Their `LOCAL_CHANGES.md` files record upstream baselines and
patches; use their manifests instead of copying `managed_components` outputs.
