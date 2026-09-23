# Run from the CI diagnostic bundle with Windows PowerShell 5.1 or PowerShell 7.
[CmdletBinding()]
param(
    [string] $RomPath,
    [string] $OutputDirectory = (Join-Path (Get-Location) ('g1-matched-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [string] $BundleDirectory = $PSScriptRoot,
    [ValidateRange(10, 3600)][int] $TimeoutSeconds = 180,
    [switch] $PrepareOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$bundle = (Resolve-Path -LiteralPath $BundleDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath ($output + '.zip'))) {
    throw 'Choose a new output directory; existing results will not be overwritten.'
}
$sourcePatch = Join-Path $bundle 'SimpleOSC.pch'
$modules = Join-Path $bundle 'modules.xml'
$tool = Join-Path $bundle 'g1patchtest.exe'
foreach ($file in @($sourcePatch, $modules, (Join-Path $bundle 'build-info.json'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing bundle file: $file" }
}

# Fail rather than accidentally testing a different oscillator or changing another value.
# Latin-1 is a byte-preserving round trip, including the source file's line endings.
$encoding = [Text.Encoding]::GetEncoding(28591)
$original = [IO.File]::ReadAllBytes($sourcePatch)
$text = $encoding.GetString($original)
$pattern = '(?m)^(1 7 10 64 64 64 64 )0( 0 0 0 0 0[ \t]*\r?$)'
$matches = [regex]::Matches($text, $pattern)
if ($matches.Count -ne 1) { throw 'SimpleOSC must have exactly one expected OscA parameter row.' }
$sawText = [regex]::Replace($text, $pattern, '${1}2${2}')
$sawBytes = $encoding.GetBytes($sawText)
$changed = @()
for ($i = 0; $i -lt $original.Length; $i++) {
    if ($original[$i] -ne $sawBytes[$i]) { $changed += $i }
}
if ($original.Length -ne $sawBytes.Length -or $changed.Count -ne 1 -or
    $original[$changed[0]] -ne 48 -or $sawBytes[$changed[0]] -ne 50) {
    throw 'The generated Saw fixture must differ by exactly one byte: waveform 0 -> 2.'
}

$romHash = $null
if (-not $PrepareOnly) {
    if (-not $RomPath) { throw 'Supply -RomPath pointing to your own 512 KB G1 rack ROM.' }
    $rom = (Resolve-Path -LiteralPath $RomPath).Path
    $romBytes = [IO.File]::ReadAllBytes($rom)
    if ($romBytes.Length -ne 524288 -or $romBytes[0x7ff] -ne 1 -or
        -not $encoding.GetString($romBytes).Contains('NORD MODULAR')) {
        throw 'Expected a 512 KB Nord Modular rack ROM (same checks as g1rom.h).'
    }
    $romHash = (Get-FileHash -LiteralPath $rom -Algorithm SHA256).Hash
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw 'Missing g1patchtest.exe.' }
}

New-Item -ItemType Directory -Path $output | Out-Null
Copy-Item -LiteralPath (Join-Path $bundle 'build-info.json') -Destination $output
Copy-Item -LiteralPath $modules -Destination $output
[IO.File]::WriteAllBytes((Join-Path $output 'SimpleOSC.pch'), $original)
[IO.File]::WriteAllBytes((Join-Path $output 'SimpleOSC-Saw.pch'), $sawBytes)
$manifest = [ordered]@{
    schema = 1
    createdUtc = [DateTime]::UtcNow.ToString('o')
    prepareOnly = [bool]$PrepareOnly
    romSha256 = $romHash
    modulesSha256 = (Get-FileHash -LiteralPath $modules -Algorithm SHA256).Hash
    note = 60
    velocity = 100
    midiChannel = 1
    seconds = 2
    threads = 1
    timeoutSeconds = $TimeoutSeconds
    changedPatchByteOffset = $changed[0]
    cases = @()
}
if (-not $PrepareOnly) {
    foreach ($case in @('SimpleOSC', 'SimpleOSC-Saw')) {
        $caseDir = Join-Path $output $case
        $dumpDir = Join-Path $caseDir 'dumps'
        New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
        $patch = Join-Path $output ($case + '.pch')
        $wav = Join-Path $caseDir 'output.wav'
        $result = [ordered]@{
            patch = $case + '.pch'
            patchSha256 = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash
            exitCode = $null
            timedOut = $false
            complete = $false
            dumpCount = 0
            error = $null
        }
        try {
            if ((Get-FileHash -LiteralPath $rom -Algorithm SHA256).Hash -ne $romHash) {
                throw 'The ROM changed between matched runs.'
            }
            $info = New-Object Diagnostics.ProcessStartInfo
            $info.FileName = $tool
            # Windows file paths cannot contain quotes; none of these arguments ends in a slash.
            $arguments = @($rom, $patch, '--modules', $modules, '--note', '60', '--seconds', '2', '--wav', $wav)
            $info.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
            $info.WorkingDirectory = $bundle
            $info.UseShellExecute = $false
            $info.CreateNoWindow = $true
            $info.RedirectStandardOutput = $true
            $info.RedirectStandardError = $true
            # Do not inherit panel probes, interpreter switches or other G1 experiment settings.
            foreach ($key in @($info.EnvironmentVariables.Keys)) {
                if ($key -like 'G1_*') { $info.EnvironmentVariables.Remove($key) }
            }
            $info.EnvironmentVariables['G1_VERBOSE'] = '1'
            $info.EnvironmentVariables['G1_THREADS'] = '1'
            $info.EnvironmentVariables['G1_MIDINOTE'] = '1'
            $info.EnvironmentVariables['G1_DUMP'] = $dumpDir
            $process = New-Object Diagnostics.Process
            $process.StartInfo = $info
            try {
                if (-not $process.Start()) { throw 'Could not start g1patchtest.' }
                $stdout = $process.StandardOutput.ReadToEndAsync()
                $stderr = $process.StandardError.ReadToEndAsync()
                if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
                    $result.timedOut = $true
                    $process.Kill()
                }
                $process.WaitForExit()
                $result.exitCode = $process.ExitCode
                $log = $stdout.GetAwaiter().GetResult()
                [IO.File]::WriteAllText((Join-Path $caseDir 'stdout.txt'), $log)
                [IO.File]::WriteAllText((Join-Path $caseDir 'stderr.txt'), $stderr.GetAwaiter().GetResult())
                $measurements = $log -split '\r?\n' | Where-Object {
                    $_ -match '^outputs |^  output |^links |^  DSP\d+ -> DSP\d+:'
                }
                $measurements | Set-Content -LiteralPath (Join-Path $caseDir 'measurements.txt')
                $dumps = @(Get-ChildItem -LiteralPath $dumpDir -Filter '*.hex' -File)
                $result.dumpCount = $dumps.Count
                $result.complete = $result.exitCode -eq 0 -and -not $result.timedOut -and
                    $dumps.Count -eq 12 -and (Test-Path -LiteralPath $wav) -and
                    $log.Contains('outputs (') -and $log.Contains('links (')
            }
            finally { $process.Dispose() }
        }
        catch { $result.error = $_.Exception.Message }
        $manifest.cases += $result
        Write-Host "$case : exit=$($result.exitCode), timeout=$($result.timedOut), complete=$($result.complete)"
    }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'run-info.json') -Encoding UTF8
# Only the new results directory is archived. The ROM is never copied into it.
Compress-Archive -LiteralPath $output -DestinationPath ($output + '.zip')
Write-Host "Results: $output.zip"
if (-not $PrepareOnly -and @($manifest.cases | Where-Object { -not $_.complete }).Count -gt 0) {
    throw 'At least one run failed or timed out. Both were attempted; inspect the results ZIP.'
}
