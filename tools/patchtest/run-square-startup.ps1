param(
    [Parameter(Mandatory = $true)][string]$RomPath,
    [string]$OutputDirectory = (Join-Path (Get-Location).Path ('square-startup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [switch]$SettleTrace,
    [ValidateRange(0,299)][int]$FocusMs = 0
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
if (($info.buildId -notlike 'CMPM-build8-squarestartup-*' -and
     $info.buildId -notlike 'CMPM-build8-squaresettle-*' -and
     $info.buildId -notlike 'CMPM-build8-squarehost-*' -and
     $info.buildId -notlike 'CMPM-build8-squareirqboundary-*' -and
     $info.buildId -notlike 'CMPM-build8-squaredmawait-*' -and
     $info.buildId -notlike 'CMPM-build8-squareinternaldma-*' -and
     $info.buildId -notlike 'CMPM-build8-squareessiproducer-*') -or
    $info.executableSha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash) {
    throw 'This bundle is not the matching CMPM-corrected Square startup executable.'
}
if ($SettleTrace -and $info.buildId -notlike 'CMPM-build8-squarehost-*' -and
    $info.buildId -notlike 'CMPM-build8-squareirqboundary-*' -and
    $info.buildId -notlike 'CMPM-build8-squaredmawait-*' -and
    $info.buildId -notlike 'CMPM-build8-squareinternaldma-*' -and
    $info.buildId -notlike 'CMPM-build8-squareessiproducer-*') {
    throw 'The host-port event trace requires a squarehost, squareirqboundary or squaredmawait diagnostic build.'
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

$names = @('G1_MIDINOTE', 'G1_DSP_STARTUP_WATCH_FILE', 'G1_DSP_SETTLE_FILE',
           'G1_DSP_SETTLE_FOCUS_MS', 'G1_DSP_WATCH_BLOCKSIZE',
           'G1_DSP_TRACE', 'G1_DSP_TRACE_FILE', 'G1_DSP_TRACE_START',
           'G1_DSP_TRACE_STEPS', 'G1_DUMP', 'G1_INTERP', 'G1_NO_LA_FIX',
           'G1_KNOBS', 'G1_PREPRESS', 'G1_PRESS', 'G1_HOLD', 'G1_HOLD_END', 'G1_DIAL',
           'G1_DSP_DMA_TRACE_FILE', 'G1_DSP_DMA_TRACE_BEGIN', 'G1_DSP_DMA_TRACE_END',
           'G1_DSP_ESSI_TRACE_FILE')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$allCheckpoints = @()
$settleByCase = @{}
try {
    foreach ($name in $names) {
        Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue
    }
    foreach ($case in @(
        @{ Name = 'Saw-32'; Patch = $fixture; BlockSize = 32; Audible = $true },
        @{ Name = 'Square-32'; Patch = $square; BlockSize = 32; Audible = $false },
        @{ Name = 'Square-1-reference'; Patch = $square; BlockSize = 1; Audible = $true }
    )) {
        $caseDir = Join-Path $output $case.Name
        $dumpDir = Join-Path $caseDir 'dumps'
        New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
        $watch = Join-Path $caseDir 'dsp0-startup.csv'
        $env:G1_MIDINOTE = '1'
        $env:G1_DSP_STARTUP_WATCH_FILE = $watch
        if ($SettleTrace) {
            $env:G1_DSP_SETTLE_FILE = Join-Path $caseDir 'dsp0-settle.csv'
            $env:G1_DSP_SETTLE_FOCUS_MS = [string]$FocusMs
        }
        Remove-Item -LiteralPath Env:G1_DSP_DMA_TRACE_FILE, Env:G1_DSP_DMA_TRACE_BEGIN, Env:G1_DSP_DMA_TRACE_END, Env:G1_DSP_ESSI_TRACE_FILE -ErrorAction SilentlyContinue
        if ($SettleTrace -and ($info.buildId -like 'CMPM-build8-squareinternaldma-*' -or
            $info.buildId -like 'CMPM-build8-squareessiproducer-*') -and $FocusMs -eq 25) {
            $env:G1_DSP_DMA_TRACE_FILE = Join-Path $caseDir 'dsp0-dma3-internal.csv'
            if ($info.buildId -like 'CMPM-build8-squareessiproducer-*') {
                $env:G1_DSP_ESSI_TRACE_FILE = Join-Path $caseDir 'dsp0-essi1-producer.csv'
            }
            if ($case.BlockSize -eq 32 -and $case.Name -eq 'Square-32') {
                $env:G1_DSP_DMA_TRACE_BEGIN = '214241712'
                $env:G1_DSP_DMA_TRACE_END = '214241766'
            } elseif ($case.Name -eq 'Square-1-reference') {
                $env:G1_DSP_DMA_TRACE_BEGIN = '214239700'
                $env:G1_DSP_DMA_TRACE_END = '214239930'
            } else {
                Remove-Item -LiteralPath Env:G1_DSP_DMA_TRACE_FILE, Env:G1_DSP_ESSI_TRACE_FILE -ErrorAction SilentlyContinue
            }
        }
        $env:G1_DUMP = $dumpDir
        Remove-Item -LiteralPath Env:G1_DSP_WATCH_BLOCKSIZE -ErrorAction SilentlyContinue
        if ($case.BlockSize -eq 1) { $env:G1_DSP_WATCH_BLOCKSIZE = '1' }
        $result = & $exe $RomPath $case.Patch --modules $modules --note 60 --seconds 0.25 2>&1
        $result | Out-File -LiteralPath (Join-Path $caseDir 'run.txt') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name) exited with $LASTEXITCODE; partial data is in $output" }
        $text = $result -join "`n"
        if ($text -notmatch 'pid=1' -or $text -notmatch '\( 4\)') {
            throw "$($case.Name) did not upload as a four-voice patch; partial data is in $output"
        }
        if (($case.Audible -and $text -notmatch 'output 1: peak') -or
            (-not $case.Audible -and $text -notmatch 'output 1: silence')) {
            throw "$($case.Name) audio did not reproduce the expected result; partial data is in $output"
        }
        if ((Get-Item -LiteralPath $watch).Length -lt 200) {
            throw "$($case.Name) did not record startup checkpoints."
        }
        if ($SettleTrace -and ((Get-Item -LiteralPath $env:G1_DSP_SETTLE_FILE).Length -lt 1000)) {
            throw "$($case.Name) did not record post-upload settling samples."
        }
        if ($SettleTrace -and ((Get-Item -LiteralPath ($env:G1_DSP_SETTLE_FILE + '.blocks.csv')).Length -lt 1000)) {
            throw "$($case.Name) did not record focused DSP0 blocks."
        }
        if ($SettleTrace -and ($info.buildId -like 'CMPM-build8-squareinternaldma-*' -or
            $info.buildId -like 'CMPM-build8-squareessiproducer-*') -and
            $FocusMs -eq 25 -and $case.Name -ne 'Saw-32' -and
            (-not (Test-Path -LiteralPath $env:G1_DSP_DMA_TRACE_FILE -PathType Leaf) -or
             (Get-Item -LiteralPath $env:G1_DSP_DMA_TRACE_FILE).Length -lt 100)) {
            throw "$($case.Name) did not record the internal DMA3 request/injection window."
        }
        if ($SettleTrace -and $info.buildId -like 'CMPM-build8-squareessiproducer-*' -and
            $FocusMs -eq 25 -and $case.Name -ne 'Saw-32' -and
            (-not (Test-Path -LiteralPath $env:G1_DSP_ESSI_TRACE_FILE -PathType Leaf) -or
             (Get-Item -LiteralPath $env:G1_DSP_ESSI_TRACE_FILE).Length -lt 100)) {
            throw "$($case.Name) did not record the ESSI1 clock producer window."
        }
        if ($SettleTrace) {
            foreach ($suffix in @('.events.csv', '.cpu-host.csv')) {
                if ((Get-Item -LiteralPath ($env:G1_DSP_SETTLE_FILE + $suffix)).Length -lt 1000) {
                    throw "$($case.Name) did not record $suffix host-port events."
                }
            }
            if ($FocusMs -eq 25 -and ($info.buildId -like 'CMPM-build8-squareirqboundary-*' -or
                $info.buildId -like 'CMPM-build8-squaredmawait-*' -or
                $info.buildId -like 'CMPM-build8-squareinternaldma-*' -or
                $info.buildId -like 'CMPM-build8-squareessiproducer-*') -and
                (Get-Item -LiteralPath ($env:G1_DSP_SETTLE_FILE + '.host-blocks.csv')).Length -lt 1000) {
                throw "$($case.Name) did not record word-195 interrupt boundaries."
            }
            if ($FocusMs -eq 25 -and ($info.buildId -like 'CMPM-build8-squaredmawait-*' -or
                $info.buildId -like 'CMPM-build8-squareinternaldma-*' -or
                $info.buildId -like 'CMPM-build8-squareessiproducer-*') -and
                (Get-Item -LiteralPath ($env:G1_DSP_SETTLE_FILE + '.host-waits.csv')).Length -lt 1000) {
                throw "$($case.Name) did not record the word-195 host-command waits."
            }
        }
        if ($SettleTrace) {
            $settleByCase[$case.Name] = @(Import-Csv -LiteralPath $env:G1_DSP_SETTLE_FILE)
            if ($settleByCase[$case.Name].Count -lt 300) {
                throw "$($case.Name) has too few post-upload millisecond samples."
            }
        }
        $checkpoints = @(Import-Csv -LiteralPath $watch | Where-Object kind -eq 'checkpoint')
        $expected = @('before_upload', 'upload_packet_1', 'after_packet_1',
                      'upload_packet_2', 'after_packet_2',
                      'after_dsp_load', 'before_note', 'note_on', 'capture_end')
        foreach ($stage in $expected) {
            if ($stage -notin @($checkpoints | ForEach-Object stage)) {
                throw "$($case.Name) is missing startup checkpoint $stage."
            }
        }
        $checkpoints | Select-Object stage,call,pre_pc,pre_cycles,pre_x1d,pre_x1e,pre_x1f |
            Format-Table -AutoSize | Out-String |
            Set-Content -LiteralPath (Join-Path $caseDir 'checkpoints.txt') -Encoding Ascii
        foreach ($row in $checkpoints) {
            $allCheckpoints += [pscustomobject]@{
                case = $case.Name
                stage = $row.stage
                call = $row.call
                pc = $row.pre_pc
                cycles = $row.pre_cycles
                x1d = $row.pre_x1d
                x1e = $row.pre_x1e
                x1f = $row.pre_x1f
            }
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

$allCheckpoints | Export-Csv -LiteralPath (Join-Path $output 'checkpoint-comparison.csv') -NoTypeInformation -Encoding Ascii
if ($SettleTrace) {
    $comparison = for ($i = 1; $i -lt 300; $i++) {
        $deltas = @{}
        foreach ($name in @('Saw-32', 'Square-32', 'Square-1-reference')) {
            $rows = $settleByCase[$name]
            $deltas[$name] = [long]$rows[$i].dsp_cycles - [long]$rows[$i - 1].dsp_cycles
        }
        $squareSample = $settleByCase['Square-32'][$i]
        [pscustomobject]@{
            millisecond = $i
            saw32_cycles = $deltas['Saw-32']
            square32_cycles = $deltas['Square-32']
            square1_cycles = $deltas['Square-1-reference']
            square32_excess_over_both = $deltas['Square-32'] - [Math]::Max($deltas['Saw-32'], $deltas['Square-1-reference'])
            square32_pc = $squareSample.pc
            square32_top_pc = $squareSample.top_pc
            square32_top_pc_blocks = $squareSample.top_pc_blocks
            square32_catchup_cycles = $squareSample.catchup_cycles
            square32_hostword_cycles = $squareSample.hostword_cycles
            square32_hostcommand_cycles = $squareSample.hostcommand_cycles
            square32_readisr_cycles = $squareSample.readisr_cycles
            square32_rxempty_cycles = $squareSample.rxempty_cycles
        }
    }
    $comparison | Export-Csv -LiteralPath (Join-Path $output 'settle-comparison.csv') -NoTypeInformation -Encoding Ascii
    $firstExcess = $comparison | Where-Object { $_.square32_excess_over_both -gt 10000 } | Select-Object -First 1
    if ($null -ne $firstExcess) {
        $firstExcess | Format-List | Out-String | Set-Content -LiteralPath (Join-Path $output 'first-excess.txt') -Encoding Ascii
    }
}
[ordered]@{
    buildId = $info.buildId
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    romSha256 = (Get-FileHash -LiteralPath $RomPath -Algorithm SHA256).Hash
    sawPatchSha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
    squarePatchSha256 = (Get-FileHash -LiteralPath $square -Algorithm SHA256).Hash
    failingDsp0MaxInstructionsPerBlock = 32
    referenceDsp0MaxInstructionsPerBlock = 1
    settleTrace = [bool]$SettleTrace
    focusMs = $FocusMs
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'startup-info.json') -Encoding UTF8
$zip = "$output.zip"
Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zip
Write-Output "Matched Square startup watch: $zip"
