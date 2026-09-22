# Audio Duplicate

Portable Windows utility for duplicating the audio playing on one output device to one or more additional output devices.

## Features

- WASAPI Loopback capture from the selected main output.
- Any number of additional outputs.
- Per-device channel routing: Stereo / Left / Right.
- Scrollable list of additional outputs.
- Global device refresh button.
- Russian and English UI.
- Optional settings file stored only beside `AudioDuplicate.exe`.
- No installer, service, driver, virtual audio device, registry settings, network access, telemetry, or autostart.

## Portable behavior

The application does not require installation. If **Save settings beside EXE** is disabled, Audio Duplicate does not create its own persistent files. If enabled, it creates only `AudioDuplicate.ini` in the same directory as the executable.

## Build

Requirements:

- Windows
- Visual Studio 2022 / Build Tools with .NET Framework 4.8 targeting pack
- MSBuild

Build:

```powershell
msbuild AudioDuplicate.csproj /t:Restore
msbuild AudioDuplicate.csproj /p:Configuration=Release /p:Platform=x64 /m
```

Output:

```
bin/x64/Release/net48/AudioDuplicate.exe
```

Releases are built on GitHub Actions from the public source tree.
