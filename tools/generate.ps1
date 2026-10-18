# Generates the C++ recompilation of the game into src/nfs3hp/disassembly with tools/recompiler.
# Usage: tools/generate.ps1 -Binaries <dir with nfs3.exe (original or Modern Patch 1.6.1), eacsnd.dll, softtria.dll, voodoo2a.dll>
#        [-Out <dir>] [-Analysis <file.json>]
# Needs Python 3; the recompiler's packages (capstone) are installed with pip when missing. Only files whose content
# changed are replaced, so the build recompiles just those; generated files of earlier runs are deleted.
param(
    [Parameter(Mandatory = $true)][string]$Binaries,
    [string]$Out,
    [string]$Analysis
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Out) { $Out = Join-Path $root '../src/nfs3hp/disassembly' }

# The first python on PATH that runs Python 3; the Microsoft Store's python alias only prints an install hint.
function Test-Python3([string]$exe) {
    $ErrorActionPreference = 'Continue'
    try { return (& $exe -c 'import sys; print(sys.version_info[0])' 2>$null) -eq '3' } catch { return $false }
}
$python = $null
foreach ($name in 'python', 'python3') {
    $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and (Test-Python3 $command.Source)) { $python = $command; break }
}
if (-not $python) {
    throw ('Python 3 not found on PATH (https://www.python.org/downloads/). On Windows, the "python" app execution ' +
           'alias of the Microsoft Store is not Python: install Python, or turn the alias off in Settings.')
}

# Runs Python and returns its exit code; failure is judged by the exit code, since Windows PowerShell 5.1 would stop
# on any stderr line.
function Invoke-Python {
    $ErrorActionPreference = 'Continue'
    & $python.Source @args | Write-Host
    return $LASTEXITCODE
}

# Installs the recompiler's Python packages (capstone) when they are missing.
if ((Invoke-Python -c "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('capstone') else 1)") -ne 0) {
    Write-Host 'Installing the recompiler requirements (capstone)...'
    if ((Invoke-Python -m pip install -r (Join-Path $root 'recompiler/requirements.txt')) -ne 0) { throw 'pip install failed' }
}

New-Item -ItemType Directory -Force $Out | Out-Null
$arguments = @((Join-Path $root 'recompiler/generate.py'), '--binaries', $Binaries, '--out', (Resolve-Path $Out).Path)
if ($Analysis) { $arguments += @('--analysis', $Analysis) }
if ((Invoke-Python @arguments) -ne 0) { throw 'generation failed' }
