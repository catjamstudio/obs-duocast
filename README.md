# OBS DuoCast

**Current development build:** 0.14.25 Beta for Windows x64 / OBS Studio 32.2.2.
**Fallback build:** 0.14.13 Stable remains available if you need to roll back.

The 0.14.25 build follows the 0.14.13 Stable MJPEG behavior and adds the
sender's output-resolution option, Kbps audio stat cards, an adjustable
receiver A/V sync buffer, and automatic low-FPS feed recovery. NVENC and the
experimental PTS-DTS offset change are excluded.

OBS DuoCast relays the composed OBS program output and two selected OBS audio
tracks over a timestamped TCP connection. The receiving PC does not recreate
the sender's scenes or audio mixes. The sender can use OBS Base (Canvas) or
Output (Scaled) resolution. Optional portrait video uses Aitum Vertical's
independent canvas.

## Install

Use `dist/OBS-DuoCast-0.14.25-Beta-Windows-x64.zip` for the MJPEG-only sender and receiver with the output-resolution option, Kbps audio stat cards, receiver sync buffering, automatic low-FPS recovery and fixed TurboJPEG quality with independent bitrate pacing.
Use `dist/OBS-DuoCast-0.14.13-Stable-Windows-x64.zip` to roll back. Close OBS before installing.
The ZIP contains both OBS's new per-plugin layout and the legacy plugin folders.
For OBS 33.0+ Portable, extract it into the portable root; for a standard OBS
33.0+ install, copy `plugins/OBS-DuoCast` to
`%ProgramData%/obs-studio/plugins/`. For older OBS versions, copy the included
`obs-plugins` and `data` folders into the OBS installation directory. OBS 33.0+
tries the new layout first and skips the legacy duplicate if DuoCast loads from
the new location. Install only one layout per OBS installation. The same plugin
package is installed on sender and receiver PCs.

## Sender

Open **Tools → DuoCast**. Turn on **Enable DuoCast Sender**, select the live mix
and music-free VOD tracks, then start sending. Sender and receiver have
independent enable switches, both off by default. The sender address defaults to
`192.168.50.102`, port `17777`. Video uses TurboJPEG MJPEG. The selected JPEG
quality is applied to every frame. A bitrate cap paces TCP writes independently
of image quality; stale queued video frames are dropped if the network falls
behind, so a cap below the sustained feed rate can reduce delivered FPS. The
output-resolution option can reduce compression work
when OBS's scaled output is smaller than its canvas.
The optional max FPS
cap applies to both feeds; sender stats show the actual outgoing FPS. Optional
portrait sending requires Aitum Vertical installed and configured on the sender
PC. Install 0.14.25 on both PCs for matching sender and receiver behavior. The
sender emits MJPEG; the receiver retains legacy H.264 decode compatibility.

The sender also provides a **DuoCast Receiver Stats** dock. It receives the
streaming PC's OBS performance/output statistics over the existing TCP session,
alongside the sender's DuoCast feed rates. No additional port is required.

For quick access, enable **DuoCast Sender Controls** from **Docks**. It
contains only Start Sending and Stop Sending buttons and uses the saved sender
configuration.

## Receiver

Allow inbound TCP port `17777` through Windows Firewall, turn on **Enable
DuoCast Receiver** in the tool, then add these OBS sources:

- **DuoCast Video Source**: landscape video and live mix audio.
- **DuoCast Vertical Video Source**: optional portrait video and live mix audio.
- **DuoCast Audio VOD Track Mix Source**: selected music-free mix.

The sources share one receiver session and port. In OBS Advanced Audio
Properties, route the live source to the Twitch stream track and the VOD source
to Twitch's VOD track. Avoid routing both video sources' live audio together to
prevent duplicate audio.

The Receiver tab has an **A/V sync buffer** setting from 0 to 3000 ms, defaulting
to 1000 ms. It holds compressed packets and releases all received audio and
video streams against the same sender timestamp timeline. The selected value is
added as receiver latency and takes effect on the next sender connection. The
queue is bounded at 256 MiB to prevent unbounded memory growth during a stalled
receiver. This fixed playout delay absorbs jitter; it does not alter sender
timestamps or guarantee correction for sustained packet loss or a receiver that
cannot decode fast enough.

Automatic low-FPS recovery is enabled by default and can be switched off in
Receiver settings. After startup warm-up, if decoded FPS remains below 60% of
the configured stream rate for five consecutive seconds, the receiver requests
a sender transport reconnect over the existing TCP connection. The sender
reconnects while keeping OBS capture active, clearing stale network queues.
Recovery rearms after ten seconds of healthy video to avoid reconnect loops.
The sender's configured FPS cap is included in the expected-rate threshold.
The Receiver tab has separate fallback scenes for landscape and portrait. The
portrait dropdown lists Aitum Vertical canvas scenes and switches that canvas
when its feed is lost. DuoCast returns to the previous scene on that canvas
when the feed recovers. Install Aitum Vertical on the receiver to use portrait
fallback scenes.

## Transport and build

The 0.14.25 Beta sends TurboJPEG MJPEG and can use either OBS canvas or scaled
output dimensions. The sender compresses OBS's I420 planes directly, avoiding
an extra FFmpeg MJPEG conversion step. DuoCast keeps its timestamped TCP
transport and AAC audio mixes; FFmpeg on the receiver is used for AAC decoding.
The sender maps OBS system timestamps to its shared timeline.
Sender and receiver reconnect over TCP. The transport has no encryption or
authentication; use it on a trusted LAN. FFmpeg DLLs are provided by OBS.

Build and install this source against the bundled OBS 32.2.2 runtime with
`powershell -ExecutionPolicy Bypass -File .\Build-OBSDuoCast.ps1`. Create the
ZIP with `powershell -ExecutionPolicy Bypass -File .\Package-OBSDuoCast.ps1`.
Use `-BuildOnly` to compile without changing the portable OBS installation.

The exact 0.13.0 Stable package remains preserved at
`dist/OBS-DuoCast-0.13.0-Stable-Windows-x64.zip`; its extracted DLL is also
preserved in `build/baseline-0.13.0` as a historical reference.

The 0.13.12 Stable Beta package remains preserved at
`dist/OBS-DuoCast-0.13.12-Stable-Beta-Windows-x64.zip` as the FFmpeg MJPEG build.
