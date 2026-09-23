param(
    [Parameter(Mandatory = $true)][string]$RomPath,
    [string]$OutputDirectory = (Join-Path (Get-Location).Path ('square-block-trace-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$bundle = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $bundle 'g1patchtest.exe'
$modules = Join-Path $bundle 'modules.xml'
$fixture = Join-Path $bundle 'WobbleVoice-2Osc-4Voice.pch'
$buildInfoPath = Join-Path $bundle 'build-info.json'

foreach ($path in @($RomPath, $exe, $modules, $fixture, $buildInfoPath)) {
    if ([string]::IsNullOrWhiteSpace($path) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file is missing: $path"
    }
}
$info = Get-Content -LiteralPath $buildInfoPath -Raw | ConvertFrom-Json
if ($info.buildId -notlike 'CMPM-build8-squareblocktrace-*' -or
    $info.executableSha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash) {
    throw 'This bundle is not the matching CMPM-corrected Square trace executable.'
}
if ((Get-Item -LiteralPath $RomPath).Length -ne 524288) {
    throw 'The G1 ROM must be exactly 524288 bytes.'
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output directory already exists: $output" }
New-Item -ItemType Directory -Path $output | Out-Null
$source = Get-Content -LiteralPath $fixture -Raw
$old = '2 7 10 64 64 64 64 2 0 0 0 0 0'
if ([regex]::Matches($source, [regex]::Escape($old)).Count -ne 1) {
    throw 'The expected OSC1 waveform row is absent or ambiguous.'
}
$square = Join-Path $output 'WobbleVoice-2Osc-4Voice-OSC1-Square.pch'
$source.Replace($old, '2 7 10 64 64 64 64 3 0 0 0 0 0') |
    Set-Content -LiteralPath $square -Encoding Ascii -NoNewline

$names = @('G1_MIDINOTE', 'G1_DSP_TRACE', 'G1_DSP_TRACE_FILE',
           'G1_DSP_TRACE_START', 'G1_DSP_TRACE_STEPS', 'G1_DUMP',
           'G1_INTERP', 'G1_NO_LA_FIX', 'G1_KNOBS', 'G1_PREPRESS',
           'G1_PRESS', 'G1_HOLD', 'G1_HOLD_END', 'G1_DIAL')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $names) {
        Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue
    }
    foreach ($case in @(
        @{ Name = 'Saw'; Patch = $fixture },
        @{ Name = 'Square'; Patch = $square }
    )) {
        $caseDir = Join-Path $output $case.Name
        $dumpDir = Join-Path $caseDir 'dumps'
        New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
        $env:G1_MIDINOTE = '1'
        Remove-Item -LiteralPath Env:G1_DSP_TRACE_FILE -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath Env:G1_DUMP -ErrorAction SilentlyContinue
        $baseline = & $exe $RomPath $case.Patch --modules $modules --note 60 --seconds 0.25 2>&1
        $baseline | Out-File -LiteralPath (Join-Path $caseDir 'run-baseline.txt') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name) unobserved diagnostic exited with $LASTEXITCODE" }
        $baselineText = $baseline -join "`n"
        if ($baselineText -notmatch 'pid=1' -or $baselineText -notmatch '\( 4\)') {
            throw "$($case.Name) unobserved run did not upload as a four-voice patch."
        }
        if (($case.Name -eq 'Saw' -and $baselineText -notmatch 'output 1: peak') -or
            ($case.Name -eq 'Square' -and $baselineText -notmatch 'output 1: silence')) {
            throw "$($case.Name) unobserved audio did not reproduce the expected baseline/failure."
        }

        $env:G1_DSP_TRACE_FILE = Join-Path $caseDir 'dsp0-steps.csv'
        # Entry 0 starts at the next DSP0 JIT call. Waiting for $03F2 would
        # miss it when that instruction lies inside a normal 32-word block.
        $env:G1_DSP_TRACE_START = '0'
        $env:G1_DSP_TRACE_STEPS = '12000'
        $env:G1_DUMP = $dumpDir
        $result = & $exe $RomPath $case.Patch --modules $modules --note 60 --seconds 0.25 2>&1
        $result | Out-File -LiteralPath (Join-Path $caseDir 'run.txt') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name) diagnostic exited with $LASTEXITCODE" }
        $text = $result -join "`n"
        if ($text -notmatch 'pid=1' -or $text -notmatch '\( 4\)') {
            throw "$($case.Name) did not upload as a four-voice patch."
        }
        if (($case.Name -eq 'Saw' -and $text -notmatch 'output 1: peak') -or
            ($case.Name -eq 'Square' -and $text -notmatch 'output 1: silence')) {
            throw "$($case.Name) audio did not reproduce the expected baseline/failure under tracing."
        }
        $trace = Get-Item -LiteralPath $env:G1_DSP_TRACE_FILE
        if ($trace.Length -lt 500) { throw "$($case.Name) trace did not reach its selected DSP0 entry." }
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

[ordered]@{
    buildId = $info.buildId
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    romSha256 = (Get-FileHash -LiteralPath $RomPath -Algorithm SHA256).Hash
    sawPatchSha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
    squarePatchSha256 = (Get-FileHash -LiteralPath $square -Algorithm SHA256).Hash
    traceEntrySaw = 'next JIT call after 100 ms'
    traceEntrySquare = 'next JIT call after 100 ms'
    dsp0MaxInstructionsPerBlock = 32
    traceStepsRequested = 12000
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'trace-info.json') -Encoding UTF8
$zip = "$output.zip"
if (Test-Path -LiteralPath $zip) { throw "Archive already exists: $zip" }
Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zip
Write-Output "Matched Square trace: $zip"
