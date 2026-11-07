param([switch]$BuildOnly)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$versionSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\plugin.cpp')
if (-not $BuildOnly -and $versionSource -notmatch 'OBS DuoCast %s \(Beta\) loaded", "0\.14\.38"') {
	throw 'Build stopped: source version marker must be OBS DuoCast 0.14.38 Beta before installing.'
}
$obsRoot = Join-Path $projectRoot 'obs_portable'
$obsExe = Join-Path $obsRoot 'bin\64bit\obs64.exe'
$sdkRoot = Join-Path $projectRoot '.deps\obs-studio-32.2.2'
$ffmpegRoot = Join-Path $projectRoot '.deps\FFmpeg-n8.0'
$turboJpegRoot = Join-Path $projectRoot '.deps\libjpeg-turbo-3.2.0\sdk'
$qtRoot = Join-Path $projectRoot '.deps\qt610\6.10.1\msvc2022_64'
$libobsHeaders = Join-Path $sdkRoot 'libobs'
$vsRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$toolRoot = Join-Path $vsRoot 'VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64'
$vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$buildRoot = Join-Path $projectRoot 'build'
$outputRoot = if ($BuildOnly) { Join-Path $buildRoot 'hw-0.14.38-source-build' } else { $buildRoot }

if (-not (Test-Path $obsExe)) { throw "Portable OBS not found: $obsExe" }
if (-not (Test-Path (Join-Path $libobsHeaders 'obs-module.h'))) { throw "OBS SDK headers not found: $libobsHeaders" }
if (-not (Test-Path (Join-Path $toolRoot 'cl.exe'))) { throw 'Visual Studio C++ compiler not found.' }
if (-not (Test-Path $vsDevCmd)) { throw 'Visual Studio developer environment script not found.' }
if (-not (Test-Path (Join-Path $turboJpegRoot 'include\turbojpeg.h'))) { throw 'TurboJPEG 3.2.0 headers not found.' }
if (-not (Test-Path (Join-Path $turboJpegRoot 'lib\turbojpeg.lib'))) { throw 'TurboJPEG 3.2.0 import library not found.' }
if (-not (Test-Path (Join-Path $turboJpegRoot 'bin\turbojpeg.dll'))) { throw 'TurboJPEG 3.2.0 runtime DLL not found.' }

$obsVersion = (Get-Item $obsExe).VersionInfo.ProductVersion
if ($obsVersion -ne '32.2.2') { throw "This local SDK snapshot is for OBS 32.2.2, but portable OBS is $obsVersion." }

New-Item -ItemType Directory -Force -Path $buildRoot, $outputRoot | Out-Null
$exportDump = Join-Path $buildRoot 'obs-exports.txt'
$definition = Join-Path $buildRoot 'obs.def'
$importLibrary = Join-Path $buildRoot 'obs.lib'
$pluginDll = Join-Path $outputRoot 'OBS-DuoCast-TurboJPEG.dll'
$pluginImportLibrary = Join-Path $outputRoot 'OBS-DuoCast-TurboJPEG.lib'
$obsDll = Join-Path $obsRoot 'bin\64bit\obs.dll'
$frontendDll = Join-Path $obsRoot 'bin\64bit\obs-frontend-api.dll'
$generatedInclude = Join-Path $buildRoot 'include'
New-Item -ItemType Directory -Force -Path $generatedInclude | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $generatedInclude 'libavutil') | Out-Null
@'
#pragma once
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
'@ | Set-Content -Path (Join-Path $generatedInclude 'libavutil\avconfig.h') -Encoding Ascii
@'
#pragma once
#define OBS_DATA_PATH "data/obs-studio"
#define OBS_PLUGIN_PATH "obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"
#define OBS_INSTALL_PREFIX "."
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@ | Set-Content -Path (Join-Path $generatedInclude 'obsconfig.h') -Encoding Ascii

& (Join-Path $toolRoot 'dumpbin.exe') /nologo /exports $obsDll | Set-Content -Encoding Ascii $exportDump
if ($LASTEXITCODE -ne 0) { throw 'Could not read exports from the portable OBS runtime.' }
$exports = Get-Content $exportDump | ForEach-Object {
	if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_?$@.]*)(?:\s*=.*)?$') { $Matches[1] }
} | Sort-Object -Unique
if (-not $exports) { throw 'No named libobs exports were found.' }
@('LIBRARY obs.dll', 'EXPORTS') + $exports | Set-Content -Encoding Ascii $definition

$frontendExportDump = Join-Path $buildRoot 'obs-frontend-exports.txt'
$frontendDefinition = Join-Path $buildRoot 'obs-frontend-api.def'
$frontendImportLibrary = Join-Path $buildRoot 'obs-frontend-api.lib'
& (Join-Path $toolRoot 'dumpbin.exe') /nologo /exports $frontendDll | Set-Content -Encoding Ascii $frontendExportDump
if ($LASTEXITCODE -ne 0) { throw 'Could not read exports from the portable OBS frontend API runtime.' }
$frontendExports = Get-Content $frontendExportDump | ForEach-Object {
	if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_?$@.]*)(?:\s*=.*)?$') { $Matches[1] }
} | Sort-Object -Unique
if (-not $frontendExports) { throw 'No named frontend API exports were found.' }
@('LIBRARY obs-frontend-api.dll', 'EXPORTS') + $frontendExports | Set-Content -Encoding Ascii $frontendDefinition

