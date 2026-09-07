# Reverse-engineering notes

## Supplied archive

- File: `DelayLamaVSTxbeta.pkg.sit`
- SHA-256: `b15a837f0934ac3a2638c09343cf7b6b395cf0b32914093d426a5334e95fb42f`
- Container: StuffIt 5 archive containing an Installer VISE-era package
- Product metadata: Delay Lama VST OS X 1.2 beta
- Original install destination: `/Library/Audio/Plug-Ins/VST`

## Original executable

The payload contains `DelayLama1.2b.vst`. Its executable is a big-endian,
32-bit PowerPC Mach-O bundle (`MH_BUNDLE`) linked to Carbon and the 2004-era
system library. It has no Intel or arm64 slice. Modern macOS cannot load it:
PowerPC execution support ended with Rosetta 1, Carbon was removed from the
64-bit runtime, and VST 1/2 plug-ins are not standalone applications.

The binary was not stripped. Its C++ names expose the VST shell, VSTGUI classes,
the Delay Lama editor/audio classes, processing callbacks, parameter accessors,
and MIDI event handling. This was enough to confirm the control surface and
processing topology without trying to execute untrusted legacy code.

## Recovered resources

The plug-in's data-fork resource container includes ten named `PICT` resources:

- 360×510 background
- 1570×1866 face matrix containing 30 animation frames (5 columns × 6 rows)
- left/right 60-frame knob strips
- pitch and vowel tracks/handles
- delay handle
- 253×275 information panel

The PICT resources were extracted with Apple's `DeRez` and decoded with
FFmpeg's QuickDraw decoder. The original three-page PDF manual was recovered
from the package payload.

## Standalone reconstruction

The modern app uses MonkSynth's clean-room FOF (formant-wave-function) vocal
engine rather than translated PowerPC machine code. It preserves the original
monophonic playing model, vowel interpolation, voice/formant shift, glide,
vibrato, built-in stereo delay, classic MIDI mapping, and vowel-driven monk
animation. A thin C bridge serializes GUI, MIDI, and real-time audio access.

The application shell is native AppKit/AVFoundation/CoreMIDI. It requires no
plug-in host, third-party framework, installer, or network access. The delivered
binary is universal (arm64 + x86_64), ad-hoc signed, and targets macOS 13+.

## Fidelity boundary

This is a compatible standalone reconstruction, not a byte-for-byte port. The
classic artwork and interaction model come from the supplied Mac archive; the
DSP comes from a modern clean-room implementation of the same published FOF
technique. Small sonic differences from the original PowerPC VST should be
expected, especially in formant interpolation, parameter smoothing, and delay
gain staging.
