$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Get-Location).Path
if (-not (Test-Path (Join-Path $repo 'plugin\PluginProcessor.cpp'))) { throw 'Run this script from the root of your G1-Emu-VST3 repository.' }
Copy-Item (Join-Path $here 'plugin\PluginProcessor.h') (Join-Path $repo 'plugin\PluginProcessor.h') -Force
Copy-Item (Join-Path $here 'plugin\PluginEditor.h') (Join-Path $repo 'plugin\PluginEditor.h') -Force
Copy-Item (Join-Path $here 'plugin\PluginEditor.cpp') (Join-Path $repo 'plugin\PluginEditor.cpp') -Force
Copy-Item (Join-Path $here '.github\workflows\build.yml') (Join-Path $repo '.github\workflows\build.yml') -Force
& git apply --check (Join-Path $here 'PluginProcessor.cpp.patch')
if ($LASTEXITCODE -ne 0) { throw 'PluginProcessor.cpp patch check failed; no processor changes were applied.' }
& git apply (Join-Path $here 'PluginProcessor.cpp.patch')
if ($LASTEXITCODE -ne 0) { throw 'PluginProcessor.cpp patch failed.' }
Write-Host 'Build #4 applied. Review the changes in GitHub Desktop, then commit/push.'
