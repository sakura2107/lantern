# Lantern v0.3 backend handoff

Backend implementation is available; this is **not complete product acceptance**.
Use feature/v03-backend and its feature commits, not a fresh main checkout.
The continuation began at f2a0134 plus the preserved uncommitted backend work.

## Integration contract

- Exact signatures, ownership/threading, examples and UI error/state guidance:
  [V03-BACKEND-API.md](V03-BACKEND-API.md).
- Wire fields/capabilities: [V03-PROTOCOL.md](V03-PROTOCOL.md), indexed from PROTOCOL.md.
- Threat boundaries: [V03-SECURITY.md](V03-SECURITY.md), indexed from SECURITY.md.
- Static build/source/object recipe: [MEDIA-BUILD.md](MEDIA-BUILD.md), THIRD_PARTY.md,
  tools/media and licenses/media. Signing: [ANDROID-SIGNING.md](ANDROID-SIGNING.md).

No protected GUI, page, AvcView, or production manifest file was edited. Claude
must replace the old UI queues/receivers, wire limits/status/movie APIs, handle
backpressure, and do the SurfaceView migration. Backend callbacks must not turn
off input/audio because video recovers. Debug validation creates only Lantern
Surfaces; it does not restore a production movie player. The standalone debug
manifest adds loopback-only cleartext policy, instrumentation and TestDocuments;
these are not production release settings.

## A — quality and decoder capabilities

Implemented H264/HEVC NVENC, HEVC preference/fallback, 720/1080/1440/2160 presets
and 30/60/120 FPS, native screen/refresh clamps, 16 MiB video-only frames, online
reconfigure/IDR and applied configuration metadata. Fixed mode reduces bitrate
then drops stale frames; automatic mode keeps FPS and uses roughly 5-second down
and 15-second up steps. Initial 60 FPS budgets are 5/12/22/45 Mbps, scaled by FPS,
bounded 250 kbps–160 Mbps. P1/low latency/CBR/no B frames/AQ 4 favor screen text.
These are engineering budgets, not visual-quality guarantees.

Desktop v5 sends paired size/rate limits and recalculates them after online FPS
changes: 4K60 + 2K120 no longer implies 4K120. Android enumerates actual hardware
pairs; Windows can supply DecoderLimits.modes. v3/v4 retain conservative rectangle
negotiation for compatibility.

| Measured validation | Result |
|---|---|
| Windows GPU encode/decode/present | 24 codec/size/rate combinations, 16 presented frames each, 384 GPU surfaces, no CPU surfaces |
| Android H264 | 134 assertions, 12 fixed + 3 automatic requests, same TLS online changes |
| Android HEVC v5 | 119 assertions, corresponding 15 requests |
| Android receive rate, short windows | H264 29.99–114.13 FPS; HEVC 29.99–113.32 FPS |
| TextureView updates | H264 29.16–57.48/s; HEVC 29.16–60.00/s |
| Paired limits/automatic policy | Native wire clamp and deterministic up/down timing passed |

Windows 4K used a test-only upscale provider and an owned generated window,
not a native 4K display. Unverified: stable 4K120, full Windows network/display
matrix, real constrained-link automatic changes, physical 120 Hz and optical
<=80 ms latency. QPC timestamps/camera measurement method are documented;
protocol timing is not click-to-photon evidence. Audio defaults low latency and
retains its synchronization switch.

## B — authenticated UDP AV

Implemented C++/Java TLS-issued directional AES-256-GCM keys/nonces, authenticated
headers, 2048-packet anti-replay, 1024-byte fragments, 8+1 parity, bounded NACK,
reassembly/retransmit, endpoint challenge/proof, scope/session cancellation and
shared link budget. Same-port UDP carries AV only; fallback stays on the same
TLS subscription and emits Op61. Unknown sessions get no amplification.

