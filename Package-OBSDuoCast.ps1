$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $projectRoot 'build'
$builtDll = Join-Path $buildRoot 'OBS-DuoCast-TurboJPEG.dll'
$turboJpegDll = Join-Path $projectRoot '.deps\libjpeg-turbo-3.2.0\sdk\bin\turbojpeg.dll'
$turboJpegLicense = Join-Path $projectRoot '.deps\libjpeg-turbo-3.2.0\sdk\doc\LICENSE.md'
$turboJpegIjgLicense = Join-Path $projectRoot '.deps\libjpeg-turbo-3.2.0\sdk\doc\README.ijg'
$localeSource = Join-Path $projectRoot 'data\locale\en-US.ini'
$versionSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\plugin.cpp')
if ($versionSource -notmatch 'OBS DuoCast %s \(Beta\) loaded", "0\.14\.25"') {
	throw 'Packaging stopped: source version marker must be OBS DuoCast 0.14.25 Beta.'
}
$version = '0.14.25'
$releaseLabel = 'Beta'
$packageName = "OBS-DuoCast-$version-$releaseLabel-Windows-x64"
$staging = Join-Path $buildRoot "package-$version"
$dist = Join-Path $projectRoot 'dist'
$zipPath = Join-Path $dist "$packageName.zip"

if (-not (Test-Path -LiteralPath $builtDll)) { throw "Built plugin DLL not found: $builtDll. Run Build-OBSDuoCast.ps1 -BuildOnly first." }
if (-not (Test-Path -LiteralPath $turboJpegDll)) { throw "TurboJPEG runtime DLL not found: $turboJpegDll" }
if (-not (Test-Path -LiteralPath $turboJpegLicense)) { throw "TurboJPEG license file not found: $turboJpegLicense" }
if (-not (Test-Path -LiteralPath $turboJpegIjgLicense)) { throw "TurboJPEG IJG license file not found: $turboJpegIjgLicense" }
if (-not (Test-Path -LiteralPath $localeSource)) { throw "Locale file not found: $localeSource" }

