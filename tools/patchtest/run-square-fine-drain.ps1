param(
    [Parameter(Mandatory = $true)][string]$RomPath,
    [string]$OutputDirectory = (Join-Path (Get-Location).Path ('square-fine-drain-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$bundle = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $bundle 'g1patchtest.exe'
$modules = Join-Path $bundle 'modules.xml'
$fixture = Join-Path $bundle 'WobbleVoice-2Osc-4Voice.pch'
$buildInfo = Join-Path $bundle 'build-info.json'
foreach ($path in @($RomPath, $exe, $modules, $fixture, $buildInfo)) {
    if ([string]::IsNullOrWhiteSpace($path) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file is missing: $path"
    }
}
$info = Get-Content -LiteralPath $buildInfo -Raw | ConvertFrom-Json
if ($info.buildId -notlike 'CMPM-build8-squarefinedrain-*' -or
    $info.executableSha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash) {
    throw 'This bundle is not the bounded fine-drain diagnostic executable.'
}
if ((Get-Item -LiteralPath $RomPath).Length -ne 524288) {
    throw 'The G1 ROM must be exactly 524288 bytes.'
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath ($output + '.zip'))) {
    throw "Output already exists: $output"
}
New-Item -ItemType Directory -Path $output | Out-Null
$source = Get-Content -LiteralPath $fixture -Raw
$old = '2 7 10 64 64 64 64 2 0 0 0 0 0'
if ([regex]::Matches($source, [regex]::Escape($old)).Count -ne 1) {
    throw 'The expected OSC1 waveform row is absent or ambiguous.'
}
$square = Join-Path $output 'WobbleVoice-2Osc-4Voice-OSC1-Square.pch'
$source.Replace($old, '2 7 10 64 64 64 64 3 0 0 0 0 0') |
    Set-Content -LiteralPath $square -Encoding Ascii -NoNewline

$names = @('G1_MIDINOTE', 'G1_DSP_WATCH_BLOCKSIZE', 'G1_DSP_FINE_DRAIN_FILE',
           'G1_DSP_STARTUP_WATCH_FILE', 'G1_DSP_CALLBACK_WINDOW_FILE',
           'G1_DSP_DISPATCH_TRACE_FILE', 'G1_DSP_DISPATCH_TRACE_BEGIN',
           'G1_DSP_DISPATCH_TRACE_END', 'G1_DSP_IRQD_TRACE_FILE',
           'G1_DSP_CAUSAL_HOST_TRACE_FILE', 'G1_DSP_CAUSAL_HOST_TRACE_BEGIN',
           'G1_DSP_CAUSAL_HOST_TRACE_END', 'G1_DSP_CAUSAL_LINK_TRACE_FILE',
           'G1_DSP_VECTOR7E_TRACE_FILE', 'G1_DSP_SETTLE_FILE',
           'G1_DSP_SETTLE_FOCUS_MS', 'G1_DSP_ESSI_TIMELINE_FILE',
           'G1_DSP_ESSI_TIMELINE_BEGIN', 'G1_DSP_ESSI_TIMELINE_END',
           'G1_DSP_DMA_TRACE_FILE', 'G1_DSP_DMA_TRACE_BEGIN',
           'G1_DSP_DMA_TRACE_END', 'G1_DSP_ESSI_TRACE_FILE',
           'G1_DSP_TRACE', 'G1_DSP_TRACE_FILE', 'G1_INTERP', 'G1_DUMP')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$summary = @()
try {
    foreach ($name in $names) { Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
    foreach ($case in @(
        @{ Name = 'Square-32'; Patch = $square; BlockSize = 32; Audible = $false; Experiment = $false },
        @{ Name = 'Square-32-fine-drain'; Patch = $square; BlockSize = 32; Audible = $null; Experiment = $true },
        @{ Name = 'Square-1-reference'; Patch = $square; BlockSize = 1; Audible = $true; Experiment = $false },
        @{ Name = 'Saw-32'; Patch = $fixture; BlockSize = 32; Audible = $true; Experiment = $false }
    )) {
        $caseDir = Join-Path $output $case.Name
        $dumpDir = Join-Path $caseDir 'dumps'
        New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
        $env:G1_MIDINOTE = '1'
        $env:G1_DSP_STARTUP_WATCH_FILE = Join-Path $caseDir 'dsp0-startup.csv'
        $env:G1_DSP_CAUSAL_HOST_TRACE_FILE = Join-Path $caseDir 'dsp0-host-causal.csv'
        $env:G1_DSP_CAUSAL_HOST_TRACE_BEGIN = if ($case.Name -eq 'Saw-32') { '211239000' } else { '211257000' }
        $env:G1_DSP_CAUSAL_HOST_TRACE_END = '731300000'
        $env:G1_DSP_CAUSAL_LINK_TRACE_FILE = Join-Path $caseDir 'dsp0-link-causal.csv'
        $env:G1_DSP_VECTOR7E_TRACE_FILE = Join-Path $caseDir 'dsp0-vector7e.csv'
        $env:G1_DUMP = $dumpDir
        Remove-Item -LiteralPath Env:G1_DSP_WATCH_BLOCKSIZE, Env:G1_DSP_FINE_DRAIN_FILE,
            Env:G1_DSP_CALLBACK_WINDOW_FILE, Env:G1_DSP_DISPATCH_TRACE_FILE,
            Env:G1_DSP_DISPATCH_TRACE_BEGIN, Env:G1_DSP_DISPATCH_TRACE_END,
            Env:G1_DSP_IRQD_TRACE_FILE -ErrorAction SilentlyContinue
        if ($case.BlockSize -eq 1) { $env:G1_DSP_WATCH_BLOCKSIZE = '1' }
        if ($case.Name -like 'Square-*') {
            $env:G1_DSP_CALLBACK_WINDOW_FILE = Join-Path $caseDir 'dsp0-callback.csv'
            $env:G1_DSP_DISPATCH_TRACE_FILE = Join-Path $caseDir 'dsp0-dispatch.csv'
            $env:G1_DSP_DISPATCH_TRACE_BEGIN = '211258184'
            $env:G1_DSP_DISPATCH_TRACE_END = '211260000'
            $env:G1_DSP_IRQD_TRACE_FILE = Join-Path $caseDir 'dsp0-irqd.csv'
        }
        if ($case.Experiment) { $env:G1_DSP_FINE_DRAIN_FILE = Join-Path $caseDir 'dsp0-fine-drain.csv' }

        $result = & $exe $RomPath $case.Patch --modules $modules --note 60 --seconds 0.25 2>&1
        $result | Out-File -LiteralPath (Join-Path $caseDir 'run.txt') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name) exited with $LASTEXITCODE; partial data is in $output" }
        $text = $result -join "`n"
        if ($text -notmatch 'pid=1' -or $text -notmatch '\( 4\)') {
            throw "$($case.Name) did not upload as a four-voice patch; partial data is in $output"
        }
        if ($null -ne $case.Audible -and
            (($case.Audible -and $text -notmatch 'output 1: peak') -or
             (-not $case.Audible -and $text -notmatch 'output 1: silence'))) {
            throw "$($case.Name) changed its control audio result; partial data is in $output"
        }
        $firstFree = $null
        if ($case.Name -like 'Square-*') {
            $dispatch = @(Import-Csv -LiteralPath $env:G1_DSP_DISPATCH_TRACE_FILE)
            $firstFree = $dispatch | Where-Object {
                $_.selection -eq 'peripheral' -and $_.pre_pending -eq '0' -and
                [long]$_.pre_cycle -gt 211258189 -and [long]$_.pre_cycle -lt 211258368
            } | Select-Object -First 1
        }
        if ($case.Experiment) {
            if (-not (Test-Path -LiteralPath $env:G1_DSP_FINE_DRAIN_FILE -PathType Leaf)) {
                throw 'The bounded fine-drain experiment did not arm.'
            }
            $events = @(Import-Csv -LiteralPath $env:G1_DSP_FINE_DRAIN_FILE)
            if (@($events | Where-Object event -eq 'arm').Count -ne 1 -or
                @($events | Where-Object event -eq 'clear').Count -ne 7 -or
                @($events | Where-Object event -eq 'restore').Count -ne 1) {
                throw 'The fine-drain window did not arm, clear seven vectors and restore exactly once.'
            }
        }
        $summary += [pscustomobject]@{
            case = $case.Name
            initialBlockLimit = $case.BlockSize
            intervention = [bool]$case.Experiment
            audible = ($text -match 'output 1: peak')
            firstPendingFreeEssiCycle = if ($null -ne $firstFree) { $firstFree.pre_cycle } else { '' }
            firstPendingFreeEssiPc = if ($null -ne $firstFree) { $firstFree.pre_pc } else { '' }
            dsp0PMemorySha256 = (Get-FileHash -LiteralPath (Join-Path $dumpDir 'dsp0_p.hex') -Algorithm SHA256).Hash
        }
    }
}
finally {
    foreach ($name in $names) {
        if ($null -eq $previous[$name]) {
            Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
        }
    }
}
$summary | Export-Csv -LiteralPath (Join-Path $output 'comparison.csv') -NoTypeInformation -Encoding Ascii
[ordered]@{
    buildId = $info.buildId
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    romSha256 = (Get-FileHash -LiteralPath $RomPath -Algorithm SHA256).Hash
    sawPatchSha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
    squarePatchSha256 = (Get-FileHash -LiteralPath $square -Algorithm SHA256).Hash
    intervention = 'DSP0 callback exit at cycle 211258189: cached blocks rebuilt at limit 1; restored after seven natural $1E accepts and suppression exit'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'experiment-info.json') -Encoding UTF8
$zip = $output + '.zip'
Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zip
Write-Output "Bounded Square fine-drain experiment: $zip"
