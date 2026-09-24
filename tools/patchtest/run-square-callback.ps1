param(
    [Parameter(Mandatory = $true)][string]$RomPath,
    [string]$OutputDirectory = (Join-Path (Get-Location).Path ('square-callback-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
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
if (($info.buildId -notlike 'CMPM-build8-squarecallback-*' -and
     $info.buildId -notlike 'CMPM-build8-squarefinedrain-*') -or
    $info.executableSha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash) {
    throw 'This bundle is not the matching bounded callback diagnostic executable.'
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

$names = @('G1_MIDINOTE', 'G1_DSP_WATCH_BLOCKSIZE', 'G1_DSP_STARTUP_WATCH_FILE',
           'G1_DSP_CALLBACK_WINDOW_FILE', 'G1_DSP_DISPATCH_TRACE_FILE',
           'G1_DSP_DISPATCH_TRACE_BEGIN', 'G1_DSP_DISPATCH_TRACE_END',
           'G1_DSP_ESSI_TIMELINE_FILE', 'G1_DSP_ESSI_TIMELINE_BEGIN',
           'G1_DSP_ESSI_TIMELINE_END', 'G1_DSP_IRQD_TRACE_FILE',
           'G1_DSP_DMA_TRACE_FILE', 'G1_DSP_ESSI_TRACE_FILE', 'G1_DUMP')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $names) { Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
    foreach ($case in @(
        @{ Name = 'Square-32'; BlockSize = 32; Audible = $false },
        @{ Name = 'Square-1-reference'; BlockSize = 1; Audible = $true }
    )) {
        $caseDir = Join-Path $output $case.Name
        New-Item -ItemType Directory -Path $caseDir | Out-Null
        $env:G1_MIDINOTE = '1'
        $env:G1_DSP_CALLBACK_WINDOW_FILE = Join-Path $caseDir 'dsp0-callback.csv'
        $env:G1_DSP_DISPATCH_TRACE_FILE = Join-Path $caseDir 'dsp0-dispatch.csv'
        $env:G1_DSP_DISPATCH_TRACE_BEGIN = '211258184'
        $env:G1_DSP_DISPATCH_TRACE_END = '211258368'
        $env:G1_DSP_IRQD_TRACE_FILE = Join-Path $caseDir 'dsp0-irqd.csv'
        Remove-Item -LiteralPath Env:G1_DSP_WATCH_BLOCKSIZE -ErrorAction SilentlyContinue
        if ($case.BlockSize -eq 1) { $env:G1_DSP_WATCH_BLOCKSIZE = '1' }

        $result = & $exe $RomPath $square --modules $modules --note 60 --seconds 0.25 2>&1
        $result | Out-File -LiteralPath (Join-Path $caseDir 'run.txt') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name) exited with $LASTEXITCODE; partial data is in $output" }
        $text = $result -join "`n"
        if ($text -notmatch 'pid=1' -or $text -notmatch '\( 4\)' -or
            ($case.Audible -and $text -notmatch 'output 1: peak') -or
            (-not $case.Audible -and $text -notmatch 'output 1: silence')) {
            throw "$($case.Name) did not reproduce its matched upload/audio result; partial data is in $output"
        }
        foreach ($suffix in @('', '.clock.csv', '.dma.csv')) {
            $path = $env:G1_DSP_CALLBACK_WINDOW_FILE + $suffix
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                (Get-Item -LiteralPath $path).Length -lt 100) {
                throw "$($case.Name) did not record $path; partial data is in $output"
            }
        }
        $events = @(Import-Csv -LiteralPath $env:G1_DSP_CALLBACK_WINDOW_FILE)
        if (@($events | Where-Object { $_.event -eq 'callback_entry' -and
                    $_.cycle -eq '211258189' -and $_.pc -eq '366' }).Count -eq 0 -or
            @($events | Where-Object { $_.event -eq 'callback_exit' -and
                    $_.cycle -eq '211258189' -and $_.pc -eq '366' }).Count -eq 0) {
            throw "$($case.Name) is missing the aligned callback entry/exit; partial data is in $output"
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

$zip = $output + '.zip'
Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zip
Write-Output "Bounded Square callback trace: $zip"