$resolvedBuild = [System.IO.Path]::GetFullPath($buildRoot).TrimEnd('\') + '\'
$resolvedStaging = [System.IO.Path]::GetFullPath($staging)
if (-not $resolvedStaging.StartsWith($resolvedBuild, [System.StringComparison]::OrdinalIgnoreCase)) {
	throw "Refusing to use staging directory outside build folder: $resolvedStaging"
}
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Force -Path `
	(Join-Path $staging 'plugins\OBS-DuoCast\data\locale'), `
	(Join-Path $staging 'obs-plugins\64bit'), `
	(Join-Path $staging 'data\obs-plugins\OBS-DuoCast\locale'), `
	(Join-Path $staging 'license'), `
	$dist | Out-Null

Copy-Item -LiteralPath $builtDll -Destination (Join-Path $staging 'plugins\OBS-DuoCast\OBS-DuoCast.dll')
Copy-Item -LiteralPath $turboJpegDll -Destination (Join-Path $staging 'plugins\OBS-DuoCast\turbojpeg.dll')
Copy-Item -LiteralPath $localeSource -Destination (Join-Path $staging 'plugins\OBS-DuoCast\data\locale\en-US.ini')
Copy-Item -LiteralPath $builtDll -Destination (Join-Path $staging 'obs-plugins\64bit\OBS-DuoCast.dll')
Copy-Item -LiteralPath $turboJpegDll -Destination (Join-Path $staging 'obs-plugins\64bit\turbojpeg.dll')
Copy-Item -LiteralPath $localeSource -Destination (Join-Path $staging 'data\obs-plugins\OBS-DuoCast\locale\en-US.ini')
Copy-Item -LiteralPath $turboJpegLicense -Destination (Join-Path $staging 'license\LICENSE-libjpeg-turbo.md')
Copy-Item -LiteralPath $turboJpegIjgLicense -Destination (Join-Path $staging 'license\LICENSE-libjpeg-turbo-README.ijg')
Copy-Item -LiteralPath (Join-Path $projectRoot 'Uninstall-OBS-DuoCast.bat') -Destination (Join-Path $staging 'Uninstall-OBS-DuoCast.bat')

$installInstructions = @"
OBS DuoCast $version for Windows x64 (Beta)

Build basis: the 0.14.13 Stable MJPEG sender/receiver behavior, with the sender's
OBS output-resolution option, Kbps audio stat cards, and an adjustable receiver
A/V sync buffer. The sender remains MJPEG-only; legacy H.264 is receive-only.

Requires OBS Studio 32.2.2 x64, matching the runtime used to build this plugin.

1. Close OBS Studio before installing.
2. Choose the installation layout for your OBS version:
   - OBS Studio 33.0 or newer, Portable: extract the archive into the portable
     OBS root. The included plugins folder installs DuoCast in the new layout.
   - OBS Studio 33.0 or newer, standard install: copy the included
     plugins\OBS-DuoCast folder to %ProgramData%\obs-studio\plugins\ so the
     module is at %ProgramData%\obs-studio\plugins\OBS-DuoCast\OBS-DuoCast.dll.
   - Older OBS versions: copy the included obs-plugins and data folders into
     the OBS installation folder to use the legacy layout.
   OBS 33.0+ tries the new layout first and skips the same plugin in the legacy
   layout if the new module loads successfully. Install only one layout per
   OBS installation to avoid stale duplicate copies.
3. Start OBS and open Tools > DuoCast to use the Sender and Receiver tabs.
4. On the receiving PC, add DuoCast Video Source and DuoCast Audio VOD Track
   Mix Source. The Video Source includes video and the selected live mix audio.
   Use port 17777 for both sources.
5. To receive the optional portrait composition, add DuoCast Vertical Video
   Source. It includes the same live mix audio. Install Aitum Vertical on both
   PCs if you need to send portrait video and configure a vertical canvas on the
   sender. The receiver uses its Aitum Vertical canvas scenes for the portrait
   fallback selector. Vertical sending adds a second MJPEG feed and uses extra
   CPU and network resources; leave it off when the portrait feed is not needed.
6. Allow inbound TCP port 17777 through Windows Firewall on the receiving PC.
7. In OBS Advanced Audio Properties on the receiver, route Video Source audio
   to the stream track and VOD Track Mix Source audio to the Twitch VOD track.
   If using the vertical source in a scene instead, route its live audio there
   and avoid adding both video sources to the same audio mix to prevent doubling.

The Video Source carries video and live mix audio. There is no separate live
audio source. Remove any older DuoCast Audio Source 1 source from existing OBS
scenes to avoid duplicate audio.

The Receiver tab has an A/V sync buffer adjustable from 0 to 3000 ms, with
1000 ms as the default. It queues compressed packets and releases the audio and
video feeds using their shared sender timestamps before decoding. This adds the
selected delay to receiver output; changes take effect when the sender reconnects.
The queue is bounded to 256 MiB.

Automatic low-FPS recovery is enabled by default. If decoded video remains below
60% of its configured FPS for five seconds, the receiver asks the sender to
reconnect the transport. It does not stop OBS capture; it rearms after the feed
stays healthy for ten seconds. Disable it in Receiver settings if you prefer
manual recovery.
The Receiver tab can choose an Aitum Vertical scene for loss of the vertical
feed and a regular OBS scene for loss of the landscape feed. Each fallback
returns to the previous scene in its own canvas when that feed recovers.

On the sender PC, the default receiver address is 192.168.50.102. Change it in
Tools > DuoCast if the receiver PC uses a different address.
The sender's autostart option is enabled by default. Start the sender once to
save its destination and track selections; subsequent OBS launches start it
automatically when Enable DuoCast Sender is on. Enable DuoCast Sender and Enable
DuoCast Receiver are separate switches and both default to off.

The sender uses TurboJPEG MJPEG for video. Max total bitrate defaults to
100 Mbps and caps the combined TCP output for video and audio. Set it to 0
(Auto) to disable the cap. The selected JPEG quality is fixed for every frame,
matching Teleport's fixed-quality behavior. A numeric cap paces TCP output
without changing JPEG quality. If the network falls behind, stale queued video
frames are dropped; a cap that is too low for the selected quality and frame
rate can reduce delivered FPS. Max video FPS defaults to Canvas FPS
(no cap); frames above the selected cap are skipped before JPEG compression.
The video resolution can use either OBS canvas dimensions or OBS output dimensions.

The Docks menu includes DuoCast Sender Controls, a compact dock with
Start Sending and Stop Sending buttons. The buttons reflect sender state and
use the saved Sender settings.

This build sends TurboJPEG MJPEG video and AAC audio. The receiver also retains
legacy H.264 decoding compatibility; the sender does not emit H.264. FFmpeg
libraries supplied by OBS decode AAC and H.264 on the receiver. DuoCast writes settings to the OBS
plugin configuration folder. That per-PC settings file is created on first save
and should not be copied between the sender and receiver installations.
Install this version on both PCs for matching protocol behavior. The 0.14.13
Stable receiver supports MJPEG and legacy H.264 decoding.

The archive includes both the new per-plugin layout and the legacy obs-plugins
and data layout, with TurboJPEG's runtime DLL beside the module in each layout.
Run Uninstall-OBS-DuoCast.bat from the OBS root, or drag an OBS root folder onto
it, to remove DuoCast files while keeping OBS settings and other plugins.
FFmpeg libraries are provided by the matching OBS installation. TurboJPEG's
licenses are included in the license folder.
"@
Set-Content -LiteralPath (Join-Path $staging 'INSTALL.txt') -Value $installInstructions -Encoding utf8

if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zipPath -CompressionLevel Optimal
Remove-Item -LiteralPath $staging -Recurse -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
	$files = @($archive.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) } |
		ForEach-Object { $_.FullName.Replace('/', '\') } | Sort-Object)
	$expected = @(
		'plugins\OBS-DuoCast\OBS-DuoCast.dll',
		'plugins\OBS-DuoCast\turbojpeg.dll',
		'plugins\OBS-DuoCast\data\locale\en-US.ini',
		'data\obs-plugins\OBS-DuoCast\locale\en-US.ini',
		'license\LICENSE-libjpeg-turbo.md',
		'license\LICENSE-libjpeg-turbo-README.ijg',
		'Uninstall-OBS-DuoCast.bat',
		'INSTALL.txt',
		'obs-plugins\64bit\OBS-DuoCast.dll',
		'obs-plugins\64bit\turbojpeg.dll') | Sort-Object
	if (Compare-Object -ReferenceObject $expected -DifferenceObject $files) {
		throw "Package contents do not match the expected DuoCast files: $($files -join ', ')"
	}
	$dllEntry = $archive.Entries | Where-Object {
		$_.FullName.Replace('/', '\') -eq 'obs-plugins\64bit\OBS-DuoCast.dll'
	} | Select-Object -First 1
	if (-not $dllEntry -or $dllEntry.Length -lt 4096) { throw 'Packaged DuoCast module is missing or unexpectedly small.' }
} finally {
	$archive.Dispose()
}

Write-Output "Created $zipPath"
