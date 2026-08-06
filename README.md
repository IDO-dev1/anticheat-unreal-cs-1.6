# Live Unreal Scanner for CS 1.6

Prototype live anti-cheat detector for HLDS/ReHLDS, inspired by the measurement approach of demo scanners. It is an independent implementation and does not include UnrealDemoScanner source code.

## v0.1 detectors

- Large aim-angle snap in a very short command interval.
- Repeated attack immediately after a snap.
- Repeated jump command exactly on landing.
- Per-player score with time decay.
- Console alerts and evidence log.

## Safety/performance

The Metamod callback performs bounded in-memory arithmetic only. There is no network request, Python dependency, sleeping, waiting, or automatic punishment.

## Build through GitHub

Push this repository to GitHub and run **Actions → Build Linux Metamod SO**. The workflow installs a 32-bit compiler, fetches the official HLSDK and Metamod-HL1 headers, compiles the `.so`, runs a detector test, and uploads a packaged artifact.

See `docs/INSTALL_HE.md`.

## Scope

This initial release is suitable for telemetry and threshold calibration. Accurate triggerbot/hitbox and wallhack analysis will require server-side traces, weapon timing, target visibility and labelled clean/cheat samples.
