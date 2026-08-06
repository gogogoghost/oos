# OOS System Media Service

OOS owns decoding, playback state, audio focus, and the physical output stream.
Applications use a managed package-asset player when OOS supports the format,
or submit signed interleaved S16 PCM after decoding a niche format themselves.
No guest receives AAudio, OpenSL ES, file-descriptor, or device-node access.

## Public Contract

The `audio` WIT interface provides:

- a truthful `supported-formats` list and explicit PCM rate/channel/queue limits;
- immutable guest-byte sources with explicit 16 MiB source, 32 MiB session,
  eight-source, and eight-player limits;
- asynchronous session-owned players with play, pause, seek, loop, volume,
  position/state, underrun count, and close;
- persistent bounded PCM streams with non-blocking partial writes, negotiated
  stream information, queued/consumed frames, pause/resume, flush, and close.

`source-create` performs one guest-to-host copy, sniffs bounded content when
legacy MIME or locator hints are missing/wrong, and stores the encoded bytes in
one reference-counted allocation. Multiple players share it, and closing the
source handle does not invalidate existing players. Status distinguishes
unsupported format, malformed data, resource exhaustion, I/O, and decoder
failure. Decoder thread exception boundaries convert allocation failure into a
normal failed-player state rather than terminating OOS.

Player decoding runs on host threads. Those threads never call guest code or
retain a guest pointer. Accepted PCM is copied before a WIT call returns. Direct
PCM streams and managed players both pause when their application loses audio
focus. Session destruction joins decoder threads, closes all PCM streams, and
releases their native queues even when guest shutdown traps.

The Nokia 2780 implementation uses one persistent AAudio stream per PCM handle.
The Nokia 8110 implementation uses a persistent four-buffer OpenSL ES queue and
software volume. Both implement the same bounded `PcmOutput` contract; the WIT
surface contains no device-specific branch.

## Supported Formats

| Family | MIME key | Backend |
|---|---|---|
| WAV PCM | `audio/wav` | miniaudio |
| MP3 | `audio/mpeg` | miniaudio |
| FLAC | `audio/flac` | miniaudio |
| SMF / SP-MIDI | `audio/midi`, `audio/sp-midi` | FluidLite + GeneralUser GS |
| Mobile XMF | `audio/mobile-xmf` | Sonivox |
| iMelody / RTTTL / Nokia OTA | legacy MIME keys | Sonivox |
| AAC, M4A/MP4, 3GP | `audio/aac`, `audio/mp4`, `audio/3gpp` | reduced FFmpeg |
| AMR-NB / AMR-WB | `audio/amr`, `audio/amr-wb` | reduced FFmpeg |
| Ogg Vorbis / Opus | codec-qualified `audio/ogg` keys | reduced FFmpeg |
| Raw Opus | `audio/opus` | reduced FFmpeg |

The GeneralUser GS 2.0.3 bank is compiled into the OOS executable as read-only
data. FluidLite maps its sample section in place, and all active MIDI players
share one parsed bank, avoiding a 31 MiB heap copy per player. Standard MIDI is
rendered as 44.1 kHz stereo with a 64-voice limit and linear interpolation.
Sonivox remains available for Mobile XMF and legacy phone ringtone formats.
FFmpeg is configured with only the listed audio decoders, parsers, demuxers and
the file protocol. It is built as shared LGPL libraries and packaged with its
license.

WMA and other proprietary/vendor formats are deliberately not advertised.
Applications must query the runtime list and use PCM fallback when a MIME key is
absent. Extensions are discovery hints, never the capability identifier.

## Build And Verification

`scripts/fetch-media-dependencies.sh` checks out and verifies pinned miniaudio,
FFmpeg, FluidLite, TinyMidiLoader, GeneralUser GS, and Sonivox revisions. It
also verifies the SoundFont SHA-256 before a build. GeneralUser GS is licensed
for software redistribution; FluidLite remains a separately packaged LGPL
shared library. `scripts/build-media-codecs.sh TARGET` creates isolated
`local`, `nokia-8110-4g`, and `nokia-2780-flip` FFmpeg trees. Resource packaging
copies and strips the four FFmpeg shared objects and includes all notices.

Host coverage synthesizes SMF and decodes WAV plus generated AAC, M4A, 3GP,
Vorbis, and Opus corpus files. WIT smoke coverage checks format discovery, PCM
negotiation, partial writes, status, pause/resume, flush, and close. Physical
device acceptance should additionally measure latency, underruns, PSS, decoder
CPU, and teardown recovery.
