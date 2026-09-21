G1-Emu VST3 Build #4 - Playability

Replace the files in this ZIP at the matching repo-relative paths.

Changes:
- PANIC button sends All Notes Off (CC123) and All Sound Off (CC120) on all 16 MIDI channels.
- Plugin state now remembers the .pch path and reloads it when a host session is restored.
- Build #3 state remains readable.
- Patch loader suspend/mutex handling is cleaned up.
- GitHub Actions is Windows x64 only.
- No CMake files are changed.

Test sequence:
1. Build in GitHub Actions.
2. Install VST3 and load ROM + SimpleOSC.pch.
3. Confirm MIDI/audio still work.
4. Hit PANIC and confirm notes/sound stop where the patch responds to MIDI CC120/123.
5. Save a Gig Performer session, close/reopen it, and confirm ROM + patch restore automatically.
