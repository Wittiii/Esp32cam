# DFR comparison source

DFR implementation copied from commit `1633cc5`. The controller differs only
in OV3660 gain mapping, validation, raw status and gain-setting migration.
Migration deliberately preserves the other saved camera settings.
The local `camera_common.h` preserves the baseline shared helper behavior.

`platformio.ini` selects this directory for normal DFR USB/OTA builds and
ignores the modern `micro_rtsp` library. The matching baseline library lives
in `lib/dfr1154_baseline_rtsp/`. AI-Thinker selects the modern library instead.
The newer top-level DFR implementations are retained but not compiled by
these two profiles. Do not compile both implementations together.

This isolates a source-level hardware comparison, not a bit-identical rebuild
of a historical binary or a general security/performance rollback proposal.
