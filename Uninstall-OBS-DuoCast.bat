@echo off
setlocal EnableExtensions
title OBS DuoCast uninstaller

rem Usage:
rem   Run from an OBS installation root, or drag an OBS root folder onto this file.
rem   The script removes only known DuoCast binaries and locale files. It keeps
rem   OBS settings and all other plugins intact.

if not "%~1"=="" (
	set "DUOCAST_ROOT=%~1"
) else (
	set "DUOCAST_ROOT=%~dp0"
)
if "%DUOCAST_ROOT:~-1%"=="\" set "DUOCAST_ROOT=%DUOCAST_ROOT:~0,-1%"

echo OBS DuoCast uninstaller
echo.
echo Target OBS folder: "%DUOCAST_ROOT%"
echo.

tasklist /FI "IMAGENAME eq obs64.exe" /NH 2>NUL | findstr /I /C:"obs64.exe" >NUL
if not errorlevel 1 goto obs_running
tasklist /FI "IMAGENAME eq obs32.exe" /NH 2>NUL | findstr /I /C:"obs32.exe" >NUL
if not errorlevel 1 goto obs_running

if not exist "%DUOCAST_ROOT%\." (
	echo ERROR: The target folder does not exist.
	echo Run this file from the OBS folder, or drag the OBS folder onto it.
	echo.
	pause
	exit /b 2
)

set "REMOVED_ANY=0"
call :RemoveFromRoot "%DUOCAST_ROOT%"

rem Also offer the system-wide new-layout location used by OBS 33+.
set "SYSTEM_PLUGIN_ROOT=%ProgramData%\obs-studio\plugins"
if /I not "%SYSTEM_PLUGIN_ROOT%"=="%DUOCAST_ROOT%\plugins" if exist "%SYSTEM_PLUGIN_ROOT%\OBS-DuoCast\OBS-DuoCast.dll" (
	echo.
	echo Found a system-wide DuoCast installation:
	echo   "%SYSTEM_PLUGIN_ROOT%\OBS-DuoCast"
	choice /C YN /N /M "Remove this system-wide DuoCast installation too? [Y/N] "
	if errorlevel 2 goto after_system_plugin
	call :RemoveNewLayout "%SYSTEM_PLUGIN_ROOT%"
)
:after_system_plugin

if "%REMOVED_ANY%"=="1" (
	echo.
	echo DuoCast files were removed.
) else (
	echo.
	echo No DuoCast plugin files were found in the target folder.
)
echo OBS plugin settings were kept. Other plugins were not touched.
echo Restart OBS to complete the uninstall.
echo.
pause
exit /b 0

:obs_running
echo ERROR: OBS is running. Close OBS Studio first, then run this uninstaller again.
echo This script will not terminate OBS automatically.
echo.
pause
exit /b 1

:RemoveFromRoot
set "ROOT=%~1"
call :RemoveNewLayout "%ROOT%\plugins"
call :RemoveFile "%ROOT%\obs-plugins\64bit\OBS-DuoCast.dll"
call :RemoveFile "%ROOT%\obs-plugins\64bit\turbojpeg.dll"
call :RemoveFile "%ROOT%\data\obs-plugins\OBS-DuoCast\locale\en-US.ini"

rem Remove directories only when they are empty; never recursively delete a folder.
2>NUL rmdir "%ROOT%\obs-plugins\64bit"
2>NUL rmdir "%ROOT%\obs-plugins"
2>NUL rmdir "%ROOT%\data\obs-plugins\OBS-DuoCast\locale"
2>NUL rmdir "%ROOT%\data\obs-plugins\OBS-DuoCast"
2>NUL rmdir "%ROOT%\data\obs-plugins"
2>NUL rmdir "%ROOT%\data"
exit /b 0

:RemoveNewLayout
set "PLUGIN_ROOT=%~1\OBS-DuoCast"
call :RemoveFile "%PLUGIN_ROOT%\OBS-DuoCast.dll"
call :RemoveFile "%PLUGIN_ROOT%\turbojpeg.dll"
call :RemoveFile "%PLUGIN_ROOT%\data\locale\en-US.ini"
2>NUL rmdir "%PLUGIN_ROOT%\data\locale"
2>NUL rmdir "%PLUGIN_ROOT%\data"
2>NUL rmdir "%PLUGIN_ROOT%"
exit /b 0

:RemoveFile
if exist "%~1" (
	del /F /Q "%~1" >NUL
	if not errorlevel 1 (
		echo Removed: "%~1"
		set "REMOVED_ANY=1"
	) else (
		echo Could not remove: "%~1"
	)
)
exit /b 0
