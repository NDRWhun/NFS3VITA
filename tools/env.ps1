# Sets up the toolchain environment for this shell. Dot-source it: . tools\env.ps1
# Override paths in tools\env.local.ps1 (ignored by git).

$env:VITASDK = 'E:\vitasdk'
$env:SCE_PSP2_SDK_DIR = 'h:\Downloads\psvita-sdk-3.570-2019lk\SDK Software\PSVITA\sdk'

$local = Join-Path $PSScriptRoot 'env.local.ps1'
if (Test-Path $local) { . $local }

if (-not (Test-Path "$env:VITASDK\bin\arm-vita-eabi-gcc.exe")) { throw "VITASDK not found at $env:VITASDK" }
if (-not (Test-Path "$env:SCE_PSP2_SDK_DIR\host_tools\bin\psp2cgc.exe")) { throw "psp2cgc not found under $env:SCE_PSP2_SDK_DIR" }

if ($env:Path -notlike "*$env:VITASDK\bin*") { $env:Path = "$env:VITASDK\bin;$env:Path" }
Write-Host "VITASDK=$env:VITASDK"
Write-Host "SCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR"