Physical transport test: **77 assertions**. Initial blocked video/audio probes
fell back in **1.522/1.622 s**; midstream video/audio loss fell back in
**1.543/1.716 s**. Injection used owned sockets/unknown session IDs and changed
no firewall policy. Native/JCA vector, tag/replay/FEC/reordering/NACK and owned
UDP tests exist. Firewall/UPnP support distinct TCP/UDP rules/leases/reachability,
but real public UDP, WAN congestion and actual UPnP exposure are not accepted;
setup tests use mocks. No DNS/TUN/security setting was changed.

## C — reliable input and recovery

New channels connect immediately, use 512-slot bounded admission, coalesce only
adjacent unsent moves, maintain critical input order and replay only against a
deduplicating host. Op57 binds token/channel/epoch, with bounded ten-minute
in-memory ledgers. Reliable internal Op40 heartbeats run every ten seconds and
are hidden from business results. Application errors are acknowledged results.
Old-host ambiguous ACK loss stops replay; host restart/expired ledgers do not
provide durable exactly-once execution and require deliberate restart.

Native real ten-second heartbeat/immediate connection/result/callback isolation,
queue/order/lost-ACK tests passed. Phone idle heartbeat, TLS interruption and
64 focus-query replay/flood passed. Generic Windows media lifecycle waited a
real **30 seconds**, emitted recovering/disconnected, kept background retries,
returned normal on resumed packets, and treated revocation as terminal.
Latest receiver/host shutdown ordering compiled and ran through native tests.

Old UI input queues are intentionally not replaced. Real critical key/drag/scroll
flood through the newly wired UI, slow peers, long idle AV, and physical 30-second
outage/status acceptance on each platform remain integration acceptance work.

## D — original reading and static adapted media

Preserved four-request/16 MiB read-ahead and pipelined transfers. Fixed reader
connection tracking/reconnect cleanup, seek generations and cancellation of
obsolete prefetch; stale results cannot populate the new generation. Close owns
opening/live connections. Phone SHA-verified **32 MiB**, about **8.69/8.60 s** in
the two codec matrix runs; seek back to start **268/275 ms**. This does not prove
high-bitrate long-movie throughput.

Real static FFmpeg does NVDEC -> CUDA scaling -> H.264 NVENC, optional libass ASS
burning, AAC 48 kHz stereo and independent two-second random access. Authorized
handles/version checks remain enforced; no path enters a shell/filter string.
HDR/non-H264-or-HEVC GPU sources fail explicitly. One segment encodes per host;
output/caches/queues, cancellation and contention are bounded. Windows adapted
movie playback is intentionally not exposed this round, as allowed.

**v3 (Claude, 2026-11-13): H.264 High + AAC in MPEG-TS, HLS version 3.** The
earlier attempts failed for format reasons, not bugs: Android's system HLS
player demuxes only MPEG-TS (fMP4 gave -1007), and its TS demuxer has no HEVC
(the HEVC TS prototype played audio with no video track). H.264/AAC TS is the
one form every player accepts. Segments are two seconds; a remainder under
0.5 s joins the last segment (count = max(1,(ms+1499)/2000), last <= 2.5 s), so
no tail is a one-picture sliver. Audio uses one global grid of 1024-sample AAC
frames; each segment encodes two frames of real pre/post-roll and keeps only
the frames starting inside it, so segments tile exactly (the v2 fMP4 output
repeated ~21 ms of priming per segment, +0.18 s over 12 s).

Physical phone (Android 16), 12-second generated 4K HEVC10 SDR + ASS/AAC fixture,
v3 TS (**43 assertions passed**):

- **System MediaPlayer** prepared the loopback HLS URL, first frame **1.35 s**,
  1280x720, seek to 2.5 s in **65 ms**, played to completion without error,
  **323 Surface updates**.
- Debug HTTP consumer + MediaExtractor/MediaCodec decoded all **6 segments**
  independently; decoded AAC totals **12.032 s** = the movie, no repeats.
  (A lone TS file never releases its final H.264 access unit to MediaExtractor,
  so it reports 59 of 60 pictures per segment; players reading the stream do not
  lose it.) Token, HEAD, Range and lease close passed.
