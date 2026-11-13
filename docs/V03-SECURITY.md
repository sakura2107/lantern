# v0.3 security additions

This supplements [SECURITY.md](SECURITY.md) and describes implemented boundaries,
not an assertion that Internet reachability or weak-network acceptance passed.

## UDP threat model

An unpaired Internet sender must not receive media, allocate reassembly frames,
inject input, or turn the host into a reflector. Datagram admission checks the
fixed size/header and active random session identifier before cryptographic
processing; unknown sessions are silently discarded. Valid sessions still need
AES-256-GCM authentication, a direction-specific key/nonce, and replay checks.
Keys/session IDs are fresh per authenticated TLS media subscription. No key is
derived from a public certificate, pairing invitation or discoverable endpoint.
System CNG and Android JCA implement AES-GCM; no custom cipher primitive or QUIC
library is introduced.

The authenticated header includes session, packet/frame sequences, type,
fragment indexes/count/total and ciphertext length. Altering them fails the tag.
Forged high sequences cannot advance the replay window. A session cannot reuse
the packet sequence under its key. A reconnect creates fresh offers; no UDP
key is written to settings, a replay journal, logs or the repository.

Before media transmission, an authenticated HELLO receives one equally small
challenge; proof must return from the candidate endpoint within 1500 ms. Only
then is that endpoint verified and locked. Challenges are rate limited to one
per 250 ms. Unknown/unauthenticated packets cause no challenge or data response.
There are at most four sessions, a global 4096-packet/s admission bound, two
reassembly frames per receiver, a 200 ms retention window, 16 MiB frame ceiling,
1024-byte fragments and 128-entry NACK/retransmit admission limits. Media and
retransmission consume the same authenticated link scheduler budget.

UDP sessions share TLS owner identity, scope and cancellation lifetime. Trust
revocation, host session end and shutdown cancel transmission and erase active
offers. A transport failure cannot silently acquire a new authorization epoch.
After about 1500 ms without feedback the host emits a TLS path event and falls
back on that same TLS subscription. Reauthentication/protocol failures are
terminal, while ordinary transport failures may recover in the background.
UDP carries only encoded audio/video and feedback, never input or file writes.

Public/LAN firewall helpers support an explicit, program-bound rule for the
same UDP media port as TCP. They retain the Private-profile boundary and do
not enable Public/Domain profiles or edge traversal. Discovery UDP47442 is
separate. UPnP maps TCP and UDP separately with independently tracked leases;
reachability reports distinguish them. Rule/mapping creation requires its
existing explicit user action. Backend tests use mocks or owned sockets and do
not widen real firewall rules, change DNS/TUN or claim that a mapping proves
end-to-end public UDP reachability. A failed UDP route must remain usable via TLS.

## Input replay and recovery

Reliable input envelopes are restricted to input operations and control scope,
with random channel identity and a strictly increasing sequence. The host binds
the ledger to the authenticated token/epoch, rejects modified duplicate payloads
and gaps, and retains only bounded in-memory state for ten minutes. Internal
ten-second focus heartbeats refresh that lease; business callbacks never see
them. Input data is not persisted. Host restart/ledger eviction is not durable
exactly-once execution: conflict requires deliberate recovery, and old-host
ambiguous ACK loss stops replay to avoid a second click.

Permission and session-end failures do not become apparently successful
reconnections. Media recovery keeps trying after the 30-second disconnected
status but cannot resurrect a host-ended session. UI owners should display
state independently for video, audio and input; the backend shows no dialogs.

## File adaptation and local HTTP

Adaptation retains files_read scope, root confinement, reparse-point/path guards
and source-version checks. FFmpeg consumes an already authorized read-only
Windows handle through custom AVIO, never a shell command or an interpolated
filter path. Network protocols/programs/autodetection/GPL/nonfree are disabled.
GPU source dimensions, channels, sample rates, stream count, probe budget,
segment output, ASS/font caches, HLS cache and worker queues are bounded. One
host segment encodes at a time. Peer close/cancellation/authorization lifecycle
terminates conversion through FFmpeg interrupt callbacks. GPU decode and CPU
subtitle rendering still parse media inside the host process; this is not a
separate sandbox and is an additional parser attack surface.

Android binds the HLS listener only to 127.0.0.1 on an ephemeral port. URLs need
an unguessable 128-bit token, exact Host header and a restricted path grammar.
There is no directory browsing or remote listener, and HTTP headers/ranges have
bounds. It uses two workers/four queued requests and at most four cached
segments (each <4 MiB). Close stops listeners, sockets, remote requests and
cache; idle sessions expire after thirty minutes. The local token is a bearer
capability: another local app given the URL can use it until close. It does not
protect against a compromised phone or an app that captures an intentionally
shared URL. Tokens and media URLs must not appear in release telemetry.

The production manifest is unchanged. The standalone debug APK adds a debug
network-security resource permitting cleartext loopback only, for owned tests.
External players must themselves support loopback HTTP and HEVC/fMP4 HLS;
Android hardware HEVC support alone does not guarantee that player's HLS
demuxer. No other app is installed, launched or inspected by validation.

Do not ship debug instrumentation, TestDocuments or the generated debug
manifest as the production release. The debug and release signing processes
remain separate. The relink/source package contains no signing key, pairing
secret, DPAPI state, real endpoint report, or private test backup.
