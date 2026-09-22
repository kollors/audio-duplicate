# Audio Duplicate

Portable Windows utility that captures the audio currently playing on one render endpoint using WASAPI Loopback and mirrors it to any number of additional render endpoints.

## Implemented UI

- Main output selector.
- Per-main-output channel mode: Stereo / Left / Right.
- Dynamic list of **Additional outputs**.
- `+` adds another output; `−` removes that row.
- Per-output channel mode: Stereo / Left / Right.
- Scrollbar appears when the additional-output list no longer fits.
- Global refresh button next to Settings refreshes every device selector.
- Settings:
  - auto-refresh device list;
  - save settings beside EXE;
  - Russian / English.

## Channel routing

- Main Stereo -> Output Stereo: L->L, R->R.
- Main Left -> Output Stereo: source L is duplicated to L+R.
- Main Right -> Output Stereo: source R is duplicated to L+R.
- Main Left -> Output Right: source L goes only to output R.
- Main Right -> Output Left: source R goes only to output L.
- Main Stereo -> Output Left/Right: L+R is mixed to mono and placed only on the selected output channel.

## Portability

The application itself:

- does not install drivers or services;
- does not create virtual audio devices;
- does not write its own settings to the Registry, AppData, ProgramData or Temp;
- writes `AudioDuplicate.ini` only beside the executable when **Save settings beside EXE** is enabled;
- deletes that INI when saving is disabled.

Windows, the audio service and GPU/audio drivers may maintain their own normal operating-system state independently of this application.

## Audio implementation

- Main endpoint: WASAPI Loopback, shared mode.
- Additional endpoints: WASAPI shared render streams.
- `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` and `AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY` let the Windows Audio Engine convert the source format to each output endpoint's mix format.
- Each additional endpoint has its own render worker and bounded queue.
- Audio workers opt into MMCSS `Pro Audio` scheduling.

## Build

Requires Visual Studio 2022 Build Tools or Visual Studio with **Desktop development with C++** and Windows 10/11 SDK.

From a Developer Command Prompt:

```bat
build.bat
```

The executable is created under `build\\Release\\AudioDuplicate.exe`.
