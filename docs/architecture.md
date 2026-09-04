# Architecture & design rationale

This document captures the *why* behind this project's design — the
alternatives considered, the constraints, and the honest latency budget.
For the *what* (wire format), see [`protocol-spec.md`](../protocol-spec.md).
For the *when* (build order), see [`roadmap.md`](roadmap.md).

## 0. Is this feasible without admin rights or a driver?

Yes. Every hard constraint maps onto an existing, documented, user-mode API:

| Constraint | Satisfied by |
|---|---|
| No admin install | Portable single `.exe`, runs from `%LOCALAPPDATA%` or `Downloads\` |
| No virtual audio driver | WASAPI **loopback capture** (user-mode, built into Windows since Vista) |
| No kernel/service install | WASAPI is a COM API called from a normal process |
| Phone stays the Bluetooth endpoint | Phone never proxies raw BT audio — Android's normal audio routing sends whatever the app plays to the already-connected A2DP device automatically |
| Works over phone hotspot, no Internet | Plain LAN sockets + mDNS, no cloud/relay server involved |
| Low latency | Achievable in the ~120–260ms range end-to-end (dominated by Bluetooth A2DP, not the relay pipeline — see §6) |

The one thing worth setting expectations on up front: **Bluetooth A2DP
itself typically adds 100–200ms**, true of every audio-relay tool that
exists — it's a Bluetooth/codec limitation, not something a Wi-Fi relay app
can remove. This project's job is to not add much *on top* of that.

## 1. System architecture overview

```
┌─────────────────────────────────┐        ┌────────────────────────────┐
│   macOS / Windows Desktop       │        │   Android Phone            │
│   (Unified Flutter App)         │        │   (Unified Flutter App)    │
│                                 │        │                            │
│  Loopback capture:              │  UDP   │  UDP audio receiver        │
│  - macOS: ScreenCaptureKit      │ (PCM)  │  (Kotlin AudioReceiver)    │
│  - Windows: WASAPI C++          │───────▶│         │                  │
│         ▼                       │  LAN/  │  Jitter buffer             │
│  Framer + sequencer             │hotspot/│         │                  │
│         │                       │  USB   │         ▼                  │
│         ▼                       │        │  AudioTrack (low-latency,  │
│  UDP / TCP socket (audio)       │        │  USAGE_MEDIA) → routed by  │
│                                 │        │  Android to whatever BT    │
│  TCP socket (control:           │  TCP   │  device is already active  │
│  discovery ack, pairing,        │◀──────▶│                            │
│  heartbeat, reconnect)          │        │  Foreground Service +      │
│                                 │        │  NSD (mDNS) browse         │
│  mDNS advertise / DNS-SD        │  mDNS  │  ("_audiorelay._udp")      │
│  ("_audiorelay._udp")           │◀──────▶│                            │
└─────────────────────────────────┘        └────────────────────────────┘
```

The application is built on a unified **Flutter** codebase (`audio_relay_flutter/`)
with native platform runners for capture and playback:
- **macOS Desktop**: Native `ScreenCaptureKit` loopback capture (macOS 13+) in Swift (`macos/Runner/`).
- **Windows Desktop**: Native `WASAPI` loopback capture in C++ (`windows/runner/`).
- **Android App**: Kotlin foreground audio service (`android/`) with low-latency `AudioTrack` (`PERFORMANCE_MODE_LOW_LATENCY`), dynamic jitter buffer, and Bluetooth playback.
- **USB / Cable**: Automated `adb reverse` tunneling on `tcp:45108` and `tcp:45109`.

### 1.1 Why native Android playback, not a browser

Browser-based (Web Audio/WebRTC in Chrome for Android) was seriously
considered and rejected:

- Android Chrome suspends/throttles background tabs and applies aggressive
  audio-focus/Doze restrictions once the screen is off — exactly the
  "everyday real-time use" scenario this needs to survive.
- No way to run a foreground service with a wake lock from a tab, so
  reconnection-after-sleep is much less reliable.
- WebRTC would still need a signaling path and a media-engine bridge on the
  desktop side to inject non-microphone PCM into an `RTCPeerConnection`.
- A native Kotlin service gets `AudioTrack` in `PERFORMANCE_MODE_LOW_LATENCY`, direct
  control over buffer sizes, and a `MediaSession` so it behaves like a
  normal media player to Android (survives Doze the same way Spotify does).

## 2. Desktop component

### 2.1 Audio capture

**macOS:**
- **API:** `ScreenCaptureKit` (`SCStream`) loopback capture in Swift (macOS 13+).
- Event-driven audio buffer callback capturing system audio mix without virtual audio drivers.
- Automatically handles interleaved / non-interleaved float32 or int16 PCM conversion to 48kHz 16-bit stereo.

**Windows:**
- **API:** WASAPI loopback capture in shared mode, event-driven (not
  polling) for lowest latency (`windows/runner/wasapi_capture.cpp`).
- No driver, no admin — this is a standard `IAudioClient` activated on the
  default render endpoint with `AUDCLNT_STREAMFLAGS_LOOPBACK`. Documented,
  stable, used by every legitimate "record what you hear" tool on Windows.
- Automatically handles float32/int16 and 44.1kHz/48kHz sample formats.

### 2.2 Language/runtime choice: Flutter + Native Platform Runners

| Component | Technology | Rationale |
|---|---|---|
| **UI & App Shell** | Flutter (Dart) | Modern Material 3 UI across Android, macOS, and Windows with a single shared codebase, hot reload, and responsive layouts. |
| **macOS Runner** | Swift (`ScreenCaptureKit`, `Network.framework`) | High performance system audio loopback without kernel extensions, native `NWListener` for TCP/UDP with mDNS. |
| **Windows Runner** | C++ (WASAPI, WinSock2, BCrypt) | User-mode COM loopback capture, zero extra runtime dependencies, cryptographically secure RNG via BCrypt, custom raw DNS-SD responder. |
| **Android Service** | Kotlin (`AudioTrack`, `DatagramSocket`) | Foreground service with wake lock, low-latency AudioTrack, dynamic jitter buffer, surviving screen-off and Doze mode. |

### 2.3 Desktop-side responsibilities

1. Loopback-capture the default output endpoint continuously.
2. Chunk into small frames (5–10ms, e.g. 480 samples @ 48kHz) and stamp each
   with a monotonically increasing sequence number + timestamp.
3. Advertise itself on the LAN via mDNS / DNS-SD (`_audiorelay._udp.local`, TXT
   record with hostname + device ID + protocol version).
4. Run a TCP control channel per connected phone: pairing handshake,
   capability exchange, heartbeat (PING/PONG), clean disconnect/reconnect signaling.
5. Stream raw PCM frames over UDP to the paired phone's audio port once
   paired (or TCP loopback over USB cable via `adb reverse`).
6. Persist last-paired device (ID + derived session key + last-known name)
   under `%LOCALAPPDATA%\AudioRelay\config.json` on Windows, or
   `~/Library/Application Support/AudioRelay/config.json` on macOS, so re-pairing isn't
   needed every launch.
7. Minimal UI: connection status, paired device name, 6-digit pairing code
   with 5-minute cryptographic expiration, Start/Stop, and audio permissions check.

## 3. Android component

### 3.1 Language/runtime: Kotlin, native app

Flutter was considered and set aside: low-latency `AudioTrack` and precise
buffer control aren't well exposed through Flutter's audio plugins, and this
app would end up writing a Kotlin platform channel for the one part that
matters most anyway.

### 3.2 Android-side responsibilities

1. **NSD** (Network Service Discovery) to browse for `_audiorelay._udp` —
   Android's built-in mDNS wrapper, no extra library needed.
2. TCP control channel to the chosen laptop: pairing, heartbeat, capability
   negotiation.
3. `DatagramSocket` UDP receiver for the audio stream.
4. **Jitter buffer:** small ring buffer targeting ~20–40ms depth (tunable —
   larger on a hotspot connection than on a solid router). Sequence numbers
   detect loss/reorder; on a gap, conceal with a short fade-to-silence
   rather than complex FEC for v1.
5. **AudioTrack**, `PERFORMANCE_MODE_LOW_LATENCY`, `USAGE_MEDIA` /
   `CONTENT_TYPE_MUSIC`. `USAGE_MEDIA` is what lets Android's normal
   audio-routing logic send the stream to whatever device is already the
   active output — including the earbuds the user paired themselves.
   Deliberately **not** `USAGE_VOICE_COMMUNICATION`/SCO — that route is
   mono and low quality.
6. **Foreground Service** (`FOREGROUND_SERVICE_MEDIA_PLAYBACK` type on
   Android 14+) + a `MediaSession`, so the OS treats this like Spotify
   rather than a background process it's free to kill. Holds a partial
   `WakeLock` and a `MulticastLock` (needed for mDNS to keep working with
   the screen off).
7. Auto-reconnect: `ConnectivityManager` callbacks catch Wi-Fi/hotspot
   changes and IP churn, re-run discovery, and resume without user action.
8. Minimal UI: connected laptop name, current output device, latency
   indicator, and a Start/Stop switch on the Home screen — the notification
   action alone used to be the only way to stop, and stopping that way
   didn't update the UI's own state, so reopening the app after using it
   could show a stale "Streaming" status for a service that had actually
   been torn down. Both are now the same code path, and it correctly leaves
   the UI in a genuine "off" state.
9. **Forgetting a laptop is one-sided by design** (it only touches this
   phone's storage, not the laptop's), so `RelayService` treats "the laptop
   still reports us as paired" and "we still have a usable local key" as
   two independent facts rather than trusting the laptop's claim — falling
   back to the pairing-code flow whenever the local key is missing,
   regardless of what the laptop says. Forgetting also clears the
   auto-reconnect pointer if it pointed at the forgotten laptop, so a
   forgotten laptop isn't immediately retried on the next launch.

## 4. Network protocol summary

Full detail in `protocol-spec.md`. Key decisions:

### 4.1 Codec: raw PCM for v1, not Opus

Deliberate, LAN-specific call:

- Bandwidth is not the constraint — 48kHz/16-bit stereo PCM is ~1.5 Mbps,
  trivial for any Wi-Fi network or phone hotspot.
- A codec buys bandwidth at the cost of encode+decode latency (typically
  5–20ms combined) and CPU on both ends, for zero benefit on a local
  network.
- Raw PCM keeps the pipeline dead simple: capture → packetize → socket →
  jitter buffer → play.
- **v2 escape hatch:** the packet header reserves a `codec_id` byte from
  day one, so adding Opus later (if real-world testing on a weak hotspot
  shows it's worth it) is a non-breaking upgrade, not a redesign.

### 4.2 Transport split

Audio over UDP (loss-tolerant, latency-sensitive); pairing/heartbeat/control
over TCP (low-frequency, needs reliability, latency doesn't matter there).

### 4.3 One packet must fit one MTU

**Every audio datagram stays under `packet::MAX_DATAGRAM_BYTES` (1200), and
the sender splits a captured chunk across as many packets as that takes.**

This is not a micro-optimisation. Raw PCM is bulky: 10ms of 48kHz stereo
16-bit audio is 1920 bytes, so a whole chunk in one datagram exceeded the
~1472-byte UDP payload a 1500-byte MTU allows, and *every single packet*
was IP-fragmented into two. IP fragments are all-or-nothing — lose either
half and the whole packet is gone — so fragmenting roughly doubles the
effective loss rate, on precisely the marginal Wi-Fi/hotspot links where
loss is already the limiting factor. It showed up as constant brief
dropouts.

1200 rather than 1472 leaves headroom for IPv6's larger header and any
VPN/tunnel encapsulation in the path, the same conservative budget QUIC and
WebRTC use.

Because each split packet carries its own sequence number and timestamp,
the receiver cannot distinguish a split chunk from natively-small ones —
so this needed no protocol change. It does mean **the receiver must not
assume a packet size**: `JitterBuffer` learns the real one from arriving
packets, since concealing a lost packet with the wrong amount of silence
injects drift the correction loop then has to fight.

### 4.4 Capture must be *paced*, not merely correct

The single worst defect this project has had was not a wrong calculation —
it was capture arriving in bursts.

Both OS capture APIs will happily hand you audio in large infrequent
blocks, and both do so **by default**:

- **Linux/PulseAudio.** `pa_simple_new` takes a `BufferAttr`, and passing
  `None` lets the server choose `fragsize` — documented as defaulting to
  "something like 2s". Measured against a real PipeWire server, that meant
  194 of every 200 reads returned instantly and then the stream stalled for
  **341ms**. Passing an explicit fragment size took the median gap to
  10.65ms with a worst case of 11.11ms. See
  `linux_impl::record_buffer_attr` and the `capture_delivery_cadence` probe
  that measures it.
- **Windows/WASAPI.** Event-driven shared mode is inherently paced at the
  engine period, so it does not have Linux's problem — but the buffer
  capacity passed to `IAudioClient::Initialize` must be the engine's
  *default* period, not the hardware *minimum*. The minimum only applies to
  exclusive mode, which loopback capture cannot use, and asking for a
  buffer below one engine period leaves no headroom for a late read.

Why this matters more than it looks: a burst is not just latency, it is
**unhideable** latency. A 341ms gap in delivery needs a >341ms jitter
buffer to conceal, which is far more than this app targets — so the
receiver underran on every burst and played concealment silence instead,
which is what "it cuts out constantly" was. Bursts also dump tens of UDP
packets into the network at once, which a phone hotspot answers by
dropping them, turning a pacing bug into a loss bug as well.

**So: any change to a capture backend must keep delivery paced, and pacing
is a thing to measure rather than assume.**

### 4.5 Buffer depth is a duration, never a packet count

The receiver's jitter buffer is configured in **milliseconds**. It converts
to packets internally, against the packet size it observes at runtime.

Expressing it in packets — which this project originally did — silently
ties the setting to how the sender happens to be packetising. A packet's
duration depends on the sender's latency mode *and* on the MTU split in
§4.3, so a default of "3 chunks" turned out to mean ~18ms, with even the
maximum setting reaching only ~36ms. Ordinary Wi-Fi jitter exceeds that,
and a phone hotspot exceeds it by a lot, so the buffer underran more or
less continuously.

The same rule applies to concealment: a lost packet must be concealed with
exactly as much silence as was lost, so the buffer learns the real packet
size rather than assuming one.

### 4.6 Queues between real-time stages must be bounded

The capture→sender queue is bounded and **drops** when full.

An unbounded queue between a real-time producer and a slower consumer does
not buffer, it accumulates: every chunk that piles up is delay the listener
never gets back, and nothing in the pipeline ever gives it back. Bounded,
the same stall costs a brief dropout instead — recoverable, and far less
annoying than audio drifting permanently further behind.

### 4.7 Backlog is latency, and only skipping sheds it

Buffer depth that sits persistently above target is not jitter tolerance —
it is delay the listener pays on every packet for the rest of the session.

Nothing in the steady-state design removes it. The clock-drift loop moves
one PCM frame per ten packets, which is correct for cancelling tens of ppm
of crystal drift and useless here: shedding 200ms of backlog that way takes
about ten minutes. So a single transient stall — a descheduled receive
loop, a burst of Wi-Fi retransmits — used to leave playback running
seconds behind live permanently.

`JitterBuffer` therefore **skips forward** when depth exceeds its target by
a wide margin, discarding the backlog in one step. That costs a brief
discontinuity and buys back correct latency, which for live audio is
overwhelmingly the right trade. The two mechanisms are deliberately
separate: frame-level correction for continuous drift, skip-ahead for
accumulated backlog.

The same reasoning bounds every other queue in the path. The UDP receive
buffer is sized to absorb a burst and no more, because audio sitting in the
kernel is latency the jitter buffer can neither see nor trim.

### 4.8 Forgetting a device must end its session

Pairing state and session state are separate things, and "forget" has to
act on both. Erasing only the stored key left the connection running on a
key that had just been revoked: audio kept flowing, the laptop stayed in
`Streaming` (so §2.3's pairing code never appeared), and the phone kept
trying to resume a session it could no longer prove it owned. Neither side
could recover without an app restart.

Both sides now end the live session when the user forgets a device, and the
peer is dropped through a fresh handshake — which, because §5's pairing
flags are one-sided facts, correctly asks for a new code.

Relatedly: **an explicit user action must preempt an automatic one.**
`connectTo` declines to start while an attempt is in flight, so a Connect
tap during an automatic retry did nothing at all, which the user
experiences as the app looping on its own instead of asking for a code.

### 4.9 The receiver must be able to resynchronise

A jitter buffer that only ever discards "late" packets has a failure mode
where it stops permanently: if the play position ever runs *past* the
sender, every subsequent packet looks late, so the buffer stays empty and
playback stays silent while the sender transmits normally. Nothing in the
steady-state logic recovers from it.

So `JitterBuffer` treats a wildly-diverged sequence position, or sustained
starvation, as a signal to re-prebuffer from wherever the sender actually
is. The cost is one brief gap; the alternative is silence until the user
restarts the app.

The corollary on the playback side: **anything that can make the playback
loop spin without blocking is a correctness bug, not just a performance
one**, because each iteration advances the expected sequence number. A
failed `AudioTrack.write` returns immediately rather than pacing to real
time, which is why `PlaybackTrack` checks the result, rebuilds a dead
track, and backs off instead of retrying instantly.

## 5. Discovery & pairing

- **Discovery:** mDNS/DNS-SD — Windows advertises via a custom DNS-SD responder
  on `224.0.0.251:5353`, macOS advertises via `NWListener.Service` (`_audiorelay._udp`),
  and Android browses via `NsdManager`. Works identically
  on a home router or the phone's own hotspot, since mDNS only needs
  multicast on the local subnet.
- **Pairing:** Windows generates a random 6-digit code and displays it; the
  user enters it once on the phone. The code seeds an HKDF-derived session
  key (see `protocol-spec.md` §5), which then encrypts UDP audio payloads
  with ChaCha20-Poly1305.
- After first pairing, both sides remember each other, so normal daily use
  is "open both apps, they reconnect automatically" — no code re-entry.
- **"Remembering each other" is two independent, one-sided facts** — each
  side's `paired_devices`/saved-key store, not a shared state. Forgetting a
  device on one side (there's a "Forget" action in both apps' Settings)
  doesn't tell the other side. The server's `HELLO_ACK.paired` flag is
  therefore only ever a hint, not a guarantee the phone can actually
  `REPAIR` — see `protocol-spec.md` §4/§5 and the Android app's handling in
  `RelayService.connectTo`, which falls back to a fresh pairing code
  whenever it doesn't actually have a usable local key, regardless of what
  the laptop claims. Symmetrically, the laptop always keeps a pairing code
  visible whenever nothing is actively streaming (§2.3 item 7), so this
  case never requires restarting either app to recover from.

## 6. Latency budget (honest, not aspirational)

| Stage | Typical | Notes |
|---|---|---|
| WASAPI capture buffer | 3–10ms | Event-driven, small buffer |
| Packetization | ~5–10ms | One packet per chunk |
| Network (router Wi-Fi) | 1–5ms | LAN, same subnet |
| Network (phone hotspot) | 5–15ms | Slightly higher, still local |
| Jitter buffer (Android) | 20–40ms | The main *tunable* knob — trade latency for glitch-resistance here |
| AudioTrack low-latency buffer | 10–20ms | Android's floor, device-dependent |
| **Pipeline subtotal** | **~45–90ms** | This is what the app actually controls |
| Bluetooth A2DP (SBC/AAC) | 100–200ms | Inherent to the earbuds' codec — not fixable by this app |
| Bluetooth aptX-LL / LE Audio (LC3), if earbuds support it | 40–80ms | Automatically better if the earbuds negotiate it |
| **Realistic end-to-end total** | **~150–290ms** | Fine for video/music/general desktop audio; noticeably behind for twitch-reaction gaming |

Worth stating plainly rather than overpromising: this is a "watch videos,
listen to music, sit in meetings" latency profile, not a competitive-gaming
one — and that ceiling is set by Bluetooth, not by this project.

## 7. Reliability & reconnection

- Heartbeat (1s) on the control channel; 3 missed beats → mark
  disconnected, stop feeding audio, start mDNS re-browse with exponential
  backoff.
- Wi-Fi/hotspot IP changes: both platforms expose network-change callbacks
  (`ConnectivityManager` on Android; adapter/route-change notifications on
  Windows) — trigger re-discovery rather than assuming the old socket still
  works.
- Laptop sleep/wake and phone sleep/wake: control-channel heartbeat
  naturally detects these as a disconnect; reconnect flow resumes without
  user action.
- Packet loss concealment: short fade-to-silence over the missing span
  rather than repeating stale samples (repeating audibly "stutters" more
  than a brief dip).

## 8. Future enhancements (architected for, not built in v1)

- Opus codec fallback for weak/congested networks (packet header already
  reserves a codec-ID byte).
- Process-specific loopback capture ("capture just this app").
- Browser-based receiver as a zero-install alternative, once/if the native
  app's reliability patterns are proven and worth porting.
- Microphone-back-to-laptop path — explicitly out of scope for v1: this is
  a one-way relay, not a VoIP app.

## 9. Summary

Every hard constraint (no admin/root, no driver, phone stays the BT endpoint)
maps cleanly onto APIs that already exist for exactly this purpose: WASAPI
loopback on Windows, PulseAudio monitor-source capture on Linux, and normal
`USAGE_MEDIA` audio routing plus NSD/mDNS on Android. The only
externally-imposed ceiling is Bluetooth A2DP latency,
which no software on either end can remove — everything else in the
pipeline is within this project's control and budgeted at under 100ms.
