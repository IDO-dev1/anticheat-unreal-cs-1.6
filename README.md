# Live Unreal Scanner v0.2

Metamod anti-cheat research plugin for CS 1.6/ReHLDS. It analyzes player `usercmd` data
live inside the server without Python, HTTP or blocking network requests.

## Does this version really use UnrealDemoScanner logic?

Yes, **v0.2 contains live adaptations of selected algorithms/constants from the actual
UnrealDemoScanner 1.76.0 source**. It is not the original `.dem` scanner running inside
HLDS; the original is C# and expects demo frames. The adapted detectors are clearly named:

- `UDS_AIM_TYPE_5_ADAPTED`
- `UDS_IDEALJUMP_ADAPTED`
- `UDS_AUTOATTACK_ADAPTED` (experimental warning)

See `THIRD_PARTY.md` for exact adaptation notes and attribution.

## Safety

- No automatic ban or kick.
- No Python or external API.
- No socket/HTTP call from the game thread.
- Evidence is logged as `WARNING` or `DETECTED`.
- Results must be manually reviewed while thresholds are calibrated.

## GitHub build

1. Upload this folder to a GitHub repository.
2. Open **Actions**.
3. Run **Build Linux Metamod SO**.
4. Download artifact `live-unreal-scanner-linux-i386`.
5. Extract `live_unreal_scanner_mm_i386.so`.

## Install

Copy to:

`cstrike/addons/liveac/dlls/live_unreal_scanner_mm_i386.so`

Add to `cstrike/addons/metamod/plugins.ini`:

`linux addons/liveac/dlls/live_unreal_scanner_mm_i386.so`

Restart and check `meta list`.

## Current limitations

v0.2 has no hitbox/visibility traces, recoil punch data, weapon private-data timing or
client-side sensitivity value. Therefore it is an evidence collector, not a final verdict
engine. The next useful step is ReGameDLL/ReAPI weapon timing plus hitbox-aware aim events.
