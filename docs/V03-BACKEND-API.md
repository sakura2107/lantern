# v0.3 backend API for Claude

All blocking network/open/query calls belong on workers. The UI owns lifetime,
posts callbacks to its UI thread, and keeps HWND/Surface owners alive until
workers stop. This work does not wire the protected GUI/page/AvcView/manifest
files. Protocol details and security boundaries are linked from PROTOCOL.md
and SECURITY.md.

## Windows desktop and audio

Headers: desktop_connection.hpp, audio_connection.hpp, media_receiver.hpp,
windows_playback.hpp and connection_state.hpp, under include/lantern.

```cpp
struct QualitySettings { uint32_t width=1920, fps=60; bool automatic=false; };
struct DecoderMode { uint32_t width, height, fps; };
struct DecoderLimits {
    uint32_t width=3840, height=2160, fps=120;
    bool hevc=false;
    std::vector<DecoderMode> modes; // optional paired limits, desktop v5
};
DesktopConnection(ClientCredentials, uint32_t monitor, QualitySettings,
                  DecoderLimits={}, bool automatic_reconnect=false);
void DesktopConnection::ready();
DesktopPacket DesktopConnection::next();
void DesktopConnection::set_quality(QualitySettings, bool request_keyframe=false);
QualityStatus DesktopConnection::quality();
void DesktopConnection::interrupt() noexcept;
static std::vector<DisplayLimits> DesktopConnection::limits(const ClientCredentials&);
AudioConnection(const ClientCredentials&, bool prefer_opus=true,
                bool automatic_reconnect=false);
void AudioConnection::ready();
AudioPacket AudioConnection::next();
std::string AudioConnection::path() const;
void AudioConnection::interrupt() noexcept;
```

DisplayLimits contains index,width,height,refresh,name,avc,hevc. QualityStatus
contains width,fps,bitrate,selected_width,constrained,dropping,codec,path. Actual
height is in DesktopPacket.config. `quality()` is synchronized; `config()` is
stream-owner-thread-only. `set_quality` uses a separate control connection,
not a new video subscription. Monitor changes create a new subscription.

```cpp
DecoderLimits decoder;
decoder.hevc = WindowsVideo::supports_hevc();
auto stream = std::make_shared<DesktopConnection>(credentials, 0,
    QualitySettings{2560,120,false}, decoder);
stream->ready();
auto packet = stream->next(); // contains actual configuration before this frame
// A separate control worker can request:
stream->set_quality({3840,60,true}, true);
```

Use DesktopReceiver/AudioReceiver for background recovery. Both are aliases
of `MediaReceiver<Connection,Packet>` with this constructor:

```cpp
MediaReceiver(Factory, Frames, Events={}, Errors={});
// Factory = function<shared_ptr<Connection>(bool automatic_reconnect)>;
// Frames = function<void(const Packet&)>;
// Events = function<void(ConnectionState)>;
// Errors = function<void(Code,const string&)>;
ConnectionState state() const;
std::shared_ptr<Connection> connection() const;
void close();

DesktopReceiver receiver(
    [credentials,decoder](bool automatic) {
        return std::make_shared<DesktopConnection>(credentials,0,
            QualitySettings{1920,60,false},decoder,automatic);
    },
    [&](const DesktopPacket& packet) { /* decode on this owning media worker */ },
    [&](ConnectionState state) { /* post a copy to the UI */ });
```

Initialize COM as MTA on a worker that owns WindowsVideo/WindowsAudio. On changed
dimensions/headers create a decoder for packet.config before feeding that IDR;
the TLS stream remains intact. `WindowsVideo(HWND,const VideoConfig&,
shared_ptr<AvTimeline>={},function<bool()>={})` owns the MF/D3D11 decoder.
`frame(span<const uint8_t>,uint64_t host_pts,bool visible,double zoom=1)` presents
GPU surfaces; `stats()` reports decoded,presented,gpu_surfaces,cpu_surfaces.
The HWND remains valid through close/join. Do not call receiver/channel close
or destroy them from their own worker callback; post teardown to the owner.
Connect factories must use bounded connection timeouts; close cannot cancel an
arbitrary user-supplied factory that has not returned its connection yet.

Default playback is low latency. Preserve the synchronization switch with one
shared AvTimeline; `timeline->synchronize(enabled)` controls audio-clock pacing.
AudioConnection provides codec/rate/lookahead to WindowsAudio. No video recovery
callback should turn off input or independent audio.

## Android desktop, audio and limits

Package org.lantern.client; existing classes remain package-private.

```java
new AvcDesktop.Profile(width, fps, bitrate, monitor, automatic);
AvcDesktop.start(Remote.Credentials, AvcDesktop.Profile);
AvcDesktop.stop();
AvcDesktop.close();
AvcDesktop.connectionState(); // ConnectionStatus.Snapshot
SystemAudio.start(Remote.Credentials);
SystemAudio.stop();
SystemAudio.close();
SystemAudio.connectionState();
DecoderCapabilities.query(); // immutable List<Limit>
DesktopSession.limits(Remote.Credentials); // List<Display>
```

`start` applies same-monitor quality changes online through the backend's control
worker. DecoderCapabilities.Limit includes mime,name,width,height,fps for each
supported hardware combination. Do not multiply the largest size by the largest
rate. DesktopSession v5 sends the actual combinations for the selected codec;
older v3/v4 hosts use the conservative rectangular offer. HEVC is preferred;
set AvcDesktop.preferHevc=false before starting a deliberate H264 session when
needed for compatibility. Do not mutate that preference during a live stream.

