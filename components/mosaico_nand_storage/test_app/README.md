# Mosaico NAND storage test application

This ESP-IDF test application validates the board SPI NAND, the patched NAND driver, Dhara BDL, and LittleFS integration on ESP32-S31.

> **Warning:** tests tagged `[destructive]` erase physical NAND blocks or recreate the LittleFS volume. Back up required NAND data before running them. The endurance test deliberately consumes erase cycles.

## Build and run

```bash
source /home/zhouli/esp/esp-idf-6.2/export.sh
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p /dev/usbf1 -b 1152000 flash monitor
```

At the Unity menu, enter `[auto]` to run the bounded automated suite. Tests tagged `[manual]` are excluded and must be selected explicitly by name or tag.

## Test matrix

| Category | Item | Priority | Test case / handling |
| --- | --- | --- | --- |
| Basic | JEDEC ID / device detection | P0 | `P0 JEDEC ID and geometry` |
| Basic | Reset / Busy status | P0 | `P0 reset and busy status` |
| Basic | Page Read / Program / Block Erase | P0 | `P0 page program read and block erase` |
| Basic | Cross-page and cross-block | P0 | `P0 cross-page and cross-block access` |
| Basic | Address boundaries / arguments | P0 | `P0 block-device address boundaries and arguments` |
| Integrity | Deterministic random data | P0 | `P0 deterministic random data` |
| Integrity | 00 / FF / AA / 55 | P0 | `P0 fixed data patterns` |
| Integrity | Unaligned access | P0 | Raw partial-read and filesystem unaligned-write cases |
| NAND | Corrected / uncorrectable ECC | P0 | Automated status-map validation plus manual fault injection; Zbit reports `IGNORED` if it rejects the required repeat-program operation |
| NAND | Factory bad blocks | P0 | Scans every factory bad-block marker without modifying it |
| NAND | Program / Erase failure | P0 | Protects the array and verifies failure propagation |
| NAND | Illegal repeated program | P1 | Manual characterization; accepts explicit failure or documented partial-program behavior |
| NAND | OOB / spare area | P1 | Programs and reads a controlled OOB range in an expendable block |
| Reliability | Power-loss recovery | P0 | Two-phase manual prepare/verify; physically remove power at `POWER_CUT_READY` |
| Reliability | Long random read/write | P1 | 10,000 deterministic random update/readback operations |
| Reliability | Endurance / bad-block growth | P1 | 1,000 erase/program/read cycles and before/after bad-block scans |
| Reliability | Data retention | P1 | Two-phase manual prepare/verify around the required powered-off interval |
| Integration | Concurrent access | P1 | Two VFS writer tasks with size/result verification |
| Integration | Filesystem mount/recovery | P1 | Format, durable write, unmount/remount, readback and reclaim |
| Performance | Sequential/random throughput | P1 | Reports KiB/s for 1 MiB sequential writes and random reads |
| Performance | Maximum latency | P1 | Reports maximum per-operation latency in microseconds |
| Performance | Memory leak / deadlock | P1 | Ten mount/unmount cycles with heap delta and bounded waits |
| Hardware | SPI frequency / mode / DMA | P1 | Manual SIO/QIO at 20/40/80 MHz with DMA-capable buffers |

## Reliability procedure

Power-loss recovery is intentionally split into two tests. Run `P0 power-loss recovery prepare`, wait for `POWER_CUT_READY`, then remove power without using reset. After reboot, run `P0 power-loss recovery verify`.

Data retention follows the same pattern with `P1 data retention prepare` and `P1 data retention verify`. Record the unpowered interval, board, NAND lot, voltage, and ambient temperature externally.

The automated suite uses the final good physical blocks as disposable raw-test space and recreates the logical filesystem for storage tests. It does not mark blocks bad or erase the whole chip.
