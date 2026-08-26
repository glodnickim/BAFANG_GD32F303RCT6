param(
  [Parameter(Mandatory=$true)][string]$Log
)

# Time-based fragment pairing for fw112_diag (0x1022A header + B/C/D/E).
# Each record = header + 4 data frames, sent back-to-back. We pair by nearest
# PRECEDING header within 60 ms, immune to (Repeated N times) dedup reordering.

$events = @()
Select-String -Path $Log -Pattern "ID:8001022" | ForEach-Object {
  $l = $_.Line
  if($l -match '\[INFO\]\s+(\d+)\s+ID:8001022([A-E]).*?Data:([0-9A-F ]+)'){
    $us = [long]$Matches[1]
    $frag = $Matches[2]
    $d = $Matches[3].Trim().Split(' ') | ForEach-Object { [Convert]::ToInt32($_,16) }
    $events += ,@($us, $frag, $d)
  }
}

function BE16($d,$o){ return ($d[$o] -shl 8) -bor $d[$o+1] }

$records = @{}
foreach($ev in $events){
  $us = $ev[0]; $frag = $ev[1]; $d = $ev[2]
  if($frag -eq "A"){
    # header: schema, session_id, event_id BE16(2,3), event_type, reason_bits
    $key = $us
    $records[$key] = @{ header = $d; parts = @{} }
  }
}
# second pass: assign B/C/D/E to the closest preceding header within 60ms
$keys = @($records.Keys | Sort-Object)
foreach($ev in $events){
  $us = $ev[0]; $frag = $ev[1]; $d = $ev[2]
  if($frag -eq "A"){ continue }
  $best = $null
  foreach($k in $keys){
    if($k -gt $us){ break }
    if(($us - $k) -le 60000){ $best = $k } else { }
  }
  if($best -ne $null){
    $records[$best].parts[$frag] = $d
  }
}

$evtName = @{1='BLOCKED';2='GRANTED';3='REVOKED';4='REC_ENTER';5='REC_EXIT';6='REC_COLLAPSE';7='ZEROED';8='HOLD_ARMED';9='HOLD_EXPIRED'}
"EvId Evt  Rsn Ses Dir Rec FwdRun CrSt ReqSt StartSt Cad Hold Load Thr Flags IqReq IqPre IqSet IqAct El"
$cum = 0L
foreach($k in $keys){
  $rec = $records[$k]
  $h = $rec.header
  $evid = (BE16 $h 2)
  $evt = $h[4]; $rsn = $h[5]
  $B = $rec.parts["B"]; $C = $rec.parts["C"]; $D = $rec.parts["D"]; $E = $rec.parts["E"]
  $el = 0; if($E){ $el = ($E[0] -shl 24) -bor ($E[1] -shl 16) -bor ($E[2] -shl 8) -bor $E[3] }
  $cum += $el
  $s = ""
  if($B){ $s += " {0} {1} {2} {3} {4} {5} {6} {7}" -f $B[0],$B[1],$B[2],$B[3],$B[4],$B[5],$B[6],$B[7] } else { $s += " - - - - - - - -" }
  if($C){ $s += " {0} {1} {2} 0x{3:X2}" -f (BE16 $C 0),(BE16 $C 2),(BE16 $C 4),$C[6] } else { $s += " - - - -" }
  if($D){ $s += " {0} {1} {2} {3}" -f ([int16](BE16 $D 0)),([int16](BE16 $D 2)),([int16](BE16 $D 4)),([int16](BE16 $D 6)) } else { $s += " - - - -" }
  $name = if($evtName.ContainsKey($evt)){ $evtName[$evt] } else { $evt }
  "{0:X4} {1,-10} {2:X2}{3}  el={4}" -f $evid,$name,$rsn,$s,$el
  "{0:X4} cum={1}" -f $evid,$cum
}