AvcDesktop.Events retains configured/frame/failed/reconnecting and adds
`quality(DesktopSession.Status)` and `connectionState(ConnectionStatus.Snapshot)`.
Status has width,height,fps,bitrate,codec,path,selectedWidth,constrained,dropping.
SystemAudio.Events adds the same connection state callback. Callback delivery in
these presentation backends is posted to main; the lower-level DesktopSession,
AudioSession and InputChannel are worker APIs. `activeSession.status` is diagnostic
state; use callbacks for production presentation.

Keep existing AvcView Surface ownership for now; SurfaceView migration belongs
to Claude. A shared `MediaTimeline` can be supplied through each backend's
timelineSource; `timeline.synchronize=enabled` preserves the audio-sync switch.
Observed TextureView updates do not establish 120 Hz physical presentation.

## Input admission, ACK and result

```cpp
std::unique_ptr<InputChannel> open_input_channel(
    ClientCredentials, InputChannel::Events={}, InputChannel::Results={});
// Events: void(ConnectionState); Results: void(uint64_t,const InputResult&).
bool InputChannel::submit(Op, Bytes);
size_t InputChannel::pending() const;
uint64_t InputChannel::acknowledged() const;
ConnectionState InputChannel::state() const;
void InputChannel::close();

auto input = open_input_channel(credentials, onState, onResult);
bool admitted = input->submit(Op::input_focus, {}); // harmless example
```

InputResult contains code,message,payload. On Android:

```java
try (InputChannel input = new InputChannel(credentials,new InputChannel.Events() {
    public void state(InputChannel.State s) { /* post to UI */ }
    public void result(long seq,int code,String reason,byte[] payload) {
        // application errors are acknowledged too; show reason, do not replay here
    }
})) {
    boolean admitted = input.submit(40,new byte[0]);
}
```

Both implementations expose pending/acknowledged/state/close. The Java State
uses InputChannel.Phase and recoveringMillis; AV uses ConnectionStatus.Phase.
Callbacks run on the input worker and must not block. Internal heartbeats consume
ACK sequences but produce no business result callback.

Replace the old UI queues with these classes. A false submit is backpressure:
retain the pending critical click/key/scroll and retry in order when capacity
is available, instead of discarding it or closing control. Only tail mouse moves
coalesce. Do not bypass the channel with a second concurrent input path. Explicit
stop discards the owned session; ambiguity on an old host or expired replay
ledger is a disconnected state requiring deliberate restart, not blind replay.

## Android original and adapted movies

```java
MediaPlayback.Original original = MediaPlayback.openOriginal(context,credentials,path);
// original.uri and original.mime go to the external player, with read permission.
original.close();

MediaPlayback.Tracks tracks = MediaPlayback.query(credentials,path);
MediaPlayback.Adapted adapted = MediaPlayback.openAdapted(
    context,credentials,path,targetWidth,targetHeight,audioStreamIndex,assStreamIndex);
// adapted.uri is a random-token, loopback-only HLS URL. Keep adapted alive.
adapted.close();
MediaPlayback.closeAll();
```

Tracks exposes size,durationMillis,bitrate,version,width,height,hdr,tracks. Each
Track exposes index,kind (1 audio/2 subtitle),codec,language,title. Audio -1 means
default; subtitle -1 means off. Adapted target bounds are width 320..2560,
height 180..2160; aspect ratio is preserved. HDR and unsupported GPU source
codecs fail explicitly. URI close revokes original readers or stops HLS sockets,
in-flight requests/cache and the media foreground lease. The UI must retain the
handle across Activity recreation while the external player is still using it.
Choose original on LAN and adaptation on mobile data in the UI; the backend
does not silently choose a route or launch another app.

HLS v2 uses HEVC/hvc1 and AAC in fragmented MP4 with EXT-X-MAP. The platform
MediaPlayer failure is retained as a compatibility limit; do not promise all
third-party players accept the URL. The debug HLS HTTP/extractor/codec consumer
is validation only, not a restored production movie UI. Windows adaptation
playback is intentionally not exposed in this round.

## Endpoints and user-facing state

```cpp
EndpointConnection connect_endpoints(const std::vector<ConnectionEndpoint>&,
    const std::string& pin,
    std::chrono::milliseconds timeout=std::chrono::seconds(10),
    const std::optional<ConnectionEndpoint>& preferred={});
```

Java: `NetworkDialer.connect(List<ConnectionEndpoint>,String pin,int timeoutMillis,
Remote.SocketReady opening,ConnectionEndpoint preferred)`. Both retain the
per-certificate winning endpoint and 250 ms stagger; a single-endpoint probe does
not reorder remembered multi-endpoint preferences. Explicit preference does not
weaken pinning or allow an endpoint response to replace trust.

| State/error | Suggested UI treatment |
|---|---|
| NORMAL | Clear that channel's recovery indicator |
| RECOVERING, <1 second | Silent; keep input/audio toggles enabled |
| RECOVERING, longer | Nonmodal “Network interrupted, recovering…” plus elapsed time |
| DISCONNECTED after 30 seconds | Show disconnected; background AV can still recover |
| unauthorized, certificate mismatch, session ended | Stop automatic authorization reuse; request deliberate reconnect/pairing |
| conflict/expired input ledger | Explain input outcome is uncertain; start a new deliberate control session |
| constrained/dropping | Offer lower quality; retain the selected fixed size/FPS |
| UDP unavailable | Show TCP as current path; no failure dialog |
| source_changed | Reopen the movie after explaining the source changed |
| unsupported HDR/GPU codec/adaptation | Offer original playback or download |
| busy segment | Backend retries briefly; persistent contention can be shown nonmodally |

Public UDP setup/reachability, physical 120 Hz presentation and optical latency
remain separate acceptance items. No interface entry alone establishes them.
