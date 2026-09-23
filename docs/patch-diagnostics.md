# Matched Sine/Saw diagnostics on Windows

The simplest setup is to compile the tools in GitHub Actions, then run the downloaded
bundle locally with your own ROM. No local CMake, Visual Studio or NME checkout is needed.
The ROM never goes to GitHub. There is no synthetic ROM replacement that can test the
original G1 firmware's waveform compilation: both cases need the same real rack ROM.

## Build and download

After reviewing and committing the diagnostic changes, put the manual workflow on the
repository's default branch. In Actions, select **Build patch diagnostic tools**, then
**Run workflow**. It does not run on pushes, pull requests or tags and does not publish a
release or build/install a VST3. The existing production workflow is unchanged.

The workflow uses the existing Windows toolchain, Gearmulator tag and JUCE version. NME
defaults to the exact revision inspected during the Build #7 investigation. Set `nme_ref`
to the NME commit used for the tested VST3 when known; the production workflow did not pin
NME. `build-info.json` records all resolved core/dependency commits and the tool hash.
The diagnostic build uses the static MSVC runtime for a portable executable.

Download **G1-patch-diagnostics-windows-x64** from the workflow's Artifacts section and
extract it. The artifact is retained for 14 days. It contains g1patchtest, dspdis, the
matching modules.xml, SimpleOSC, the runner and provenance. There are no ROMs or dumps.

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
- `build-info.json`: executable and dependency provenance.
- `SimpleOSC.pch`, `SimpleOSC-Saw.pch`, `modules.xml`: exact matched inputs, excluding ROM.
- Per case: `stdout.txt`, `stderr.txt`, `measurements.txt`, `output.wav` and `dumps/`.

The harness's output measurements cover the note-capture interval, but its link peaks are
cumulative since boot and may include upload transients. Compare both, not just link peaks.
The WAV has the harness's existing +36 dB gain; the text reports raw and compensated levels.
`G1_DUMP` captures the first 0x1000 words of P, X and Y on each DSP, not all memory. If relevant
generated code lies above that range, further instrumentation will be needed.

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
