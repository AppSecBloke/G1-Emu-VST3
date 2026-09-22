# Build #7 review and acceptance

This build is a SimpleOSC proof of concept. The processor owns one module catalogue for
its lifetime and destroys its parsed patch before that catalogue. Editors receive copies
of parameter metadata/values, tagged with a patch generation, never NME pointers.

Only descriptors with class `parameter`, a nonconstant range inside 0..127 and seven-bit
module/parameter indices are editable. Section comes from the patch container (0 common,
1 poly), module from its container index and parameter from its descriptor index. Custom
and morph descriptors are excluded even when their indices overlap ordinary parameters.

Edits pass through one validated model update. Each discovered parameter has one pending
value, so repeated edits coalesce. The processing path sends at most one pending edit per
block, at least five emulated milliseconds apart, without waiting for an acknowledgement.
Changing patch generation discards the pending values. Diagnostics count transmissions,
not confirmed synth changes; incoming PC-Port bytes are drained, not interpreted as readback.

The existing `oscCoarse` host parameter remains an input alias. It binds only if exactly one
discovered `OscA` / `freq coarse` parameter has range 0..127. Its value is baselined on load,
not imposed on the patch. All generated controls are custom UI controls: turning one does
not record DAW automation or update the host's displayed alias value. Subsequent changes
to the host alias enter the same edit queue. No additional host parameters are registered.

State now has a Build #7 magic/version and a version-1 XML overlay of edited raw values.
It retains ROM path, flash, patch path and host coarse value, and accepts the old state
layout. Overlays require an exact source-file SHA-256 and embedded-catalogue SHA-256 match,
plus matching address, type, component ID, range and original value for every entry. Restore
applies values before uploading the patch, never as live edits. A missing/changed source
or catalogue rejects patch restoration with a status message; the restored ROM remains.
The `.pch` is not embedded or modified, so it must remain available at its saved path.
Even a source comment or catalogue-only change invalidates the hash deliberately.

## Static verification performed

- Compared the PC-Port sender with HEAD: unchanged byte for byte.
- Confirmed one host parameter registration and no hard-coded `(1, 1, 0)` edit call.
- Checked descriptor lifetimes, generation rejection, queued delivery, initialization
  notification suppression and overlay-before-upload ordering in the source review.
- Cross-checked the repository's SimpleOSC dump against NME `modules.xml` at commit
  `94874fc726df80ae2c096e1db22df41afc566c5e`: ten OscA plus three 2Output ordinary
  parameters, with all fixture values in range. The control heuristic yields nine
  rotaries, two toggles and two selectors. This did not execute the C++ parser or UI.
- `git diff --check` passed. No local compilation/runtime tests were possible with the
  available toolchain. The workflow uses JUCE 8.0.12 and an unpinned NME checkout; CI must
  verify compatibility with the NME revision actually fetched.

## Test the CI binary

1. Confirm the visible title ends in `playability build #7`. Load the ROM and SimpleOSC.
   Expect 13 discovered parameters, zero sent live edits and the coarse host alias bound.
   Close/reopen the editor and let audio processing run; the count must remain zero.
2. Inspect both module groups (scroll if necessary). OscA starts with coarse/fine/KBT/pulse
   width at 64, waveform at Sine and the remaining five parameters at zero. 2Output starts
   at level 100, destination 1/2 and mute off. There must be no display-units or morph control.
3. Play notes and change coarse, fine, waveform, output level and both mutes. Check audible
   changes and last-transmitted address/value. OscA uses section 1, module 1, parameters
   0..9; 2Output uses section 1, module 2, parameters 0..2 for this fixture only. Modulation
   depths may have no audible effect because SimpleOSC does not connect those inputs.
4. Drag a rotary rapidly. Check responsiveness, final value and continuing audio. While
   processing is stopped, change one control repeatedly; if the DAW truly stops callbacks,
   resuming should transmit only its final pending value. Save before resuming and verify
   that this final model value is also restored.
5. Reload SimpleOSC while edits are pending. Expect source values, a reset send count and
   no stale edit after processing resumes. Use a temporary copy with a nondefault coarse
   value to verify that loading/reopening never imposes the host default of 64.
6. In the DAW automation interface, change the existing Osc A Coarse parameter. Confirm
   the matching dynamic control follows and sound changes. There must be no new automation
   parameters. Test a separate patch copy with relocated module indices (and its references
   updated consistently): transmission must follow those indices. With zero or multiple
   OscA instances the host alias must stay unbound; custom controls remain usable.
7. Edit several values, save the DAW project, close/reopen it and reopen the editor. Expect
   restored values and zero live-edit transmissions after upload. Verify the source `.pch`
   is byte-identical. Test an older Build #6 project for ROM/flash/patch/host-value recovery.
8. With a backup, change or remove the saved source patch and restore a Build #7 session.
   Expect explicit patch-restoration failure and no active controls for an uncertain patch.
   A bad ROM/patch selection should leave an intelligible status message. Restore the backup.

The NME parser remains permissive about malformed/unknown patch content; this is not an
arbitrary-patch certification. Formatter output is NME's generic display, without the full
editor's custom display-unit/context logic. Queue pacing and every control's audible effect
still require the runtime checks above. Existing mutex-based emulation/audio processing is
retained; this change does not claim a real-time-safe audio engine redesign.
