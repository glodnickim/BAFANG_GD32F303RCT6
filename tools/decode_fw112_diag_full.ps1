param(
  [Parameter(Mandatory=$true)][string]$Log
)

$lines = Select-String -Path $Log -Pattern "ID:8001022A|ID:8001022B|ID:8001022C|ID:8001022D|ID:8001022E" | ForEach-Object { $_.Line }

$byFrag = @{}
foreach($l in $lines){
  if($l -match 'ID:8001022(2|A|B|C|D|E).*?Data:([0-9A-F ]+)'){
    $frag = $Matches[1]
    $data = $Matches[2].Trim().Split(' ') | ForEach-Object { [Convert]::ToInt32($_,16) }
    if(-not $byFrag.ContainsKey($frag)){ $byFrag[$frag] = @() }
    $byFrag[$frag] += ,$data
  }
}

function BE16($d,$o){ return ($d[$o] -shl 8) -bor $d[$o+1] }
function BE32($d,$o){ return ($d[$o] -shl 24) -bor ($d[$o+1] -shl 16) -bor ($d[$o+2] -shl 8) -bor $d[$o+3] }

$h = $byFrag["A"]; $b = $byFrag["B"]; $c = $byFrag["C"]; $d = $byFrag["D"]; $e = $byFrag["E"]
$n = $h.Count

$evtName = @{1='BLOCKED';2='GRANTED';3='REVOKED';4='REC_ENTER';5='REC_EXIT';6='REC_COLLAPSE';7='ZEROED';8='HOLD_ARMED';9='HOLD_EXPIRED'}
$cum = 0L
$evCum = @{}
for($i=0;$i -lt $n;$i++){
  $hh = $h[$i]
  $evid = BE16 $hh 2
  $el = 0
  if($e -and $i -lt $e.Count){ $el = BE32 $e[$i] 0 }
  $cum += $el
  $evCum[$evid] = $cum
}

"EvId Cum Evt  Rsn Flags Ses Dir Rec FwdRun CrSt ReqSt StartSt Cad Hold Load Thr IqReq IqPre IqSet IqAct El  Stab  EvtVal"
for($i=0;$i -lt $n;$i++){
  $hh=$h[$i]; $evid=BE16 $hh 2; $evt=$hh[4]; $rsn=$hh[5]
  $el = 0; if($e -and $i -lt $e.Count){ $el = BE32 $e[$i] 0 }
  # FW-112-STABILITY (schema 3): RECOVERY_COLLAPSE (6) / RECOVERY_EXIT (5) - header[6..7] = the
  # stability streak at the transition, frag4[4..5] = filtered assist (collapse) / collapse count
  # (exit). Schema 1-2 logs leave both zero.
  $stab=""; $evtval=""
  if($hh[0] -ge 3 -and ($evt -eq 5 -or $evt -eq 6)){
    $stab = (($hh[6] -shl 8) -bor $hh[7])
    if($e -and $i -lt $e.Count){ $evtval = (($e[$i][4] -shl 8) -bor $e[$i][5]) }
  }
  $l=""
  if($b -and $i -lt $b.Count){ $bb=$b[$i]; $l += ("{0} {1} {2} {3} {4} {5} {6} {7}" -f $bb[0],$bb[1],$bb[2],$bb[3],$bb[4],$bb[5],$bb[6],$bb[7]) }
  if($c -and $i -lt $c.Count){ $cc=$c[$i]; $l += ("  {0} {1} {2} {3} {4}" -f (BE16 $cc 0),(BE16 $cc 2),(BE16 $cc 4),$cc[6],$cc[7]) }
  function S16($d,$o){ $v = BE16 $d $o; if($v -ge 32768){ $v -= 65536 }; return $v }
if($d -and $i -lt $d.Count){ $dd=$d[$i]; $l += ("  {0} {1} {2} {3}" -f (S16 $dd 0),(S16 $dd 2),(S16 $dd 4),(S16 $dd 6)) }
  $name = if($evtName.ContainsKey($evt)){ $evtName[$evt] } else { $evt }
  "{0:X4} {1,6} {2,-11} {3:X2} {4}  {5}  {6}" -f $evid,$evCum[$evid],$name,$rsn,$l,$stab,$evtval
}