- Native test: H.264/AAC TS, global video timeline, audio of consecutive segments
  ends/starts on the same 90 kHz tick, ASS burn, source conflict, cancellation.

Not yet verified: third-party player apps (none operated), long movies,
subjective AV sync, all fonts/subtitle formats. The player app must itself allow
cleartext loopback HTTP; common players do.

## E — endpoints, size, compatibility and release materials

Both explicit preferred-endpoint overloads retain certificate pinning, remembered
winner and 250 ms stagger; single-entry probes do not pollute multi-entry order.
No configuration/state-file format bump is introduced.

Against an isolated actual baseline 267b2df build: new Windows backend -> old
host H264/TCP video/audio/input passed; old Windows CLI -> new host stat/caps
passed. New Android -> old host passed **24 assertions** including hardware
H264 display, concurrent verified download, fullscreen/hide/stop; concurrent
render about **59.86 FPS**, 32 MiB download **6.749 s**. This is not the complete
old/new UI/all-operation matrix, and no old Android APK replaced personal data.

Final candidate host **9,049,088 bytes** vs local baseline **1,508,352**:
**+7,540,736 bytes**, below the approximate 10 MB growth target. Debug APK
**1,121,151 bytes** vs local baseline debug **1,088,306**: **+32,845**, below 4 MB.
These are local debug-artifact comparisons, not a comparison with another release
packaging. Final SHA-256 inventory is supplied with the delivery.

Pinned FFmpeg rebuilt successfully in a new directory, with GPL/nonfree/version3
disabled and CUDA LLVM enabled. It reused the pinned static dependency prefix;
this is not an independent clean rebuild of every dependency. tools/media packs
full corresponding archives, actual patched dependency trees, port patches,
notices, application objects/static libraries and a portable relink recipe.
The exported package was actually relinked into new host and CLI executables.
The relinked CLI passed pinned-TLS first/tail media RPCs using a generated file:
real NVDEC/NVENC + ASS/AAC returned valid fMP4 (91,325 / 6,982 bytes). That run
predates v3; rebuild media with h264_nvenc (tools/media/build_media.py) and rerun.
Public distribution must ship that matching material and expose attribution.
Claude owns release UI attribution and production signing; no release/private
key is created or distributed here.

Original Android signing identity was retained. Independent before/after checks
found **both original files preserved**, semantically identical pairing settings,
and **no new persistent files**. No data clear, reset/clean, personal movie input,
other-app operation, sub-agent or scheduled task was used.

## Final gate and remaining acceptance

Final native gate: **25/27 passed** in **117.32 seconds**. public_network and
host_admin failed on IPv6 loopback with Windows 10013, the known local network
restriction; neither is counted as passed. No firewall/TUN policy was changed.
All other tests, including core, input/lifecycle, media, Windows client/features,
native integration and Java interoperability passed. This was the one milestone
regression; unchanged passing tests were not then repeatedly rerun. The
first final build hit only an owned phone process locking the CLI executable;
that target was relinked after the test ended. Preserve the failed attempt log.

Outstanding acceptance:

1. Claude wiring, SurfaceView, notices and production-signed release.
2. Third-party player apps and long original/adapted playback (system
   MediaPlayer HLS passes since v3).
3. Real WAN TCP/UDP/UPnP/firewall/IPv6 reachability and weak-network transitions.
4. Full Windows network/display matrix, critical input flood and long idle/
   physical-disconnect state acceptance in each newly wired UI.
5. Physical 120 Hz/4K120 sustained, optical latency and subjective AV sync.
6. Independent clean dependency build and independent-machine relink audit.

English feature commits belong only on feature/v03-backend. No artifacts, local
reports, DPAPI/keystores/backups, machine-specific paths or real addresses belong
in Git. Final push/delivery evidence is recorded in the local continuation files.
