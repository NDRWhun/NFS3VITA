# Installs and/or runs the VPK in Vita3K, then prints the tail of both logs.
# Usage: tools\run-vita3k.ps1 [-Install] [-TimeoutSec N] [-Vpk path]
param(
    [switch]$Install,
    [int]$TimeoutSec = 0,
    [string]$Vpk = (Join-Path $PSScriptRoot '..\build-release\NFS3VITA.vpk')
)

$ErrorActionPreference = 'Stop'
$vita3k = 'E:\DEV\Vita3k\Vita3K.exe'
$pref = (Join-Path $env:APPDATA 'Vita3K\Vita3K')
$titleId = 'NFS3VITA1'
$local = Join-Path $PSScriptRoot 'env.local.ps1'
if (Test-Path $local) { . $local }

$gameLog = Join-Path $pref 'ux0\data\nfs3vita\nfs3vita.log'
$emuLog = Join-Path (Split-Path $vita3k) 'vita3k.log'
if (Test-Path $gameLog) { Remove-Item $gameLog -Force }

# GUI mode on purpose: Vita3K's console mode (-z) has no audio backend and crashes on sceAudioOut.
$args = if ($Install) { @('-l', '1', (Resolve-Path $Vpk)) } else { @('-l', '1', '-r', $titleId) }
$process = Start-Process -FilePath $vita3k -ArgumentList $args -WorkingDirectory (Split-Path $vita3k) -PassThru
if ($TimeoutSec -gt 0) {
    if (-not $process.WaitForExit($TimeoutSec * 1000)) {
        Write-Host "Timeout after $TimeoutSec s, stopping emulator"
        $process.Kill()
    }
} else {
    $process.WaitForExit()
}

Write-Host "=== $gameLog ==="
if (Test-Path $gameLog) { Get-Content $gameLog -Tail 60 } else { Write-Host '(no game log written)' }
Write-Host "=== $emuLog (errors) ==="
if (Test-Path $emuLog) { Get-Content $emuLog | Select-String -Pattern 'error|Error|warn|crash|unimpl|nid' | Select-Object -Last 40 }
