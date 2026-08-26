param(
    [Parameter(Mandatory=$true)][string]$Log,
    [switch]$OutputJson
)
$ErrorActionPreference = "Stop"
trap { Write-Output "SCRIPT ERR line $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message) :: $($_.InvocationInfo.Line.Trim())"; break }

$evtName = @{1="BLOCKED";2="PERMISSION_GRANTED";3="PERMISSION_REVOKED";4="RECOVERY_ENTER";5="RECOVERY_EXIT";6="RECOVERY_COLLAPSE";7="ZEROED";8="HOLD_ARMED";9="HOLD_EXPIRED"}
$reasonName = @{0x01="LEVEL_ZERO";0x02="DIRECTION";0x04="START_STEPS";0x08="LOAD_BELOW";0x10="REARM_GRANT";0x20="RECOVERY_WAIT";0x40="MODE_ZERO";0x80="SAFETY"}
$flagName = @{0x01="LATCHED";0x02="FAST_REARM";0x04="COLD_ARM";0x08="PWM_ON";0x10="SENSOR_VALID";0x20="CAL_USER";0x40="WHEEL_VALID";0x80="ROLLING_COAST"}
$sessionName = @{0="COLD";1="ACTIVE";2="SUSP_DIR";3="WAIT_REARM"}
$dirName = @{0="FWD_SAFE";1="DIR_INHIBIT";2="FWD_CONFIRM"}
$recovName = @{0="IDLE";1="WAIT_FRESH";2="TRACK_FAST"}
$flags2Name = @{0x01="ELAPSED_SAT";0x02="HOLD_SAT";0x04="FORCE_ZERO";0x08="HARD_CUT_SET";0x10="RECOVERY_WAIT";0x20="FINAL_ZERO";0x40="PU_CLAMPED";0x80="SPARE"}

function ToI16([int]$v){ if($v -ge 0x8000){ $v - 0x10000 } else { $v } }

$lines = Get-Content -LiteralPath $Log
$all = New-Object System.Collections.ArrayList
foreach($ln in $lines){
    if($ln -match '\[(\d{2}:\d{2}:\d{2})\]\s+\[INFO\]\s+(\d+)\s+ID:([0-9A-F]+)\s+DLC:\d+\s+Data:(.*)'){
        $hTs=$Matches[1]; $hUs=[uint64]$Matches[2]; $id=$Matches[3]; $ds=$Matches[4]
        if($null -eq $ds){ $ds="" }
        $rep=1; if($ln -match '\(Repeated (\d+) times\)'){ $rep=[int]$Matches[1]+1 }
        if($id -match '^8001022[ABCDE]$'){
            $b=@()
            if($ds.Trim().Length -gt 0){ $b=@($ds.Trim() -split '\s+' | Where-Object {$_ -ne ""} | ForEach-Object { try{[Convert]::ToInt32($_,16)}catch{0} }) }
            for($k=0;$k -lt $rep;$k++){ [void]$all.Add([pscustomobject]@{ Id=$id; Us=($hUs+$k); B=$b; Used=$false }) }
        }
    }
}
# sort by us then keep order
$sorted = @($all | Sort-Object Us)
$n = $sorted.Count

# gather headers in order
$headers = @()
for($i=0;$i -lt $n;$i++){ if($sorted[$i].Id -eq "8001022A"){ $headers += $sorted[$i] } }

