# Builds the VPK. Usage: tools\build.ps1 [-Config Release|Debug] [-Clean] [-Reconfigure] [-Jobs N]
# Configures only when the build directory is new (Ninja re-runs CMake itself when CMake files change), then
# rebuilds only what changed.
param(
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Reconfigure,
    [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $repo "build-$($Config.ToLower())"

if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }

if ($Reconfigure -or -not (Test-Path (Join-Path $build 'build.ninja'))) {
    $options = @("-DCMAKE_TOOLCHAIN_FILE=$env:VITASDK/share/vita.toolchain.cmake", "-DCMAKE_BUILD_TYPE=$Config")
    $options += if ($env:SCE_PSP2_SDK_DIR) { "-DSCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR" } else { '-USCE_PSP2_SDK_DIR' }
    cmake -S $repo -B $build -G Ninja @options
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
}

cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host "Output: $(Join-Path $build 'NFS3VITA.vpk')"
