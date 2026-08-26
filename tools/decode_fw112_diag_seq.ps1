param(
  [Parameter(Mandatory=$true)][string]$Log
)

$lines = Select-String -Path $Log -Pattern "ID:8001022[ABCDE2]" | ForEach-Object { $_.Line }

$frames = @()
foreach($l in $lines){
  if($l -match '\[INFO\]\s+(\d+)\s+ID:8001022(2|A|B|C|D|E).*?Data:([0-9A-F ]+)'){
    $us = [long]$Matches[1]
    $frag = $Matches[2]
    $data = $Matches[3].Trim().Split(' ') | ForEach-Object { [Convert]::ToInt32($_,16) }
    $frames += [PSCustomObject]@{ Us=$us; Frag=$frag; Data=$data }
  }
}

function BE16($d,$o){ return ($d[$o] -shl 8) -bor $d[$o+1] }
function BE32($d,$o){ return ($d[$o] -shl 24) -bor ($d[$o+1] -shl 16) -bor ($d[$o+2] -shl 8) -bor $d[$o+3] }

# Sort by us to get true chronological order
$frames = $frames | Sort-Object Us

# Walk: header starts a record; expect B,C,D,E within ~50ms after header.
# Match each header to the NEXT frame of each fragment type after it (within 60ms).
$records = @()
$i = 0
while($i -lt $frames.Count){
  $fr = $frames[$i]
  if($fr.Frag -eq "A"){
    $hdrUs = $fr.Us
    $hdr = $fr.Data
    $evid = (BE16 $hdr 2); $evt = $hdr[4]; $rsn = $hdr[5]
    $rec = @{ Us=$hdrUs; EvId=$evid; Evt=$evt; Rsn=$rsn; Schema=$hdr[0]; Header=$hdr; B=$null; C=$null; D=$null; E=$null; dticks=@() }
    # find B,C,D,E after header
    $j = $i+1
    $need = @("B","C","D","E")
    while($j -lt $frames.Count -and $frames[$j].Us - $hdrUs -lt 80000 -and $need.Count -gt 0){
      $cand = $frames[$j]
      if($cand.Frag -in $need){
        $rec[$cand.Frag] = $cand.Data
        $rec.dticks += [pscustomobject]@{F=$cand.Frag; D=($cand.Us - $hdrUs)}
        $need = @($need | Where-Object { $_ -ne $cand.Frag })
      }
      $j++
    }
    $records += $rec
  }
  $i++
}

"RECORDS=$($records.Count) (headers)"
"EvId Evt Rsn  El(dump) dTicks(B,C,D,E)  Flags(0x1022C[6])  Stab  EvtVal"
foreach($r in $records){
  $el = 0
  if($r.E){ $el = BE32 $r.E 0 }
  $dt = if($r.dticks){ ($r.dticks | ForEach-Object { $_.F+":"+$_.D }) -join "," } else { "-" }
  $fl = if($r.C){ "{0:X2}" -f $r.C[6] } else { "-" }
  # FW-112-STABILITY (schema 3): RECOVERY_COLLAPSE (6) / RECOVERY_EXIT (5) - header[6..7] = the
  # stability streak at the transition, frag4[4..5] = filtered assist (collapse) / collapse count
  # (exit). Schema 1-2 logs leave both zero.
  $stab=""; $evtval=""
  if($r.Schema -ge 3 -and ($r.Evt -eq 5 -or $r.Evt -eq 6)){
    $stab = (($r.Header[6] -shl 8) -bor $r.Header[7])
    if($r.E){ $evtval = (($r.E[4] -shl 8) -bor $r.E[5]) }
  }
  "{0:X4} {1}  {2:X2}  {3}  [{4}]  flags={5}  {6}  {7}" -f $r.EvId,$r.Evt,$r.Rsn,$el,$dt,$fl,$stab,$evtval
}
$total = 0L
foreach($r in $records){ if($r.E){ $total += (BE32 $r.E 0) } }
"SUM_ELAPSED_TICKS=$total  MS=$([math]::Round($total*0.25,1))"