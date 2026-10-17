# Sets up the toolchain environment for this shell. Dot-source it: . tools\env.ps1
# Machine-specific settings come from the environment or from tools\env.local.ps1 (ignored by git):
#   $env:VITASDK          = '<VitaSDK root>'                    required
#   $env:SCE_PSP2_SDK_DIR = '<official PSP2 SDK root>'          optional: recompiles shaders with psp2cgc

$local = Join-Path $PSScriptRoot 'env.local.ps1'
if (Test-Path $local) { . $local }

if (-not $env:VITASDK -or -not (Test-Path (Join-Path $env:VITASDK 'share/vita.toolchain.cmake'))) {
    throw 'Set VITASDK to your VitaSDK root (environment variable or tools\env.local.ps1).'
}
$bin = Join-Path $env:VITASDK 'bin'
if (($env:Path -split [IO.Path]::PathSeparator) -notcontains $bin) { $env:Path = $bin + [IO.Path]::PathSeparator + $env:Path }

$missing = @()
foreach ($tool in @(@{ Name = 'cmake'; Url = 'https://cmake.org/download/' }, @{ Name = 'ninja'; Url = 'https://github.com/ninja-build/ninja/releases' })) {
    if (-not (Get-Command $tool.Name -ErrorAction SilentlyContinue)) { $missing += "$($tool.Name) ($($tool.Url))" }
}
if ($missing) { throw "Not found on PATH: $($missing -join ', ')" }

Write-Host "VITASDK=$env:VITASDK"
if ($env:SCE_PSP2_SDK_DIR) { Write-Host "SCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR (shaders are compiled)" }
else { Write-Host 'SCE_PSP2_SDK_DIR not set: using the prebuilt shaders in shaders/gxp' }
