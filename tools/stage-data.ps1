# Builds the game data folder for the Vita: copy the result to ux0:data/nfs3vita/ (VitaShell or FTP).
# Usage: tools/stage-data.ps1 -Game <installed game or CD folder> [-ModernPatch <extracted Modern Patch 1.6.1>] -Out <dir>
# -Game must hold the fedata and gamedata folders. -ModernPatch adds the patch's data files, which builds generated
# from the patch's nfs3.exe need.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$ModernPatch = '',
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'
foreach ($folder in 'fedata', 'gamedata') {
    if (-not (Test-Path (Join-Path $Game $folder))) { throw "$Game has no $folder folder." }
}
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($folder in 'fedata', 'gamedata') {
    $target = Join-Path $Out $folder
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item (Join-Path (Join-Path $Game $folder) '*') $target -Recurse -Force
}
Write-Host "Copied the game data from $Game"

if ($ModernPatch) {
    # The patch files the port reads (copying the patch's whole drivers, fedata and gamedata folders works as well).
    # drivers/nglide is the renderer the patch's nfs3.ini selects; the port only checks that its voodoo2a.dll exists
    # and runs its own Voodoo 2 driver.
    $files = @(
        'nfs3.ini',
        'drivers/nglide/thrash.ini',
        'drivers/nglide/voodoo2a.dll',
        'fedata/menus/*.mnu',
        'fedata/text/text.*',
        'fedata/art/logos.qfs',
        'gamedata/dashhud/hud98.fsh'
    )
    $copied = 0
    foreach ($pattern in $files) {
        $found = @(Get-ChildItem -Path (Join-Path $ModernPatch $pattern) -File -ErrorAction SilentlyContinue)
        if ($found.Count -eq 0) { throw "missing in the Modern Patch folder: $pattern" }
        $folder = Join-Path $Out (Split-Path $pattern)
        New-Item -ItemType Directory -Force $folder | Out-Null
        foreach ($file in $found) {
            Copy-Item $file.FullName (Join-Path $folder $file.Name) -Force
            $copied++
        }
    }
    $driver = $null
    foreach ($line in Get-Content (Join-Path $Out 'nfs3.ini')) {
        if ($line -match '^\s*ThrashDriver\s*=\s*(\S+)') { $driver = $Matches[1]; break }
    }
    if ($driver -and $driver -ne 'nglide') {
        Write-Warning "nfs3.ini selects ThrashDriver=$driver; the port needs a voodoo-type driver folder such as nglide."
    }
    Write-Host "Copied $copied Modern Patch files from $ModernPatch"
}
Write-Host "Done: copy the contents of $Out to ux0:data/nfs3vita/"
