param(
    [Parameter(Mandatory = $true)][string]$RomPath,
    [string]$OutputDirectory = (Join-Path (Get-Location).Path ('square-output-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [switch]$FlowTrace
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
if ($info.buildId -notmatch '^CMPM-build8-square(output|voice|flow)-' -or
    $info.executableSha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash) {
    throw 'This bundle is not the bounded Square output diagnostic executable.'
}
if ($FlowTrace -and $info.buildId -notmatch '^CMPM-build8-squareflow-') {
    throw 'The requested control-flow trace requires the squareflow diagnostic executable.'
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
           'G1_DSP_OUTPUT_WRITES_FILE', 'G1_DSP_OUTPUT_LINK_FILE',
           'G1_DSP_OUTPUT_BEGIN', 'G1_DSP_OUTPUT_END', 'G1_DSP_VOICE_FILE', 'G1_DSP_FLOW_FILE', 'G1_DUMP',
           'G1_DSP_CAUSAL_HOST_TRACE_FILE', 'G1_DSP_CAUSAL_LINK_TRACE_FILE',
           'G1_DSP_STARTUP_WATCH_FILE', 'G1_DSP_CALLBACK_WINDOW_FILE',
           'G1_DSP_DISPATCH_TRACE_FILE', 'G1_DSP_IRQD_TRACE_FILE',
           'G1_DSP_DMA_TRACE_FILE', 'G1_DSP_ESSI_TRACE_FILE',
           'G1_DSP_TRACE', 'G1_DSP_TRACE_FILE', 'G1_INTERP')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$summary = @()
try {
    foreach ($name in $names) { Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
    foreach ($case in @(
        @{ Name = 'Square-32-fine-drain'; BlockSize = 32; Audible = $false; Experiment = $true },
        @{ Name = 'Square-1-reference'; BlockSize = 1; Audible = $true; Experiment = $false }
    )) {
        $caseDir = Join-Path $output $case.Name
        $dumpDir = Join-Path $caseDir 'dumps'
        New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
        $env:G1_MIDINOTE = '1'
        $env:G1_DSP_OUTPUT_BEGIN = '236970000'
        $env:G1_DSP_OUTPUT_END = '237070000'
        $env:G1_DSP_OUTPUT_WRITES_FILE = Join-Path $caseDir 'dsp0-output-writes.csv'
        $env:G1_DSP_OUTPUT_LINK_FILE = Join-Path $caseDir 'dsp0-output-link.csv'
        if ($info.buildId -match '^CMPM-build8-square(voice|flow)-') {
            $env:G1_DSP_VOICE_FILE = Join-Path $caseDir 'dsp0-voice-producer.csv'
        }
        if ($FlowTrace) { $env:G1_DSP_FLOW_FILE = Join-Path $caseDir 'dsp0-voice-flow.csv' }
        $env:G1_DUMP = $dumpDir
        if ($case.BlockSize -eq 1) { $env:G1_DSP_WATCH_BLOCKSIZE = '1' }
        else { Remove-Item -LiteralPath Env:G1_DSP_WATCH_BLOCKSIZE -ErrorAction SilentlyContinue }
        if ($case.Experiment) { $env:G1_DSP_FINE_DRAIN_FILE = Join-Path $caseDir 'dsp0-fine-drain.csv' }
        else { Remove-Item -LiteralPath Env:G1_DSP_FINE_DRAIN_FILE -ErrorAction SilentlyContinue }

        $result = & $exe $RomPath $square --modules $modules --note 60 --seconds 0.25 2>&1
        $result | Out-File -LiteralPath (Join-Path $caseDir 'run.txt') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name) exited with $LASTEXITCODE; partial data is in $output" }
        $runText = $result -join "`n"
        if ($runText -notmatch 'pid=1' -or $runText -notmatch '\( 4\)') {
            throw "$($case.Name) did not upload as a four-voice patch; partial data is in $output"
        }
        if (($case.Audible -and $runText -notmatch 'output 1: peak') -or
            (-not $case.Audible -and $runText -notmatch 'output 1: silence')) {
            throw "$($case.Name) did not retain the expected audio result; partial data is in $output"
        }
        if ($case.Experiment) {
            $events = @(Import-Csv -LiteralPath $env:G1_DSP_FINE_DRAIN_FILE)
            if (@($events | Where-Object event -eq 'arm').Count -ne 1 -or
                @($events | Where-Object event -eq 'clear').Count -ne 7 -or
                @($events | Where-Object event -eq 'restore').Count -ne 1) {
                throw 'The seven-interrupt fine-drain window did not complete exactly once.'
            }
        }
        $writes = @(Import-Csv -LiteralPath $env:G1_DSP_OUTPUT_WRITES_FILE)
        $links = @(Import-Csv -LiteralPath $env:G1_DSP_OUTPUT_LINK_FILE)
        if (-not $writes.Count -or -not $links.Count) {
            throw "$($case.Name) produced no bounded output-path records; partial data is in $output"
        }
        $targetWrite = $writes | Where-Object sequence -eq '437' | Select-Object -First 1
        if ($null -eq $targetWrite -or $targetWrite.instruction_pc -ne '1650' -or
            $targetWrite.address -ne '1760') {
            throw "$($case.Name) no longer has the aligned output write #437; partial data is in $output"
        }
        if ($info.buildId -match '^CMPM-build8-square(voice|flow)-') {
            $voice = @(Import-Csv -LiteralPath $env:G1_DSP_VOICE_FILE)
            $sourceWrites = @($voice | Where-Object { $_.pc -eq '580' -and $_.phase -eq 'post' })
            $targetProducer = @($voice | Where-Object { $_.pc -eq '1650' -and $_.phase -eq 'post' -and $_.output_write_sequence -eq '437' })
            if (-not $voice.Count -or $targetProducer.Count -ne 1 -or
                ($case.Audible -and -not $sourceWrites.Count) -or
                (-not $case.Audible -and $sourceWrites.Count)) {
                throw "$($case.Name) lacks the bounded producer trace; partial data is in $output"
            }
        }
        if ($FlowTrace) {
            $flow = @(Import-Csv -LiteralPath $env:G1_DSP_FLOW_FILE)
            if (-not $flow.Count -or
                ($case.Audible -and -not @($flow | Where-Object { $_.pc -eq '574' -and $_.phase -eq 'pre' }).Count)) {
                throw "$($case.Name) lacks the bounded post-note control-flow trace; partial data is in $output"
            }
        }
        $firstNonzeroWrite = $writes | Where-Object { [long]$_.new -ne 0 } | Select-Object -First 1
        $firstNonzeroLink = $links | Where-Object { [long]$_.link0 -ne 0 -or [long]$_.link1 -ne 0 } | Select-Object -First 1
        $summary += [pscustomobject]@{
            case = $case.Name
            blockLimit = $case.BlockSize
            fineDrain = $case.Experiment
            audible = $case.Audible
            writes = $writes.Count
            linkSamples = $links.Count
            firstNonzeroWriteSequence = if ($null -ne $firstNonzeroWrite) { $firstNonzeroWrite.sequence } else { '' }
            firstNonzeroWritePc = if ($null -ne $firstNonzeroWrite) { $firstNonzeroWrite.instruction_pc } else { '' }
            firstNonzeroLinkCycle = if ($null -ne $firstNonzeroLink) { $firstNonzeroLink.cycle } else { '' }
            targetWrite437Value = $targetWrite.new
            source0244WriteCount = if ($info.buildId -match '^CMPM-build8-square(voice|flow)-') { $sourceWrites.Count } else { '' }
            postNoteFlowRecords = if ($FlowTrace) { $flow.Count } else { '' }
            dsp0PMemorySha256 = (Get-FileHash -LiteralPath (Join-Path $dumpDir 'dsp0_p.hex') -Algorithm SHA256).Hash
        }
    }
}
finally {
    foreach ($name in $names) {
        if ($null -eq $previous[$name]) { Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
}
if ($summary.Count -ne 2 -or
    $summary[0].dsp0PMemorySha256 -ne $summary[1].dsp0PMemorySha256 -or
    $summary[0].firstNonzeroLinkCycle -ne '' -or
    $summary[1].firstNonzeroLinkCycle -eq '') {
    throw "The matched DSP0 program/link control comparison changed; partial data is in $output"
}
$summary | Export-Csv -LiteralPath (Join-Path $output 'comparison.csv') -NoTypeInformation -Encoding Ascii
[ordered]@{
    buildId = $info.buildId
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    romSha256 = (Get-FileHash -LiteralPath $RomPath -Algorithm SHA256).Hash
    squarePatchSha256 = (Get-FileHash -LiteralPath $square -Algorithm SHA256).Hash
    observationWindow = if ($FlowTrace) { 'DSP0 control flow, cycles 236977500 through 236979500, plus established output and voice probes' } else { 'DSP0 cycles 236970000 through 237070000, with JIT and DMA Y output writes plus each link sample' }
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'experiment-info.json') -Encoding UTF8
$zip = $output + '.zip'
Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zip
Write-Output "Bounded Square output comparison: $zip"
