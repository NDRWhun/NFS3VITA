# Generates the C++ recompilation of the game into src/nfs3hp/disassembly with tools/recompiler.
# Usage: tools\generate.ps1 -Binaries <dir with the original nfs3.exe, eacsnd.dll, softtria.dll, voodoo2a.dll>
#        [-Out <dir>] [-Analysis <file.json>]
# Needs Python 3 with capstone (pip install -r tools/recompiler/requirements.txt).
param(
    [Parameter(Mandatory = $true)][string]$Binaries,
    [string]$Out,
    [string]$Analysis
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Out) { $Out = Join-Path $root '..\src\nfs3hp\disassembly' }
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command python3 -ErrorAction Stop }

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# Generate into a staging folder, then replace only files whose content changed, so unchanged files
# keep their timestamps and the build recompiles just the changed ones.
$stage = Join-Path ([IO.Path]::GetTempPath()) ("nfs3gen-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $stage | Out-Null
try {
    $arguments = @((Join-Path $root 'recompiler\generate.py'), '--binaries', $Binaries, '--out', $stage)
    if ($Analysis) { $arguments += @('--analysis', $Analysis) }
    & $python.Source @arguments
    if ($LASTEXITCODE -ne 0) { throw 'generation failed' }

    $generated = Get-ChildItem $stage -File
    $changed = 0
    foreach ($file in $generated) {
        $target = Join-Path $Out $file.Name
        if (-not (Test-Path $target) -or (Get-FileHash $target).Hash -ne (Get-FileHash $file.FullName).Hash) {
            Copy-Item $file.FullName $target -Force
            $changed++
        }
    }
    $names = @{}
    foreach ($file in $generated) { $names[$file.Name] = $true }
    $removed = 0
    foreach ($file in Get-ChildItem $Out -File -Include *.cpp, *.h) {
        if (-not $names.ContainsKey($file.Name)) { Remove-Item $file.FullName -Force; $removed++ }
    }
    Write-Host "Generated $($generated.Count) files into $Out ($changed changed, $removed removed)"
}
finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
