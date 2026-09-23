# Matched Sine/Saw diagnostics on Windows

The simplest setup is to compile the tools in GitHub Actions, then run the downloaded
bundle locally with your own ROM. No local CMake, Visual Studio or NME checkout is needed.
The ROM never goes to GitHub. There is no synthetic ROM replacement that can test the
original G1 firmware's waveform compilation: both cases need the same real rack ROM.

## Build and download

After reviewing and committing the diagnostic changes, put the manual workflow on the
repository's default branch. In Actions, select **Build patch diagnostic tools**, then
**Run workflow**. It does not run on pushes, pull requests or tags and does not publish a
release or build/install a VST3. The production workflow separately packages the
four-voice patch with its Windows VST artifact.

The workflow uses the existing Windows toolchain, Gearmulator tag and JUCE version. NME
defaults to the exact revision inspected during the Build #7 investigation. Set `nme_ref`
to the NME commit used for the tested VST3 when known; the production workflow did not pin
NME. `build-info.json` records all resolved core/dependency commits, the corrected DSP
overlay hash, a unique build ID and the diagnostic executable hash.
The diagnostic build uses the static MSVC runtime for a portable executable.

Download **G1-patch-diagnostics-CMPM-build8-RUN-ATTEMPT** from the new workflow run's
Artifacts section and extract it into a new directory. The artifact is retained for 14 days.
It contains g1patchtest, dspdis, the matching modules.xml, SimpleOSC,
WobbleVoice-4Voice, the runner and provenance. There are no ROMs or dumps.
The pinned dsp56300 revision remains `1378c430...`; the correction is applied in the
G1 build overlay. Check `build-info.json`: `buildId` must start with `CMPM-build8-`,
`runUrl` must identify the new workflow run, and `executableSha256` must match the
downloaded `g1patchtest.exe`. The runner checks the build ID and hash before starting.

## Run the matched experiment

Open PowerShell in the extracted directory and run (replace the ROM path):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-matched.ps1 -RomPath 'C:\path\NORD-MODULAR-RACK-VER-3.03.BIN'
```

The execution policy override applies only to this invocation. PowerShell 7 (`pwsh`) also
works. No administrator privileges are needed. The script creates a new timestamped
results folder and ZIP in the current directory; it refuses to overwrite existing results.
Use `-OutputDirectory 'C:\path\new-results'` or `-TimeoutSeconds 600` if needed.

The runner copies SimpleOSC and generates SimpleOSC-Saw by changing exactly one byte:
OscA waveform 0 to 2. It rejects an unexpected source fixture. Each case starts a fresh
g1patchtest process with freshly installed flash from the same ROM, note 60 at velocity 100
through MIDI IN channel 1, two emulated seconds, normal JIT and threaded processing.
It clears inherited G1 experiment variables for each child process. The input signal is
the harness's default with no injected sine. This avoids the VST3 UI, state overlay and
live-edit queue entirely. ROM size/model/marker and SHA-256 are checked, including between
runs; the ROM is read in place and never copied to the results.

Each case has a 180-second wall-clock timeout. Even if Sine fails, Saw is attempted, and
available results are zipped before an error is returned. Silence itself does not make a
run fail: read the measurements. Successful capture requires exit zero, output/link reports,
a WAV and all twelve memory dumps. A timeout/crash may produce incomplete or buffered logs.

## What to inspect or share

- `run-info.json`: ROM/catalogue hashes, identical settings, fixture hashes and case outcomes.
- `build-info.json`: build ID, corrected DSP overlay and executable hashes, and dependency provenance.
- `SimpleOSC.pch`, `SimpleOSC-Saw.pch`, `modules.xml`: exact matched inputs, excluding ROM.
- Per case: `stdout.txt`, `stderr.txt`, `measurements.txt`, `output.wav` and `dumps/`.

The harness's output measurements cover the note-capture interval, but its link peaks are
cumulative since boot and may include upload transients. Compare both, not just link peaks.
The WAV has the harness's existing +36 dB gain; the text reports raw and compensated levels.
`G1_DUMP` captures the first 0x1000 words of P, X and Y on each DSP, not all memory. If relevant
generated code lies above that range, further instrumentation will be needed.

## Bounded DSP0 Square trace (diagnostic build only)

The manual **Build patch diagnostic tools** workflow builds the diagnostic bundle with
`G1_DSP_TRACE=ON`. Normal builds leave that option off. The trace runner uses your local ROM;
neither the ROM nor the trace is uploaded to GitHub Actions. From the downloaded bundle:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-square-trace.ps1 -RomPath 'D:\path\to\NORD-MODULAR-RACK-VER-3.03.BIN'
```

It makes a temporary patch differing only in OSC1 waveform (`2` Saw to `3` Square), starts each
case from a fresh machine, and writes a dated `square-block-trace-*.zip` next to the invocation directory. The
runner checks the executable's build ID/hash. It first runs each case without observation, then
with observation, and refuses the capture unless Saw remains audible and Square remains silent
in both runs. `Saw` and `Square` each contain `run-baseline.txt`, `run.txt`, P/X/Y dumps,
`dsp0-steps.csv` and `dsp0-steps.csv.writes.csv`.

