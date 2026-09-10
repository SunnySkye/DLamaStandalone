# Delay Lama Standalone for Windows

This directory adds a native Win32 build while leaving the existing macOS
AppKit build and `build.sh` unchanged. The Windows executable shares the C
vocal DSP and uses only Windows-provided APIs:

- Win32/GDI+ for the fixed-size classic interface and PNG artwork;
- WinMM `waveOut` with four 512-frame buffers for 48 kHz stereo audio;
- WinMM `midiIn` for all available short-message MIDI input devices.

No third-party runtime or audio/MIDI library is required. GDI+ and WinMM are
part of Windows. The audio output is converted from the DSP's float samples to
16-bit PCM for compatibility with the system wave mapper.

## Build and run

Install Visual Studio 2019 or newer (the Desktop C++ workload), CMake 3.20 or
newer, and the Windows SDK. From PowerShell at the repository root:

```powershell
.\Windows\build.ps1
```

For a clean Debug build:

```powershell
.\Windows\build.ps1 -Configuration Debug -Clean
```

The script configures `Windows/CMakeLists.txt`, builds the GUI and headless
Windows smoke tests, and runs those tests through CTest. The executable is in
the CMake configuration's output directory under `Windows/build`; its
`Resources` folder is populated automatically from the repository assets. Run
`DelayLamaStandalone.exe` from that output directory so it can find the copied
artwork and original manual.

The same path can be driven directly by CMake, for example in a Visual Studio
developer shell:

```powershell
cmake -S .\Windows -B .\Windows\build -A x64
cmake --build .\Windows\build --config Release --parallel
ctest --test-dir .\Windows\build -C Release --output-on-failure
```

To create the reproducible x64 release bundle, run:

```powershell
.\Windows\package-release.ps1 -Clean
```

This builds Release, stages `DelayLamaStandalone.exe`, the `Resources` folder,
both license files, and this Windows README, then writes these files under
`Windows\build\package`:

- `DelayLamaStandalone-v1.0-windows-x64.zip`
- `DelayLamaStandalone-v1.0-windows-x64.zip.sha256`

The release gate intentionally uses the two focused CTest executables
`delay_lama_windows_smoke` and `delay_lama_windows_bridge_smoke` rather than a
GUI `--self-test` mode. They exercise the shared DSP and Win32 bridge without
requiring a desktop session, audio device, or physical MIDI device.

GitHub Actions exercises this configuration in
`.github/workflows/windows.yml` on `windows-latest`.

## Controls and known boundaries

The flag, two knobs, delay handle, computer keyboard, and the About/manual
action are implemented with native Win32 messages. MIDI follows the existing
mapping: notes, pitch wheel to vowel, CC1 vibrato, CC5 glide, CC7 volume, CC12
delay, and CC13 voice. The default Windows playback device is used; there is
no ASIO-specific path or device picker. WinMM short MIDI messages are handled,
while SysEx is intentionally ignored.
