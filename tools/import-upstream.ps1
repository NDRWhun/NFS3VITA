# Copies the generated NFS3 disassembly from a local nfs-recompiled clone into src/nfs3hp/disassembly.
# Usage: tools\import-upstream.ps1 [-Upstream <path-to-nfs-recompiled>]
param(
    [string]$Upstream = (Join-Path $PSScriptRoot '..\..\ref\nfs-recompiled')
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$lock = Get-Content (Join-Path $repo 'upstream.lock') | Where-Object { $_ -match '^commit\s*=' } | ForEach-Object { ($_ -split '=')[1].Trim() }
$src = Join-Path $Upstream 'src\nfs3hp\disassembly'
$dst = Join-Path $repo 'src\nfs3hp\disassembly'

if (-not (Test-Path $src)) { throw "Upstream disassembly not found: $src" }

$head = (git -C $Upstream rev-parse HEAD).Trim()
if ($head -ne $lock) {
    Write-Warning "Upstream HEAD $head differs from pinned commit $lock"
}

New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item (Join-Path $src '*') $dst -Force
$count = (Get-ChildItem $dst -File).Count
Write-Host "Imported $count files into $dst (upstream $head)"
