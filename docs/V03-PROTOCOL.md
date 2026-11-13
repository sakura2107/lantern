# v0.3 protocol additions

This supplements [PROTOCOL.md](PROTOCOL.md). Integers are unsigned big endian unless
specified; strings/blobs retain their existing length prefix. All new RPCs require
the existing certificate-pinned authenticated TLS HELLO. Unknown versions fail
explicitly. No state-file format is changed by these additions.

## Capabilities and compatibility

Op30 adds `input-replay=v1`, `desktop-quality=v3`, and, when the UDP listener exists,
`datagram=av-v1;decoder-table=v1`. A host built with actual static media libraries
adds `media-adapt=hevc-hls-fmp4-v2`; a build without them advertises no adaptation.
Existing desktop/audio capability strings and Op21 remain available. The new
client selects desktop v1/v2 and H.264/TCP on old hosts; a v3 host permits online
quality, v4 adds UDP, and v5 adds paired decoder limits. Old clients may request
v1/v2 unchanged from a new host. Audio v1/v2 remain accepted. Adaptation v2 is a
new capability; the unreleased MPEG-TS adaptation prototype is not advertised.

## Desktop configuration and control

Op32 request v3/v4:
`u32 version,monitor,width,fps,bitrate,prefer_hevc,max_width,max_height,max_fps,automatic; u64 initial_bps`.
v5 appends `u32 count; count * (u32 width,height,fps)`; count is 1..64.
Size/rate pairs are for the selected codec, not a rectangular union. The highest
available rate at or below the requested rate is selected, then the largest size
available at that rate, clamped to the monitor's aspect ratio/native dimensions.
The same calculation runs after every online FPS change. v3/v4 retain their
rectangular ceiling for compatibility.

The successful configuration returns `u32 version,width,height,fps,bitrate;
str capture,encoder; blob parameter_sets; str clock_domain` (v2+), followed by
`str stream_id,codec` (v3+). Codec is `h264` or `hevc`; stream_id is a random
64-hex-character bearer identifier, also bound to the authenticated owner.
The clock domain is `host-qpc-us` in both clients and the wire configuration.

v4/v5 append the UDP offer below. Op34 readiness is `u32 1` plus `u32 want_udp`
for v4/v5. The host answers Op61 before media, with `str tcp|udp`. Midstream UDP
expiry sends a further Op61 `tcp` and continues on this same TLS stream.

Op33 response payload after the ordinary response header:
`u64 sequence,host_pts_us; u32 flags` (bit0 IDR, bit1 repeated), then, for v3+,
`u32 actual_width,height,fps,bitrate; str codec; blob updated_parameter_sets;
u32 selected_width,constrained,dropping`, then `blob Annex-B frame`.
Unchanged parameter sets may be empty. Configuration precedes the corresponding
IDR. Frame bounds are 16 MiB including the envelope; other messages remain 4 MiB.
Missing video sequences request one IDR and discard dependent frames until it
arrives. They do not create a new control connection or terminate input.

Op58 requires view scope. Request `u32 1`; response `u32 1,count`, then
`u32 index,width,height,refresh_hz; str name; u32 avc,hevc` per monitor.

Op59 requires view scope and ownership of stream_id. Request
`u32 1; str stream_id; u32 width,fps,automatic,keyframe; [u64 initial_bps]`.
The optional bandwidth field is accepted for early v3 peers. Response
`u32 1,width,fps,bitrate; str codec,path; u32 selected_width,constrained,dropping`.
It is a snapshot; subsequent Op33 metadata confirms the applied configuration.
Allowed widths are 1280/1920/2560/3840, rates 30/60/120. Automatic mode retains
the requested FPS, drops one resolution after about five seconds of sustained
shortage, and rises one after about fifteen seconds of headroom. Fixed mode
reduces bitrate to 250 kbps then marks dropping, retaining requested size/rate
subject to hardware ceilings. Initial budgets at 60 FPS are 5/12/22/45 Mbps,
scaled by FPS, bounded to 160 Mbps. NVENC uses P1/low latency/CBR/no B frames/AQ 4.

Op60 requires view scope. Request `u32 1`; response `blob probe` (256 KiB).
Clients measure bytes/time locally; this is an initial estimate, not an optical
latency measurement or a promise of sustained capacity.

## Reliable input (Op57)

Requires control scope and input lane. Request
`u32 1; str channel_id; u64 sequence; u16 nested_op; blob nested_request`.
channel_id is random 64 hex, sequence begins at one, nested payload is <=8192.
Allowed nested ops: 22,35,36,37,38,39,40,51,52 (see their existing schemas).
Response: `u64 acknowledged_sequence; u32 result_code; str reason; blob result`.
Application failures are acknowledged and must not be blindly repeated.
Unauthorized/epoch changes terminate authorization. A repeated sequence must
have identical operation/payload; changed data or a gap returns conflict.

