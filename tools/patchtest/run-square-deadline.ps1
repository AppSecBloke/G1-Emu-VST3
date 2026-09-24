param(
    [Parameter(Mandatory = $true)][string]$RomPath,
    [string]$OutputDirectory = (Join-Path (Get-Location).Path ('square-deadline-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
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
if ($info.buildId -notmatch '^CMPM-build8-squaredeadline-' -or
    $info.executableSha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash) {
    throw 'This bundle is not the deadline-aware JIT diagnostic executable.'
}
if ((Get-Item -LiteralPath $RomPath).Length -ne 524288) { throw 'The G1 ROM must be exactly 524288 bytes.' }
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

$names = @('G1_MIDINOTE', 'G1_DSP_WATCH_BLOCKSIZE', 'G1_DSP_DEADLINE_PROBE',
           'G1_DSP_FINE_DRAIN_FILE', 'G1_DSP_STARTUP_WATCH_FILE',
           'G1_DSP_CALLBACK_WINDOW_FILE', 'G1_DSP_DISPATCH_TRACE_FILE',
           'G1_DSP_IRQD_TRACE_FILE', 'G1_DSP_CAUSAL_HOST_TRACE_FILE',
           'G1_DSP_CAUSAL_LINK_TRACE_FILE', 'G1_DSP_VECTOR7E_TRACE_FILE',
           'G1_DSP_SETTLE_FILE', 'G1_DSP_ESSI_TIMELINE_FILE',
           'G1_DSP_DMA_TRACE_FILE', 'G1_DSP_ESSI_TRACE_FILE',
           'G1_DSP_TRACE', 'G1_DSP_TRACE_FILE', 'G1_INTERP', 'G1_DUMP')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$summary = @()
try {
    foreach ($name in $names) { Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
    foreach ($case in @(
        @{ Name = 'Saw-32'; Patch = $fixture; Single = $false; Probe = $false },
        @{ Name = 'Square-1'; Patch = $square; Single = $true; Probe = $false },
        @{ Name = 'Square-32-deadline'; Patch = $square; Single = $false; Probe = $true }
    )) {
        $caseDir = Join-Path $output $case.Name
        $dumpDir = Join-Path $caseDir 'dumps'
        New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
        $env:G1_MIDINOTE = '1'
        $env:G1_DUMP = $dumpDir
        if ($case.Single) { $env:G1_DSP_WATCH_BLOCKSIZE = '1' }
        if ($case.Probe) { $env:G1_DSP_DEADLINE_PROBE = '1' }
        $result = & $exe $RomPath $case.Patch --modules $modules --note 60 --seconds 0.25 2>&1
        $result | Out-File -LiteralPath (Join-Path $caseDir 'run.txt') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name) exited with $LASTEXITCODE; partial data is in $output" }
        $text = $result -join "`n"
        if ($text -notmatch 'pid=1' -or $text -notmatch '\( 4\)') {
            throw "$($case.Name) did not upload as a four-voice patch; partial data is in $output"
        }
        if ($text -notmatch 'output 1: peak') {
            throw "$($case.Name) produced no output-1 audio; partial data is in $output"
        }
        $link = [regex]::Match($text, 'DSP0 -> DSP1:([^\r\n]+)')
        if (-not $link.Success -or $link.Groups[1].Value -notmatch '-?\d') {
            throw "$($case.Name) has no nonzero DSP0-to-DSP1 link peak; partial data is in $output"
        }
        $probe = $null
        if ($case.Probe) {
            $probe = [regex]::Match($text, 'deadline probe: long blocks (\d+), multi-instruction long blocks (\d+), short blocks (\d+), IRQD overruns (\d+)')
            $host = [regex]::Match($text, 'DSP0 host: words (\d+), commands (\d+), serviced \$7E (\d+), serviced \$76 (\d+)')
            $dropped = [regex]::Match($text, 'DSP0 dropped commands: \$7E (\d+), \$76 (\d+)')
            if (-not $probe.Success -or -not $host.Success -or -not $dropped.Success) {
                throw "Missing deadline/host counters; partial data is in $output"
            }
            if ([long]$probe.Groups[1].Value -eq 0 -or [long]$probe.Groups[2].Value -eq 0 -or
                [long]$probe.Groups[3].Value -eq 0 -or [long]$probe.Groups[4].Value -ne 0) {
                throw "The probe did not retain multi-instruction blocks or crossed an IRQD deadline; partial data is in $output"
            }
            if ([long]$host.Groups[1].Value -le 4211 -or [long]$host.Groups[2].Value -le 4395 -or
                [long]$host.Groups[3].Value -eq 0 -or [long]$host.Groups[4].Value -eq 0 -or
                [long]$dropped.Groups[1].Value -ne 0 -or [long]$dropped.Groups[2].Value -ne 0) {
                throw "Square-32 host traffic did not recover; partial data is in $output"
            }
        }
        $summary += [pscustomobject]@{
            case = $case.Name
            initialBlockLimit = if ($case.Single) { 1 } else { 32 }
            deadlineProbe = $case.Probe
            output1Audible = $true
            dsp0ToDsp1Nonzero = $true
            multiInstructionLongBlocks = if ($null -ne $probe) { [long]$probe.Groups[2].Value } else { '' }
            dsp0PMemorySha256 = (Get-FileHash -LiteralPath (Join-Path $dumpDir 'dsp0_p.hex') -Algorithm SHA256).Hash
        }
        Remove-Item -LiteralPath Env:G1_DSP_WATCH_BLOCKSIZE, Env:G1_DSP_DEADLINE_PROBE -ErrorAction SilentlyContinue
    }
}
finally {
    foreach ($name in $names) {
        if ($null -eq $previous[$name]) { Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
}
$summary | Export-Csv -LiteralPath (Join-Path $output 'comparison.csv') -NoTypeInformation -Encoding Ascii
[ordered]@{
    buildId = $info.buildId
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    romSha256 = (Get-FileHash -LiteralPath $RomPath -Algorithm SHA256).Hash
    mechanism = 'DSP0 diagnostic runtime JIT block variants: one instruction near dispatcher deadlines, 32 otherwise'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'experiment-info.json') -Encoding UTF8
$zip = $output + '.zip'
Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zip
Write-Output "Deadline-aware Square comparison: $zip"
