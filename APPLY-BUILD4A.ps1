param(
    [string]$RepoRoot = (Get-Location).Path
)

$ErrorActionPreference = "Stop"
$processor = Join-Path $RepoRoot "plugin\PluginProcessor.cpp"

if (-not (Test-Path $processor)) {
    throw "Can't find plugin\PluginProcessor.cpp. Run this from the G1-Emu-VST3 repository root, or pass -RepoRoot."
}

$text = Get-Content $processor -Raw

if ($text -match 'void\s+G1PluginProcessor::panic\s*\(') {
    Write-Host "panic() is already implemented. Nothing to do."
    exit 0
}

$anchor = "void G1PluginProcessor::advanceTo(uint64_t t)"
$idx = $text.IndexOf($anchor)
if ($idx -lt 0) {
    throw "Couldn't find the advanceTo() anchor. No files were changed."
}

$implementation = @'
void G1PluginProcessor::panic()
{
    suspendProcessing(true);
    {
        std::lock_guard lock(machineMutex);
        if (mc)
        {
            // Send All Notes Off (CC123) and All Sound Off (CC120)
            // on every MIDI channel to the emulated physical MIDI input.
            for (int ch = 0; ch < 16; ++ch)
            {
                const uint8_t status = static_cast<uint8_t>(0xb0 | ch);
                mc->getSci().write({ status, 123, 0 });
                mc->getSci().write({ status, 120, 0 });
            }

            // Give the emulated machine a short opportunity to consume them.
            runFor(*mc, 20 * g_ms);
            native.clear();
            emuTimeCycles = static_cast<double>(mc->ucCycles());
            lastStatus = "MIDI panic sent on all 16 channels.";
        }
    }
    suspendProcessing(false);
}

'@

$text = $text.Insert($idx, $implementation)
Set-Content -Path $processor -Value $text -Encoding UTF8

Write-Host "Build #4a applied: added G1PluginProcessor::panic()."
Write-Host "Review the single modified file in GitHub Desktop, then commit and push."
