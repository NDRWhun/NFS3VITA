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
Get-ChildItem $Out -File -Include *.cpp, *.h -Recurse | Remove-Item -Force

$arguments = @((Join-Path $root 'recompiler\generate.py'), '--binaries', $Binaries, '--out', $Out)
if ($Analysis) { $arguments += @('--analysis', $Analysis) }
& $python.Source @arguments
if ($LASTEXITCODE -ne 0) { throw 'generation failed' }
$count = (Get-ChildItem $Out -File).Count
Write-Host "Generated $count files into $Out"
