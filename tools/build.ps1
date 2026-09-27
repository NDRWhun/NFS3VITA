# Configures and builds the VPK. Usage: tools\build.ps1 [-Config Release|Debug] [-Clean] [-Jobs N]
param(
    [string]$Config = 'Release',
    [switch]$Clean,
    [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $repo "build-$($Config.ToLower())"

if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }

cmake -S $repo -B $build -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$env:VITASDK/share/vita.toolchain.cmake" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DSCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR"
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "Output: $build\NFS3VITA.vpk"
