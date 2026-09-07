# Delay Lama Standalone for macOS

This is a native, standalone preservation build inspired by AudioNerdz' 2002
Delay Lama instrument. It was built for the supplied 2004 Mac package, whose
PowerPC/Carbon VST can no longer execute on current macOS.

## Use

- Drag across the Tibetan flag to sing. Horizontal movement controls pitch;
  vertical movement controls the vowel.
- Drag the left knob vertically to change Glide.
- Drag the right knob vertically to change Voice.
- Drag the handle at the bottom to change Delay.
- Play chromatically from the computer keyboard with
  `A W S E D F T G Y H U J K O L`.
- Connected MIDI devices work automatically. Notes, pitch wheel → vowel,
  CC1 → vibrato, CC5 → glide, CC7 → volume, CC12 → delay, and CC13 → voice.

The app is ad-hoc signed for local use and makes no network connections. It
targets macOS 13 or later and contains native arm64 and x86_64 executables.

## Build

Run `./build.sh` on macOS with Xcode Command Line Tools installed. The generated
application is written to `build/Delay Lama Standalone.app`.

## Provenance and licensing

The vocal synthesis code is derived from Jonathan Taylor's MIT-licensed
MonkSynth clean-room reconstruction. See `MONKSYNTH-LICENSE.txt`.

Classic artwork and the original manual were recovered from the user-supplied
`DelayLamaVSTxbeta.pkg.sit`. AudioNerdz' original manual states that Delay Lama
is freeware and may be distributed freely when all original files accompany the
plug-in; it may not be sold or used as part of a commercial promotion. These
assets are not relicensed by this project.

This preservation build is not affiliated with or endorsed by AudioNerdz.
