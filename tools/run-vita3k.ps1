# Installs and/or runs the VPK in Vita3K, then prints the tail of both logs.
# Usage: tools\run-vita3k.ps1 [-Install] [-TimeoutSec N] [-Vpk path] [-ScreenshotAt 30,60 -ScreenshotDir dir]
#                             [-AutoInput script.txt] [-Vita3K path]
# -AutoInput copies a "<ms> <key>" script to the data dir for the game to replay (see lib/inputscript.h).
# -Vita3K defaults to $env:VITA3K_EXE (tools\env.local.ps1); the data path is Vita3K's pref-path.
param(
    [switch]$Install,
    [int]$TimeoutSec = 0,
    [string]$Vpk = (Join-Path $PSScriptRoot '..\build-release\NFS3VITA.vpk'),
    [int[]]$ScreenshotAt = @(),
    [string]$ScreenshotDir = (Join-Path $PSScriptRoot '..\build-release\shots'),
    [string]$AutoInput = '',
    [string]$Vita3K = ''
)

Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public class Vita3kWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);

    // Visible top-level window of the process whose title starts with the prefix, or zero.
    public static IntPtr Find(uint pid, string prefix) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner != pid || !IsWindowVisible(h)) return true;
            StringBuilder title = new StringBuilder(256);
            GetWindowText(h, title, title.Capacity);
            if (!title.ToString().StartsWith(prefix)) return true;
            found = h; return false;
        }, IntPtr.Zero);
        return found;
    }
}
'@
Add-Type -AssemblyName System.Drawing

# Captures the game window's client area without bringing it to the front (PrintWindow, full content).
function Save-GameWindow($process, $path) {
    $handle = [Vita3kWin]::Find([uint32]$process.Id, 'NFS3VITA')
    if ($handle -eq [IntPtr]::Zero) { Write-Host "no game window at $path"; return }
    $rect = New-Object RECT
    [Vita3kWin]::GetClientRect($handle, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { Write-Host "game window minimized at $path"; return }
    $bitmap = New-Object System.Drawing.Bitmap $w, $h
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    $ok = [Vita3kWin]::PrintWindow($handle, $hdc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $graphics.ReleaseHdc($hdc)
    if ($ok) { $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); Write-Host "screenshot $path" }
    else { Write-Host "capture failed at $path" }
    $graphics.Dispose(); $bitmap.Dispose()
}

# Vita3K's pref-path from the config.yml next to the executable, else its default location.
function Get-Vita3kPrefPath($exe) {
    $yml = Join-Path (Split-Path $exe) 'config.yml'
    if (Test-Path $yml) {
        $line = Get-Content $yml | Where-Object { $_ -match '^pref-path:\s*(\S.*)$' } | Select-Object -First 1
        if ($line -and $Matches[1].Trim() -notin @("''", '""')) { return $Matches[1].Trim() }
    }
    if ($IsLinux) { return Join-Path $HOME '.local/share/Vita3K/Vita3K' }
    if ($IsMacOS) { return Join-Path $HOME 'Library/Application Support/Vita3K/Vita3K' }
    return Join-Path $env:APPDATA 'Vita3K\Vita3K'
}

$ErrorActionPreference = 'Stop'
$local = Join-Path $PSScriptRoot 'env.local.ps1'
if (Test-Path $local) { . $local }
$vita3k = if ($Vita3K) { $Vita3K } else { $env:VITA3K_EXE }
if (-not $vita3k -or -not (Test-Path $vita3k)) {
    throw 'Set VITA3K_EXE (environment variable or tools\env.local.ps1) or pass -Vita3K <path to the Vita3K executable>.'
}
$pref = Get-Vita3kPrefPath $vita3k
$titleId = 'NFS3VITA1'

$dataDir = Join-Path $pref 'ux0\data\nfs3vita'
$gameLog = Join-Path $dataDir 'nfs3vita.log'
$emuLog = Join-Path (Split-Path $vita3k) 'vita3k.log'
if (Test-Path $gameLog) { Remove-Item $gameLog -Force }

# Vita3K reads the 565 texture swizzle the other way round than hardware; the game has a switch for it.
$config = Join-Path $dataDir 'nfs3vita.cfg'
if (-not (Test-Path $config)) { Set-Content -Path $config -Value "vita3k_bgr565=1" -Encoding ascii }

# A stale input script would replay into every run, so it only exists when asked for.
$autoInputTarget = Join-Path $dataDir 'autoinput.txt'
if (Test-Path $autoInputTarget) { Remove-Item $autoInputTarget -Force }
if ($AutoInput) { Copy-Item $AutoInput $autoInputTarget }

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
# Console output is written per line, so it survives a killed emulator (vita3k.log may not).
$emuOut = Join-Path (Split-Path $vita3k) 'vita3k_stdout.txt'
$emuErr = Join-Path (Split-Path $vita3k) 'vita3k_stderr.txt'
$process = Start-Process -FilePath $vita3k -ArgumentList $args -WorkingDirectory (Split-Path $vita3k) -PassThru `
    -RedirectStandardOutput $emuOut -RedirectStandardError $emuErr
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
        # A normal close lets Vita3K flush its log; kill only if it does not exit.
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(10000)) { $process.Kill() }
    }
} else {
    $process.WaitForExit()
}

Write-Host "=== $gameLog ==="
if (Test-Path $gameLog) { Get-Content $gameLog -Tail 60 } else { Write-Host '(no game log written)' }
foreach ($log in @($emuOut, $emuErr, $emuLog)) {
    Write-Host "=== $log (errors) ==="
    if (Test-Path $log) { Get-Content $log | Select-String -Pattern 'error|Error|warn|crash|unimpl|nid' | Select-Object -Last 40 }
}
