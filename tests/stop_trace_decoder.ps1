param([Parameter(Mandatory=$true)][string]$Fixture,
      [string]$WorkDirectory = '')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$decoder=Join-Path $root 'tools/decode_stop_trace.ps1'
if(-not $WorkDirectory){$WorkDirectory=Join-Path $root '.work/stop-trace/decoder-tests'}
[void](New-Item -ItemType Directory -Path $WorkDirectory -Force)
$good=@(Get-Content -LiteralPath $Fixture)
if($good.Count -lt 30){throw 'Fixture too short'}
$badLine=$good[20]
$last=[Convert]::ToByte($badLine.Substring($badLine.Length-2),16) -bxor 1
$badLine=$badLine.Substring(0,$badLine.Length-2)+('{0:X2}' -f $last)
$cases=@(
    @{name='complete';ok=$true;lines=$good},
    @{name='duplicate';ok=$true;lines=@($good[0..20])+$good[20]+@($good[21..($good.Count-1)])},
    @{name='header_duplicate';ok=$true;lines=@($good[0])+$good},
    @{name='sniffer_summary';ok=$true;lines=@($good[0..20])+($good[0]+' (Repeated 2 times)')+@($good[21..($good.Count-1)])},
    @{name='missing';ok=$false;lines=@($good[0..19])+@($good[21..($good.Count-1)])},
    @{name='corrupt';ok=$false;lines=@($good[0..19])+$badLine+@($good[21..($good.Count-1)])},
    @{name='conflict';ok=$false;lines=@($good[0..20])+$badLine+@($good[21..($good.Count-1)])},
    @{name='no_trailer';ok=$false;lines=@($good[0..($good.Count-9)])},
    @{name='new_incomplete_replay';ok=$false;lines=$good+@($good[0..20])},
    @{name='retry_complete';ok=$true;lines=@($good[0..20])+$good}
)
$failed=0
foreach($case in $cases){
    $path=Join-Path $WorkDirectory ($case.name+'.log')
    $case.lines | Set-Content -LiteralPath $path -Encoding ASCII
    $out=Join-Path $WorkDirectory ($case.name+'-'+[guid]::NewGuid().ToString('N'))
    $previous=$ErrorActionPreference; $ErrorActionPreference='Continue'
    $result=& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $decoder -Log $path -OutputDirectory $out 2>&1
    $code=$LASTEXITCODE; $ErrorActionPreference=$previous
    if(($code -eq 0) -ne $case.ok){$failed++;Write-Output "FAIL $($case.name): $result"}
    elseif(-not $case.ok -and (Test-Path -LiteralPath (Join-Path $out 'summary.json'))){$failed++;Write-Output "FAIL invalid capture exported: $($case.name)"}
    else {Write-Output "PASS $($case.name)"}
}
if($failed){throw "$failed decoder tests failed"}
Write-Output 'STOP-TRACE decoder: ALL CHECKS PASSED'
