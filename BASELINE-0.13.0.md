# DuoCast 0.13.0 baseline reset

The restored portable OBS plugin and the released 0.13.0 package are the
authoritative runnable baseline. Their DLLs were SHA-256 checked and match:

`779C2E4F6420443E16672DB3B413953F10A061BF1B3BA50711C5B2C6044616B5`

The original 0.13.0 package is preserved at
`dist/OBS-DuoCast-0.13.0-Stable-Windows-x64.zip`, and an extracted reference is
at `build/baseline-0.13.0`. The later MJPEG rework package is archived under
`build/rework-archives`; the editable files in `src` are still that later
rework, because the 0.13.0 archive contains no source snapshot.

The editable source has since evolved through 0.13.12 Stable Beta using FFmpeg
MJPEG. Version 0.14.0 Beta is the TurboJPEG development line. Do not label the
MJPEG rework as 0.13.0.