The trace starts after 100 ms of the MIDI note at the next DSP0 JIT call, and records at most
12000 normal JIT blocks. Each CSV block has pre/post PC, opcode at the block entry,
cycles, A/B, X/Y data registers, R2–R5, status, X/Y values at candidate address-register
locations and current output-buffer
words. The writes CSV records every changed internal X/Y word in `$000`–`$7FF`. The register-address
values are snapshots, not a claim that the current instruction read each one. A row covers a
normal JIT call, which may execute multiple instructions. DSP0 retains the normal
`maxInstructionsPerBlock = 32`; the runner's Saw/Square audio checks establish that observation
has not masked the Square failure. Disassemble the block-entry PCs and compare memory writes
to narrow a suspect block before attributing a result to a particular instruction.

## DSP0 Square startup watch (diagnostic build only)

The newer manual diagnostic bundle also includes `run-square-startup.ps1`. Run it from the
downloaded bundle with your local ROM:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-square-startup.ps1 -RomPath 'D:\path\to\NORD-MODULAR-RACK-VER-3.03.BIN'
```

It runs the stored Saw patch with normal 32-instruction DSP0 JIT blocks, the generated
stored-Square patch with the same normal setting, and an explicitly selected Square
one-instruction-block reference. Each run starts from a fresh machine. The runner requires
Saw and the reference Square to produce audio and normal-block Square to remain silent.
It creates a dated `square-startup-*.zip` containing per-case audio logs, P/X/Y dumps,
`dsp0-startup.csv`, per-case checkpoint summaries, `checkpoint-comparison.csv` and
build/ROM hashes. No ROM is copied into it.

The watch starts before the first patch upload packet. It records `X:$1D–$1F` after each
DSP0 JIT call, at boot/host handoffs and at upload, DSP-load and note checkpoints. Rows
with changed watched words include the pre/post PC, cycles and selected registers;
up to 2048 note-stage blocks near the oscillator, `$023F` consumer and output path are
also recorded. The failing run always keeps `maxInstructionsPerBlock = 32`. A change
within a 32-instruction block is attributable to that block, not necessarily to one
instruction; an intermediate write that is reversed inside the same block is not visible
to this probe. The one-instruction reference helps narrow such a block without changing
the failing run.

For the post-upload cycle divergence, the diagnostic bundle can add a bounded
settling trace without changing the 32-instruction failing configuration:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-square-startup.ps1 -RomPath 'D:\path\to\NORD-MODULAR-RACK-VER-3.03.BIN' -SettleTrace
```

Each case includes `dsp0-settle.csv`, sampled once per emulated millisecond during
the original continuous 300 ms load interval. It records DSP0 cycles and PC,
IRQD/host-port state, the most frequent JIT block-entry PC, largest block cycle
advance, and separate cycle totals for regular catch-up, host-word waits, host
commands, ISR reads and RX-empty polling. `dsp0-settle.csv.blocks.csv` records
pre/post PC, cycles, status, registers and watched X words for at most 20,000
blocks in the first millisecond. Its `cause` field is 0=catch-up, 1=host word,
2=host command, 3=ISR read, 4=RX-empty callback. After finding the first divergent millisecond,
rerun with `-SettleTrace -FocusMs N` to capture that millisecond instead. The
runner still rejects any run in which the known Saw-audible, Square-silent,
Square-reference-audible pattern changes. No ROM is packaged.

## Overlapping-note capture

The same bundle can capture two overlapping notes through the G1 MIDI IN path. Run this
in the extracted bundle directory with your own ROM:

```powershell
.\g1patchtest.exe 'C:\path\NORD-MODULAR-RACK-VER-3.03.BIN' .\WobbleVoice-4Voice.pch --modules .\modules.xml --note 60 --overlap-note 67 --seconds 4 --wav .\four-voice-overlap.wav
```

The diagnostic sends note 60 on at 0 s, note 67 on at 0.8 s, note 60 off at 1.6 s,
and note 67 off at 2.4 s, leaving 1.6 s for the release tail. Its log reports the
event times and the Nord display; the four-channel WAV captures the whole sequence.
The existing single-note mode is unchanged. The ROM is read in place and is not
included in the WAV or diagnostic artifact.

To disassemble a P-memory dump, use the bundled tool, for example:

```powershell
Get-Content '.\g1-matched-TIMESTAMP\SimpleOSC-Saw\dumps\dsp0_p.hex' | .\dspdis.exe > saw-dsp0-disassembly.txt
```

Share the results ZIP for comparison; it is not automatically uploaded anywhere. Dumps
contain firmware-derived DSP code/data, so review their contents before public distribution.
Do not add the ROM to the ZIP. Keep the bundle local or distribute it under the accompanying
GPL licence with the exact source revisions recorded in build-info.json.

CI runs the existing ROM-free g1dspcheck tests, checks executable startup and verifies fixture
generation with `-PrepareOnly`. The actual Sine/Saw experiment runs locally, not on the hosted
runner. `-PrepareOnly` is a packaging check and does not claim to test emulation.