$recs=@()
foreach($h in $headers){
    $t0 = $h.Us
    $schema = [int]$h.B[0]
    # Schema 4 compacted the old 4 snapshot frames into 3 and changed their
    # layout. Schemas 1-3 keep the historical decoder path byte-for-byte.
    $fragmentIds = if($schema -ge 4){
        @("8001022B","8001022C","8001022D")
    } else {
        @("8001022B","8001022C","8001022D","8001022E")
    }
    $frag = @{}; $delta = @{}
    foreach($fid in $fragmentIds){
        $best = $null; $bestD = [uint64]::MaxValue
        for($i=0;$i -lt $n;$i++){
            $f = $sorted[$i]
            if($f.Used -or $f.Id -ne $fid){ continue }
            $d = if($f.Us -ge $t0){ $f.Us - $t0 } else { [uint64]($t0 - $f.Us) }
            if($d -lt $bestD){ $bestD = $d; $best = $f }
        }
        if($null -ne $best){ $best.Used=$true; $frag[$fid]=$best; $delta[$fid]=$bestD }
    }
    $ok = ($frag.Count -eq $fragmentIds.Count)
    $evid = (($h.B[2] -shl 8) -bor $h.B[3]); $etype=$h.B[4]; $reason=$h.B[5]
    $r=[ordered]@{ Schema=$schema; EvId=("0x{0:X4}" -f $evid); Evt=$evtName[[int]$etype]; Reason=$reason.ToString("X2"); OK=if($ok){"Y"}else{"N"} }
    $rb=@(); for($bi=0;$bi -lt 8;$bi++){ if(($reason -band (1 -shl $bi)) -ne 0){ $rb += $reasonName[(1 -shl $bi)] } }
    $r["Rbits"]=(($rb -join "+"))
    if($ok -and $schema -ge 4){
        $f1=$frag["8001022B"].B; $f2=$frag["8001022C"].B; $f3=$frag["8001022D"].B
        $packed=[int]$f1[0]; $perm=[int]$f1[1]; $f2bits=[int]$f1[5]
        $f2names=@(); for($bi=0;$bi -lt 8;$bi++){ if(($f2bits -band (1 -shl $bi)) -ne 0){ $f2names += $flags2Name[(1 -shl $bi)] } }
        $r["Flags"]=""
        $r["SesSt"]=$sessionName[($packed -band 0x03)]
        $r["Dir"]=$dirName[(($packed -shr 2) -band 0x03)]
        $r["Recov"]=$recovName[(($packed -shr 4) -band 0x03)]
        $r["FwdRun"]=$f1[3]; $r["CrSteps"]=""; $r["ReqSt"]=""; $r["StartSt"]=""; $r["Cad"]=$f1[2]
        $r["Hold"]=$f1[4]; $r["Load"]=(($f2[0] -shl 8) -bor $f2[1]); $r["Thr"]=(($f2[2] -shl 8) -bor $f2[3])
        $r["PermBits"]=$perm; $r["Latched"]=if(($perm -band 0x01) -ne 0){1}else{0}
        $r["Permission"]=if(($perm -band 0x80) -ne 0){1}else{0}
        $r["Flags2"]=$f2bits; $r["Flags2Names"]=(($f2names -join "+"))
        $r["MVU"]=(($f2[4] -shl 8) -bor $f2[5]); $r["IqBeforePu"]=ToI16 (($f2[6] -shl 8) -bor $f2[7])
        $r["IqReq"]=ToI16 (($f3[0] -shl 8) -bor $f3[1]); $r["IqPre"]=ToI16 (($f3[2] -shl 8) -bor $f3[3]); $r["IqSet"]=ToI16 (($f3[4] -shl 8) -bor $f3[5]); $r["IqAct"]=ToI16 (($f3[6] -shl 8) -bor $f3[7])
        $r["El"]=(($f1[6] -shl 8) -bor $f1[7]); $r["Stab"]=if($etype -eq 5 -or $etype -eq 6){(($h.B[6] -shl 8) -bor $h.B[7])}else{""}; $r["EvtVal"]=""
        $r["dTicks"]=([uint64]([math]::Max([double]0,[double]$delta["8001022D"])))
    } elseif($ok) {
        $f1=$frag["8001022B"].B; $f2=$frag["8001022C"].B; $f3=$frag["8001022D"].B; $f4=$frag["8001022E"].B
        $stab=$null; $evtval=$null
        if($schema -ge 3 -and ($etype -eq 6 -or $etype -eq 5)){ $stab=(($h.B[6] -shl 8) -bor $h.B[7]); $evtval=(($f4[4] -shl 8) -bor $f4[5]) }
        $fl=$f2[6]; $fb=@(); for($bi=0;$bi -lt 8;$bi++){ if(($fl -band (1 -shl $bi)) -ne 0){ $fb += $flagName[(1 -shl $bi)] } }
        $r["Flags"]=(($fb -join "+")); $r["SesSt"]=$sessionName[[int]$f1[0]]; $r["Dir"]=$dirName[[int]$f1[1]]; $r["Recov"]=$recovName[[int]$f1[2]]
        $r["FwdRun"]=$f1[3]; $r["CrSteps"]=$f1[4]; $r["ReqSt"]=$f1[5]; $r["StartSt"]=$f1[6]; $r["Cad"]=$f1[7]
        $r["Hold"]=(($f2[0] -shl 8) -bor $f2[1]); $r["Load"]=(($f2[2] -shl 8) -bor $f2[3]); $r["Thr"]=(($f2[4] -shl 8) -bor $f2[5])
        $r["IqReq"]=ToI16 (($f3[0] -shl 8) -bor $f3[1]); $r["IqPre"]=ToI16 (($f3[2] -shl 8) -bor $f3[3]); $r["IqSet"]=ToI16 (($f3[4] -shl 8) -bor $f3[5]); $r["IqAct"]=ToI16 (($f3[6] -shl 8) -bor $f3[7])
        $r["El"]=(($f4[0] -shl 24) -bor ($f4[1] -shl 16) -bor ($f4[2] -shl 8) -bor $f4[3]); $r["Stab"]=if($null -ne $stab){$stab}else{""}; $r["EvtVal"]=if($null -ne $evtval){$evtval}else{""}
        $r["PermBits"]=""; $r["Latched"]=""; $r["Permission"]=""; $r["Flags2"]=""; $r["Flags2Names"]=""; $r["MVU"]=""; $r["IqBeforePu"]=""
        $r["dTicks"]=([uint64]([math]::Max([double]0,[double]$delta["8001022E"])))
    } else {
        foreach($name in @("Flags","SesSt","Dir","Recov","FwdRun","CrSteps","ReqSt","StartSt","Cad","Hold","Load","Thr","PermBits","Latched","Permission","Flags2","Flags2Names","MVU","IqBeforePu","IqReq","IqPre","IqSet","IqAct","El","Stab","EvtVal","dTicks")){ $r[$name]="" }
    }
    $recs += [pscustomobject]$r
}
if($OutputJson){
    # Stable machine-readable mode used by tools/trace_analyzer.py. Keep this
    # decoder as the single owner of FW-112 frame pairing and wire layout.
    ConvertTo-Json -InputObject @($recs) -Depth 5 -Compress
} else {
    Write-Output ("LOG: " + $Log + "  headers=" + $headers.Count)
    $recs | ConvertTo-Csv -NoTypeInformation
}
