# Installs and/or runs the VPK in Vita3K, then prints the tail of both logs.
# Usage: tools\run-vita3k.ps1 [-Install] [-TimeoutSec N] [-Vpk path] [-ScreenshotAt 30,60 -ScreenshotDir dir]
param(
    [switch]$Install,
    [int]$TimeoutSec = 0,
    [string]$Vpk = (Join-Path $PSScriptRoot '..\build-release\NFS3VITA.vpk'),
    [int[]]$ScreenshotAt = @(),
    [string]$ScreenshotDir = (Join-Path $PSScriptRoot '..\build-release\shots')
)

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public class Vita3kWin {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
Add-Type -AssemblyName System.Drawing

# Captures the emulator window only while it shows the game (title starts with the app name).
function Save-GameWindow($process, $path) {
    $process.Refresh()
    if ($process.MainWindowTitle -notlike 'NFS3VITA*') { Write-Host "no game window ($($process.MainWindowTitle))"; return }
    $handle = $process.MainWindowHandle
    [Vita3kWin]::SetForegroundWindow($handle) | Out-Null
    Start-Sleep -Milliseconds 500
    $rect = New-Object RECT
    [Vita3kWin]::GetWindowRect($handle, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { return }
    $bitmap = New-Object System.Drawing.Bitmap $w, $h
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
    Write-Host "screenshot $path"
}

$ErrorActionPreference = 'Stop'
$vita3k = 'E:\DEV\Vita3k\Vita3K.exe'
$pref = (Join-Path $env:APPDATA 'Vita3K\Vita3K')
$titleId = 'NFS3VITA1'
$local = Join-Path $PSScriptRoot 'env.local.ps1'
if (Test-Path $local) { . $local }

$gameLog = Join-Path $pref 'ux0\data\nfs3vita\nfs3vita.log'
$emuLog = Join-Path (Split-Path $vita3k) 'vita3k.log'
if (Test-Path $gameLog) { Remove-Item $gameLog -Force }

# Install by copying the SELF over the installed eboot: the VPK path form asks before overwriting.
if ($Install) {
    $appDir = Join-Path $pref "ux0\app\$titleId"
    $self = Join-Path (Split-Path $Vpk) 'NFS3VITA.self'
    $sfo = Join-Path (Split-Path $Vpk) 'NFS3VITA.vpk_param.sfo'
    New-Item -ItemType Directory -Force (Join-Path $appDir 'sce_sys') | Out-Null
    Copy-Item $self (Join-Path $appDir 'eboot.bin') -Force
    Copy-Item $sfo (Join-Path $appDir 'sce_sys\param.sfo') -Force
    Write-Host "Installed $self -> $appDir\eboot.bin"
}
# GUI mode on purpose: Vita3K's console mode (-z) has no audio backend and crashes on sceAudioOut.
$args = @('-l', '1', '-r', $titleId)
$process = Start-Process -FilePath $vita3k -ArgumentList $args -WorkingDirectory (Split-Path $vita3k) -PassThru
if ($ScreenshotAt.Count -gt 0) { New-Item -ItemType Directory -Force $ScreenshotDir | Out-Null }
foreach ($second in ($ScreenshotAt | Sort-Object)) {
    $wait = $second - [int]((Get-Date) - $process.StartTime).TotalSeconds
    if ($wait -gt 0 -and $process.WaitForExit($wait * 1000)) { break }
    Save-GameWindow $process (Join-Path $ScreenshotDir "shot_$second.png")
}
if ($TimeoutSec -gt 0) {
    $remaining = $TimeoutSec - [int]((Get-Date) - $process.StartTime).TotalSeconds
    if ($remaining -lt 1) { $remaining = 1 }
    if (-not $process.WaitForExit($remaining * 1000)) {
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