The host retains at most 64 owner/channel replay ledgers for ten minutes,
in memory only. Reconnection with the same token/channel reuses the ledger;
revocation or session epoch change invalidates it. This is not durable exactly
once delivery across host restart. A conflict requires a new deliberate session.
Both clients connect immediately and send an internal reliable Op40 focus query
after ten seconds idle. The heartbeat refreshes the lease and detects loss;
it consumes a sequence but is hidden from business result callbacks.
Queues have 512 slots; only adjacent unsent moves coalesce. False admission
means the caller retains and retries a critical event. Older hosts cannot
deduplicate an ambiguous lost ACK: the client stops replay rather than risking
a second click. No UI is closed by the backend channel.

## UDP AV v1

Offer (inside TLS): `u32 port; blob session[16]; blob host_send_key[32];
blob host_receive_key[32]`. Fresh random AES-256-GCM keys are generated per media
session/direction. Host-to-client direction is 1, client-to-host is 2. Nonce is
`u32 direction || u64 packet_sequence`; sequence starts at one and is never reused
under a key. The 56-byte unencrypted header is authenticated as GCM AAD:

| Offset | Field |
|---|---|
| 0 | u32 magic 0x4c554433 |
| 4 | u16 version 1, u16 type |
| 8 | 16-byte session ID |
| 24 | u64 packet sequence |
| 32 | u64 media frame sequence |
| 40 | u32 fragment index, fragment count, total frame bytes, payload bytes |
| 56 | <=1024 encrypted payload bytes, then 16-byte GCM tag |

Types: DATA=1, PARITY=2, NACK=3, HELLO=4, CHALLENGE=5, PROOF=6, FEEDBACK=7.
Bad lengths/tags, unknown sessions, stale/replayed packets are silently dropped.
Only authenticated packets advance the 2048-packet replay window. The listener
accepts at most 4096 packets/s and four sessions. HELLO is 32 random bytes;
an authenticated 32-byte challenge must be echoed within 1500 ms from the same
candidate endpoint before media is sent. Verified endpoints are locked to the
session. Challenges are limited to one per 250 ms. No discovery trust is reused.

One XOR parity fragment covers each group of eight data fragments. Reassembly
retains at most two frames for 200 ms, with a 16 MiB frame ceiling. NACK payload
is `u32 count; count*u32 fragment_index`, count <=128, emitted after 15 ms and
before 160 ms, at most once per 20 ms. Retransmits use fresh packet sequences,
bounded cached media, and the common link budget. FEEDBACK is `byte 1`, with
last delivered frame in the header; it is sent about every 100 ms.
Probe and feedback expiry are 1500 ms. Unusable UDP falls back to the existing
TLS socket; it does not carry control, pairing, files, or original media.

Audio Op41 v3 extends v2 with the offer; Op43 readiness appends want_udp.
Op42's existing sequence/host-clock/flags/codec payload becomes the encrypted
frame, including the ordinary response envelope. Audio gaps mark discontinuity;
old audio is dropped to catch up. Clock timestamps are diagnostics; click-to-
photon claims require filming the owned input target and display together at a
known camera frame rate, or instrumenting capture/input plus a photodiode.

## Adapted media

Op70 requires files_read. Request `u32 1; str path`; response
`u32 1; u64 size; str source_version; u64 duration_ms,bitrate;
u32 width,height,hdr,count; count*(u32 index,kind; str codec,language,title)`.
Kinds: audio=1, subtitle=2. Indexes are source stream indexes.

Op71 requires files_read. Request `u32 2; str path,source_version;
u32 width,height; i32 audio_index,subtitle_index; u32 segment_index`.
Audio -1 selects the default, subtitle -1 disables burning. Response
`u32 2; blob self_contained_fragmented_mp4`, bounded below 4 MiB. Segments cover
two seconds except the tail. AAC padding beyond the last video sample holds
its last image. Each result includes ftyp/moov plus moof/mdat; tfdt retains one
global timeline across independent encoder instances. The client exposes one
init.mp4 and strips repeated initialization from seg/N.m4s. HLS version 7 uses
EXT-X-MAP, VOD/ENDLIST, and independently decodable HEVC hvc1/AAC stereo media.
HEVC MPEG-TS is deliberately not the v2 contract: Android's system container
support is narrower than its MediaCodec capabilities. See
[Android formats](https://developer.android.com/media/platform/supported-formats)
and [HLS authoring](https://developer.apple.com/streaming/hls-authoring-specification-for-apple-devices.html).

Adaptation uses NVDEC -> CUDA scaling -> NVENC, with optional bounded libass
ASS composition, bitrate approximately one third of the source (750 kbps–10 Mbps).
HDR tone mapping and non-H264/HEVC GPU sources fail explicitly with unsupported;
the UI should offer original playback. Source-version changes fail with code 10.
One segment encodes per host; contention returns busy. Disconnect, cancellation,
revocation, and session changes terminate work. User paths are authorized handles,
not filter strings or command lines. Android contains no FFmpeg or subprocess.
