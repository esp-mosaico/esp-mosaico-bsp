# Local changes

Baseline: `joltwallet/littlefs` **1.23.0**, commit `75e4cd412d17c18cd8a10d3dbf09d2620b6dd2e8`.
LittleFS core: `6cb4e86540eca0d9ba62500a298385c9d863c8be`.
This is a modified local copy; the manifest version denotes the upstream baseline. Original licenses are retained.

Add `mosaico_littlefs_with_fs()` to run a synchronous callback under the VFS lock.
The caller must exclude unmount/format and reject open handles before reclamation. The existing cancelable usage traversal is also provided directly by this component. An optional per-blockdev `lookahead_size` mount field avoids changing other volumes' Kconfig defaults. Disk format is unchanged.
The usage helper preserves main's committed-block counting and cancellation semantics.
Upstream test apps/fixtures are omitted; whitespace is normalized.

Selected by the BSP/storage manifests with same-repository `override_path` dependencies. Reapply patches and rerun storage tests/builds when upgrading.

Original SHA-256:

- `src/esp_littlefs.c`: `c64f9c206cc7de1df3c8ad62f9c9dedf0da61edb033f916e4d26d268709d4926`
- `src/littlefs/lfs.c`: `a36d6a095785ddea9571d541d68d3e4ef01d5b255a99d17d3f07fb6ea60ea132`

- `include/esp_littlefs.h`: `a42677fda67c453049019c77bdabc77da4458cceea988f39e3140480c03d06cc`
