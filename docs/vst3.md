# G1-Emu VST3 (experimental)

The `g1vst3` target wraps the existing G1 emulation core as a 64-bit VST3 instrument. It does not contain, download, or redistribute a Nord ROM. On first use, press **Choose ROM…** and select a 512 KB ROM dumped from your own Nord Modular G1.

Current v0.1 scope:

- MIDI from the DAW is sent to the emulated G1 SCI/MIDI input.
- DSP 3's four 96 kHz hardware outputs are exposed as four VST3 output channels.
- Host sample rates are supported with a simple linear 96 kHz-to-host-rate conversion.
- The emulated flash is stored in the DAW plug-in state, so G1 patches/settings can persist with the project.
- The ROM itself is never embedded in plug-in state; only its path is stored.

Known limitations: external audio input and PC-Port/NME routing are not yet exposed by the VST3; resampling is intentionally basic; the first GUI is a functional shell rather than the complete hardware panel.

## Windows build

Use the same Gearmulator and JUCE revisions as the repository CI, then configure normally. The VST3 target is built automatically when JUCE is available. With Visual Studio/CMake Release builds the bundle is produced under `build/plugin/g1vst3_artefacts/Release/VST3/G1-Emu.vst3`.
