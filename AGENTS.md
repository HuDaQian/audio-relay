# AGENTS.md

Instructions for AI coding agents (and humans who want the terse version)
working in this repository. This file is the canonical machine-readable
guide; `CLAUDE.md` just points here so Claude Code picks it up automatically.

## What this project is

A cross-platform audio relay built on Flutter with platform-native capture and playback:
- **macOS Desktop**: Native `ScreenCaptureKit` loopback capture (macOS 13+) in Swift.
- **Windows Desktop**: Native `WASAPI` loopback capture in C++ (`windows/runner/`).
- **Android App**: Kotlin foreground audio service with low-latency `AudioTrack` (`PERFORMANCE_MODE_LOW_LATENCY`), dynamic jitter buffer, and Bluetooth playback.
- **USB / Cable**: Automated `adb reverse` tunneling on `tcp:45108` and `tcp:45109`.
- **LAN / Wi-Fi**: UDP low-latency transport, ChaCha20-Poly1305 encryption, and mDNS discovery.

The wire format and control state machine are specified in `protocol-spec.md` (v2).

## Repo layout

```
audio_relay_flutter/    Flutter app — unified codebase
  android/              Android runner & Kotlin native audio playback service
  macos/                macOS runner & Swift ScreenCaptureKit capture server
  windows/              Windows runner & C++ WASAPI capture server + mDNS
  lib/                  Flutter UI (Material 3 dashboard, pair screens, settings)
protocol-spec.md        Wire format + control messages (canonical)
docs/                   Architecture, roadmap, latency budget
.github/workflows/      CI automation for Android APK and Windows binary
```

## Build / test / lint commands

### Flutter App (all platforms)

```sh
cd audio_relay_flutter
flutter pub get
flutter test
```

### Android build

```sh
cd audio_relay_flutter
flutter build apk --release
```

### macOS build

```sh
cd audio_relay_flutter
flutter build macos --release
```

### Windows build

```sh
cd audio_relay_flutter
flutter build windows --release
```

## Conventions

- **Commits:** imperative mood, scoped prefix when it helps
  (`desktop-app: fix sequence wraparound in jitter calc`). Keep commits
  focused; don't mix protocol changes with unrelated refactors.
- **Rust:** `rustfmt` defaults, `clippy` clean (`-D warnings` in CI — don't
  add `#[allow]` to silence a real lint without a comment explaining why).
  Keep platform-specific code behind `cfg` gates rather than sprinkled ad hoc
  through shared modules — isolate it in `capture/`'s per-OS submodules and
  anything else that's genuinely OS-specific.
- **Kotlin:** standard Android/Kotlin style (4-space indent, no wildcard
  imports). Prefer coroutines/Flow over raw threads/callbacks for anything
  new. Keep `AudioTrack`/socket code off the main thread.
- **No new heavyweight dependencies** (Electron/Tauri/WebView equivalents,
  large frameworks) without discussing first — the whole point of this
  project is a small, dependency-light footprint (single portable `.exe`,
  no bundled runtime). See `docs/architecture.md` §2.2 for why Tauri/Electron
  were rejected for the Windows UI.
- **Don't invent codecs/formats not in `protocol-spec.md`.** v1 is
  deliberately raw PCM (see spec's rationale). If you want Opus or another
  codec, that's a `docs/roadmap.md` Phase 7 discussion, not a drive-by change
  — the packet header already reserves a `codec_id` byte for this.

## Things that need real hardware to verify

This project's riskiest assumptions are hardware/OS behaviors that can't be
confirmed by reading code or running on a CI runner:

1. WASAPI loopback capture actually works from a non-admin account on the
   target Windows versions.
2. `AudioTrack` in `PERFORMANCE_MODE_LOW_LATENCY` + `USAGE_MEDIA` actually
   routes to A2DP earbuds, not the phone speaker.
3. mDNS advertise/browse works over an Android-hosted hotspot, not just a
   home router (multicast is the thing most likely to misbehave there).

These are `docs/roadmap.md` Phase 0 validation spikes. If you're an agent
without access to the physical devices, say so explicitly rather than
claiming these are verified — leave a note in the PR and/or a tracking issue
instead of marking the related roadmap item done.

## Do not

- Do not commit secrets, session keys, or paired-device config files (see
  `.gitignore` — `%LOCALAPPDATA%\AudioRelay\config.toml` and Android
  equivalents are runtime state, never repo content).
- Do not silently change the UDP packet header layout — it's a breaking
  protocol change and needs a version bump + spec update + both apps
  touched together.
- Do not add telemetry/analytics/network calls beyond LAN discovery and the
  relay itself. This app talks to your own phone on your own network and
  nothing else.
