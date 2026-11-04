# Local changes

Baseline: `espressif/dhara` **1.1.0**, commit `4d842853e0bd625aa9a1e86366955b8feb676c27`.
This is a modified local copy; the manifest version denotes the upstream baseline. Original licenses are retained.

`dhara_map_trim()` skips GC for unmapped sectors. Mapped trims retain automatic GC; disk format is unchanged.
Selected by the BSP/storage manifests with same-repository `override_path` dependencies. Reapply patches and rerun storage tests/builds when upgrading.

Original SHA-256:

- `dhara/dhara/map.c`: `38c321f0885e5925e5fc8daddfee5817774ba222d698a8389c5dcd93bd599355`
