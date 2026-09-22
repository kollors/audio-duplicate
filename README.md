# Audio Duplicate

Portable Windows utility for duplicating the audio mix of one render endpoint to multiple additional render endpoints via WASAPI loopback.

## Current behavior

- Native Win32/C++ application.
- No installer, service, driver, virtual audio device, registry configuration, AppData, ProgramData, or temp files are used by the application.
- Runtime creates no files unless **Save settings next to EXE** is enabled.
- When settings saving is enabled, the only file created by the app is `AudioDuplicate.cfg` next to `AudioDuplicate.exe`.
- Main output is captured with WASAPI loopback in shared mode.
- Any number of additional output rows can be added in the UI; practical limits are CPU/driver/device limits.
- The additional-output area scrolls independently while the main output and footer remain visible.
- Refresh button in the top-right refreshes all device selectors at once.
- Automatic endpoint-list refresh can be enabled/disabled in Settings.
- Russian and English UI are supported.
- Device choices are locked while duplication is running.

## Channel routing

The main-output selector determines what is taken from the source:

- `Stereo`: preserve source L/R.
- `Left channel`: take source L and duplicate it internally to L/R.
- `Right channel`: take source R and duplicate it internally to L/R.

Each additional-output selector determines where that signal is rendered:

- `Stereo`: internal L -> device L, internal R -> device R.
- `Left channel`: internal stereo is mixed to mono, then sent only to device L.
- `Right channel`: internal stereo is mixed to mono, then sent only to device R.

Therefore, for example:

- Main = `Left channel`
- Additional = `Right channel`

results in the source left channel being played only through the right channel of the additional device.

For mono output endpoints there is no physical L/R distinction, so the mono signal is rendered through the endpoint's only channel.

## Audio engine

The engine uses:

- `IAudioClient` + `IAudioCaptureClient` for WASAPI loopback capture.
- `IAudioClient` + `IAudioRenderClient` for each additional endpoint.
- Shared-mode event-driven WASAPI.
- MMCSS `Pro Audio` worker threads.
- Internal float stereo transport.
- Linear sample-rate conversion when source and destination sample rates differ.
- Adaptive queue-rate correction to compensate for small clock drift between independent DisplayPort/HDMI audio endpoints.
- Automatic low-latency buffering; no user-facing WASAPI/buffer controls.

Supported endpoint mix formats are common Windows PCM formats (8/16/24/32-bit) and 32-bit IEEE float.

## Validation rules

The app refuses to start when:

- the main output is unavailable;
- no available additional output is selected;
- an additional output is the same endpoint as the main output (this would create a loopback feedback path);
- the same additional endpoint is selected more than once.

If an active endpoint disappears or WASAPI reports a runtime error, duplication is stopped and the error is shown.

## Windows requirement

Target: 64-bit Windows 10/11. Event-driven WASAPI loopback is supported directly on Windows 10 version 1703 and later.

## Build

Recommended toolchain: Visual Studio 2022 with **Desktop development with C++** and CMake tools.

From a Developer Command Prompt:

```bat
build_release.bat
```

The script creates a Release x64 build and copies the resulting file to:

```text
AudioDuplicate.exe
```

The MSVC runtime is linked statically (`/MT`) so the release executable does not need the Visual C++ Redistributable installed separately. It still uses normal Windows system DLLs/APIs.

## Project layout

```text
AudioDuplicate/
  CMakeLists.txt
  build_release.bat
  README.md
  res/
    resources.rc      # version metadata
  src/
    main.cpp           # Win32 GUI, scrolling output list, settings, localization
    audio.hpp
    audio.cpp          # endpoint enumeration, loopback capture, render workers
    config.hpp
    config.cpp         # optional config file next to EXE only
```

## Portable-settings behavior

Default: settings are not persisted and the app creates no configuration file.

If **Save settings next to EXE** is enabled, `AudioDuplicate.cfg` stores:

- language;
- automatic device refresh setting;
- main endpoint and its channel mode;
- additional endpoints and their channel modes.

Turning that option off deletes the existing `AudioDuplicate.cfg` file.
