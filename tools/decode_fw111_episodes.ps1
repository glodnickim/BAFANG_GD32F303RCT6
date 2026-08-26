param(
  [Parameter(Mandatory=$true)][string]$Log
)

$lines = Select-String -Path $Log -Pattern "ID:80010210|ID:80010211|ID:80010212|ID:80010213|ID:80010214|ID:8001021A" | ForEach-Object { $_.Line }

$byFrag = @{}
foreach($l in $lines){
  if($l -match 'ID:800102(10|11|12|13|14|1A).*?Data:([0-9A-F ]+)'){
    $frag = [Convert]::ToInt32($Matches[1],16)
    $data = $Matches[2].Trim().Split(' ') | ForEach-Object { [Convert]::ToInt32($_,16) }
    if(-not $byFrag.ContainsKey($frag)){ $byFrag[$frag] = @() }
    $byFrag[$frag] += ,$data
  }
}

function BE16($d,$o){ return ($d[$o] -shl 8) -bor $d[$o+1] }

$hdrs = $byFrag[0x1A]
$n = $hdrs.Count
"EPISODES=$n  (frag counts: 10=$($byFrag[0x10].Count) 11=$($byFrag[0x11].Count) 12=$($byFrag[0x12].Count) 13=$($byFrag[0x13].Count) 14=$($byFrag[0x14].Count))"
"Num RevS Flg LatchMs RecMs TgtMs PreRamp FastR LstRev FstFwd StepsReady LoadReady ReqSt Thr Peak ArmLoad ArmIq"
for($i=0;$i -lt $n;$i++){
  $num = BE16 $hdrs[$i] 2
  $out = "{0:X2}  " -f $num
  $h = $byFrag[0x10][$i]
  if($h){ $out += ("{0}  {1:X2} {2} {3}  " -f $h[2],$h[3],(BE16 $h 4),(BE16 $h 6)) } else { $out += "-  - - -  " }
  $t = $byFrag[0x12][$i]
  if($t){ $out += ("{0} {1}   {2}   " -f (BE16 $t 0),(BE16 $t 2),$t[5]) } else { $out += "- -   -   " }
  $w = $byFrag[0x13][$i]
  if($w){ $out += ("{0} {1} {2} {3}  " -f (BE16 $w 0),(BE16 $w 2),(BE16 $w 4),(BE16 $w 6)) } else { $out += "- - - -  " }
  $g = $byFrag[0x14][$i]
  if($g){ $out += ("{0} {1} {2}  " -f $g[0],(BE16 $g 2),(BE16 $g 4)) } else { $out += "- - -  " }
  $a = $byFrag[0x11][$i]
  if($a){ $out += ("{0} {1}" -f (BE16 $a 0),(BE16 $a 6)) } else { $out += "- -" }
  $out
}
