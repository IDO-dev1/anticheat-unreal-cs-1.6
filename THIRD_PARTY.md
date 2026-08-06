# Third-party attribution

## UnrealDemoScanner

Upstream project: `UnrealKaraulov/UnrealDemoScanner`
Upstream version inspected: 1.76.0, main branch, 2026-08-06.
License displayed by upstream: WTFPL.

This project does **not** embed the GoldSrc demo parser or run the original C# scanner.
It contains live C++ adaptations of selected detector behavior documented and implemented
in `UnrealDemoScanner/UnrealDemoScanner.cs`:

- `UDS_AIM_TYPE_5_ADAPTED`
  - Uses the upstream 15-sample sensitivity history idea.
  - Uses the upstream minimum playable sensitivity, `/2.1` detection threshold,
    `/1.001` warning threshold and 500 ms attack correlation window.
- `UDS_IDEALJUMP_ADAPTED`
  - Arms after more than 10 airborne frames.
  - Requires a new jump/takeoff within 150 ms.
  - Emits strong evidence after more than 11 consecutive strikes.
- `UDS_AUTOATTACK_ADAPTED`
  - Uses the upstream repeated command-frame gap range of 2..7 and four-strike concept.
  - Marked experimental because the v0.2 live collector does not yet read weapon
    availability/private weapon timing used by the original scanner.

The live implementation is independently structured for per-player server-side usercmd
streams. Names containing `ADAPTED` intentionally distinguish them from an unmodified
execution of the original scanner.
