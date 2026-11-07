# OBS DuoCast

**Current development build:** 0.14.37 Beta for Windows x64 / OBS Studio 32.2.2.
**Fallback build:** 0.14.13 Stable remains available if you need to roll back.

The 0.14.37 build continues from the 0.14.36 Beta checkpoint. MJPEG remains the
default, with optional texture-passed NVIDIA NVENC, Intel QuickSync, and AMD AMF
H.264 and HEVC/H.265 sender modes. Hardware bitrate is configurable per feed.
The receiver offers Auto or an available FFmpeg hardware decoder for H.264 and
HEVC, with software fallback. AAC audio transport is unchanged.

OBS DuoCast relays the composed OBS program output and two selected OBS audio
tracks over a timestamped TCP connection. The receiving PC does not recreate
the sender's scenes or audio mixes. The sender can use OBS Base (Canvas) or
Output (Scaled) resolution. Optional portrait video uses Aitum Vertical's
independent canvas.

## Install

Use `dist/OBS-DuoCast-0.14.37-Beta-Windows-x64.zip` for the sender and receiver with MJPEG, NVENC/QuickSync/AMF H.264 or HEVC video, receiver decoder selection (including Intel and AMD GPU D3D11VA), a live active decoder indicator, output-resolution selection, Kbps audio stat cards, receiver sync buffering, and automatic low-FPS recovery.
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
`192.168.50.102`, port `17777`. Video defaults to TurboJPEG MJPEG; NVIDIA
NVENC, Intel QuickSync, and AMD AMF H.264 and HEVC/H.265 are optional hardware
encoders, shown only when registered by OBS on that PC. The JPEG quality slider applies only
to MJPEG. Hardware encoders use CBR and the selected bitrate is the target for each feed. The
HEVC hardware modes can optionally encode Main10/P010 10-bit video when OBS,
the sender GPU/driver, and receiver support it. This preserves 10-bit frames
through DuoCast; the receiver's final OBS/Twitch output may still be 8-bit.
Stop and restart sending after changing the 10-bit option.
separate total network cap paces all video feeds and both audio tracks. With
portrait enabled, account for two hardware video targets when setting the shared
cap. The sender UI shows the combined target and warns when the cap will throttle
frames. Set the cap to 0 to disable network pacing. The output-resolution
option can reduce encoding work when OBS's scaled output is smaller than its
canvas.
The optional max FPS
cap applies to both feeds; sender stats show the actual outgoing FPS. Optional
portrait sending requires Aitum Vertical installed and configured on the sender
PC. Install 0.14.37 on both PCs for matching sender and receiver behavior. The
receiver supports MJPEG, H.264, and HEVC.

The sender also provides a **DuoCast Receiver Stats** dock. It receives the
streaming PC's OBS performance/output statistics over the existing TCP session,
alongside the sender's DuoCast feed rates. No additional port is required.

For quick access, enable **DuoCast Sender Controls** from **Docks**. Start and
Stop sit side by side on the first row; Refresh Feed and the settings gear are
on the second row when the dock is narrow and tall. When the dock is wide or
short, all controls fit on one row. Refresh Feed restarts the sender connection
to flush and resync its audio/video timeline.

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
For H.264 and HEVC, the receiver attempts D3D11 hardware decoding first and
falls back to software decoding if the local FFmpeg runtime or GPU cannot use
it. Decoded frames are copied back to system memory for OBS source output.

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

The 0.14.37 Beta sends TurboJPEG MJPEG or OBS texture-passed NVENC, QuickSync,
or AMF H.264/HEVC and can use either OBS canvas or scaled output dimensions.
Hardware modes use the OBS GPU texture path, with B-frames disabled so video
decode and audio stay on the shared sender system-time timeline. The receiver
can select Auto or any probed FFmpeg hardware decoder, and falls back to software
if hardware initialization fails. DuoCast keeps its timestamped TCP transport
and AAC audio mixes; FFmpeg on the receiver decodes AAC, H.264, and HEVC.
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