$ffmpegLibraries = @(
	@{ Name = 'avcodec'; Dll = 'avcodec-62.dll' },
	@{ Name = 'avutil'; Dll = 'avutil-60.dll' },
	@{ Name = 'swresample'; Dll = 'swresample-6.dll' })
foreach ($entry in $ffmpegLibraries) {
	$dllPath = Join-Path $obsRoot ('bin\64bit\' + $entry.Dll)
	$dumpPath = Join-Path $buildRoot ($entry.Name + '-exports.txt')
	$defPath = Join-Path $buildRoot ($entry.Name + '.def')
	$libPath = Join-Path $buildRoot ($entry.Name + '.lib')
	& (Join-Path $toolRoot 'dumpbin.exe') /nologo /exports $dllPath | Set-Content -Encoding Ascii $dumpPath
	if ($LASTEXITCODE -ne 0) { throw "Could not inspect portable FFmpeg library $($entry.Dll)." }
	$ffExports = Get-Content $dumpPath | ForEach-Object {
		if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_?$@.]*)(?:\s*=.*)?$') { $Matches[1] }
	} | Sort-Object -Unique
	if (-not $ffExports) { throw "No named exports found in $($entry.Dll)." }
	@("LIBRARY $($entry.Dll)", 'EXPORTS') + $ffExports | Set-Content -Encoding Ascii $defPath
}

$includeDirs = @(
	(Join-Path $qtRoot 'include'),
	(Join-Path $qtRoot 'include\QtCore'),
	(Join-Path $qtRoot 'include\QtGui'),
	(Join-Path $qtRoot 'include\QtWidgets'),
	$libobsHeaders,
	(Join-Path $libobsHeaders 'graphics'),
	(Join-Path $libobsHeaders 'media-io'),
	(Join-Path $libobsHeaders 'util'),
	(Join-Path $libobsHeaders 'callback'),
	(Join-Path $sdkRoot 'frontend\api'),
	$ffmpegRoot,
	(Join-Path $turboJpegRoot 'include'),
	$generatedInclude) | ForEach-Object { '/I"' + $_ + '"' }
$sources = @(
	'src\plugin.cpp',
	'src\protocol.cpp',
	'src\sender_output.cpp',
	'src\receiver_source.cpp') -join ' '
$includeArgs = $includeDirs -join ' '
$commandFile = Join-Path $buildRoot 'compile-obs-duocast.cmd'
$command = @"
@echo off
call "$vsDevCmd" -no_logo -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cd /d "$projectRoot"
"$($toolRoot)\lib.exe" /nologo /def:"$definition" /out:"$importLibrary" /machine:x64
if errorlevel 1 exit /b %errorlevel%
"$($toolRoot)\lib.exe" /nologo /def:"$frontendDefinition" /out:"$frontendImportLibrary" /machine:x64
if errorlevel 1 exit /b %errorlevel%
"$($toolRoot)\lib.exe" /nologo /def:"$buildRoot\avcodec.def" /out:"$buildRoot\avcodec.lib" /machine:x64
if errorlevel 1 exit /b %errorlevel%
"$($toolRoot)\lib.exe" /nologo /def:"$buildRoot\avutil.def" /out:"$buildRoot\avutil.lib" /machine:x64
if errorlevel 1 exit /b %errorlevel%
"$($toolRoot)\lib.exe" /nologo /def:"$buildRoot\swresample.def" /out:"$buildRoot\swresample.lib" /machine:x64
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++20 /Zc:__cplusplus /EHsc /MD /O2 /LD /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DQT_SHARED /Fo"$outputRoot\\" /Fd"$outputRoot\\OBS-DuoCast-TurboJPEG.pdb" $includeArgs $sources /link /OUT:"$pluginDll" /IMPLIB:"$pluginImportLibrary" "$importLibrary" "$frontendImportLibrary" "$buildRoot\avcodec.lib" "$buildRoot\avutil.lib" "$buildRoot\swresample.lib" "$turboJpegRoot\lib\turbojpeg.lib" "$qtRoot\lib\Qt6Widgets.lib" "$qtRoot\lib\Qt6Gui.lib" "$qtRoot\lib\Qt6Core.lib" ws2_32.lib
exit /b %errorlevel%
"@
Set-Content -Path $commandFile -Value $command -Encoding Ascii

& $env:ComSpec /d /c "`"$commandFile`""
if ($LASTEXITCODE -ne 0) { throw "OBS DuoCast compilation failed with exit code $LASTEXITCODE." }

if ($BuildOnly) {
	Write-Output "Built current source for OBS ${obsVersion} without installing it: $pluginDll"
} else {
	$pluginDir = Join-Path $obsRoot 'obs-plugins\64bit'
	$localeDir = Join-Path $obsRoot 'data\obs-plugins\OBS-DuoCast\locale'
	New-Item -ItemType Directory -Force -Path $pluginDir, $localeDir | Out-Null
	Copy-Item -LiteralPath $pluginDll -Destination (Join-Path $pluginDir 'OBS-DuoCast.dll') -Force
	Copy-Item -LiteralPath (Join-Path $turboJpegRoot 'bin\turbojpeg.dll') -Destination (Join-Path $pluginDir 'turbojpeg.dll') -Force
	Copy-Item -LiteralPath (Join-Path $projectRoot 'data\locale\en-US.ini') -Destination (Join-Path $localeDir 'en-US.ini') -Force
	Write-Output "Built and installed OBS DuoCast 0.14.38 Beta for portable OBS ${obsVersion}: $pluginDir\OBS-DuoCast.dll"
}
