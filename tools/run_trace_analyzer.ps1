param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$AnalyzerArguments
)

$ErrorActionPreference = 'Stop'

$python = $null
foreach ($commandName in @('python3', 'python')) {
    $command = Get-Command $commandName -ErrorAction SilentlyContinue
    if ($command -and $command.Source -and [System.IO.File]::Exists($command.Source)) {
        $python = $command.Source
        break
    }
}
$localPythonRoot = Join-Path $env:LOCALAPPDATA 'Programs\Python'
if (-not $python -and (Test-Path -LiteralPath $localPythonRoot)) {
    $python = Get-ChildItem -Path (Join-Path $localPythonRoot 'Python*\python.exe') -File -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -ExpandProperty FullName -First 1
}
if (-not $python) {
    throw 'Python 3.10+ was not found. Install Python or put python/python3 on PATH.'
}

$versionText = & $python -c 'import sys;print(sys.version_info.major,sys.version_info.minor,sep=chr(46))'
if ($LASTEXITCODE -ne 0 -or [version]$versionText -lt [version]'3.10') {
    throw "Python 3.10+ is required; found $versionText at $python"
}

$entryPoint = Join-Path $PSScriptRoot 'trace_analyzer.py'
& $python $entryPoint @AnalyzerArguments
exit $LASTEXITCODE
