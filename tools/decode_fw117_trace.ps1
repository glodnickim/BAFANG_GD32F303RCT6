param(
    [Parameter(Mandatory=$true)][string]$Log,
    [switch]$Full,
    [switch]$OutputJson
)
# FW-117 (TEMPORARY): bridge lifecycle trace decoder (START/STOP click card).
#
# Wire format is the ACTUAL producer serializer (src/main.c diag_fw117_frame) over the sample
# layout in inc/fw117_trace.h:
#
#   header frame 0x00010234  (8 B):
#       [0]=schema (FW117_TRACE_SCHEMA_VERSION), [1]=session_id, [2]=0,
#       [3]=capture_id, [4]=event_type (0=START bridge-on, 1=STOP MOE-off), [5:7]=0
#   data frames 0x00010235..0x00010238 (4 x 8 B) per sample:
#       frag1 0x10235:  [0:3]=tick_abs BE(U32)  [4]=flags  [5]=state_and_hall  [6:7]=pi_q_int BE(S16)
#       frag2 0x10236:  [0:1]=pi_d_int BE(S16)  [2:3]=iq_request BE(S16)  [4:5]=iq_pre_ramp BE(S16)
#                       [6:7]=iq_setpoint BE(S16)
#       frag3 0x10237:  [0:1]=iq_meas BE(S16)  [2:3]=id_meas BE(S16)  [4:5]=vq BE(S16)
#                       [6:7]=vd BE(S16)
#       frag4 0x10238:  [0:1]=angle BE(U16)  [2:3]=ccr1 BE(U16)  [4:5]=ccr2 BE(U16)
#                       [6:7]=ccr3 BE(U16)
#
# flags: bit0 MOE, bit1 CEN, bit2 PWM_ON, bit3 CUTOFF_ACT, bit4 START_CAP, bit5 REVERSE
# state (bits0-2 of state_and_hall): 0=IDLE 1=STARTING 2=RUNNING 3=COASTING 4=SOFT_OFF 5=FAULT
$ErrorActionPreference = "Stop"
trap { Write-Output "SCRIPT ERR line $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message) :: $($_.InvocationInfo.Line.Trim())"; break }

$TICKS_PER_MS = 4.0   # CONTROL_TIMEBASE_HZ

$stateName = @{0="IDLE";1="STARTING";2="RUNNING";3="COASTING";4="SOFT_OFF";5="FAULT"}
$evtName = @{0="START (bridge-on)";1="STOP (MOE-off)"}
$flagName = @{0x01="MOE";0x02="CEN";0x04="PWM_ON";0x08="CUTOFF";0x10="START_CAP";0x20="REVERSE"}

function ToU16($d,$o){ return (([int]$d[$o] -shl 8) -bor [int]$d[$o+1]) -band 0xFFFF }
function ToS16($d,$o){ $v = ToU16 $d $o; if($v -ge 0x8000){ $v = $v - 0x10000 }; return $v }
function ToU32($d,$o){ return (([int]$d[$o] -shl 24) -bor ([int]$d[$o+1] -shl 16) -bor ([int]$d[$o+2] -shl 8) -bor [int]$d[$o+3]) }
function Bits($v,$names){ return (($names.GetEnumerator() | Where-Object { ($v -band $_.Key) -ne 0 } | ForEach-Object { $_.Value }) -join "+") }
function TicksMs($v){ return ("{0:N1} ms" -f ($v / $TICKS_PER_MS)) }

# ---- 1. collect frames from the log, per FW-117 ID, honouring the sniffer's two DIFFERENT
#         "(Repeated ...)" markers (see sniffer.js) - RAW logs (captured after the sniffer fix
#         that made these five IDs bypass its accumulator, CB-side) never contain either marker
#         and every line is exactly one frame; LEGACY logs (captured before that fix) still do.
#
#         "(Repeated N times - same data)" is a PROGRESS marker only, logged every 100th
#         identical repeat while the accumulator is still counting - it is not itself a new
#         occurrence and must never be parsed as a frame (its trailing text is not hex, and
#         swallowing it as one would corrupt that sample's byte array with garbage).
#
#         "(Repeated N times)" (no "- same data" suffix) is the FINAL summary the accumulator
#         writes once, either when the value changes or at shutdown: N is the TOTAL number of
#         occurrences of that payload, INCLUDING the plain, unmarked line logged when it first
#         appeared (that line was already added to this ID's list, as 1 frame, by the normal
#         no-marker case below). The summary line itself represents the reconstruction of the
#         REMAINING occurrences, so it contributes N-1 more - not N (would double count the
#         first line) and not N+1 (the old bug: two too many per repeated run).
$lines = Get-Content -LiteralPath $Log
$fwEfids = @('0x10234','0x10235','0x10236','0x10237','0x10238')
$idLists = @{}
foreach($ef in $fwEfids){ $idLists[$ef] = New-Object System.Collections.ArrayList }

$lineRe = '^\[(\d{2}:\d{2}:\d{2})\]\s+\[INFO\]\s+(\d+)\s+ID:([0-9A-F]+)\s+DLC:\d+\s+Data:((?:[0-9A-Fa-f]{2}\s*)*)(?:\(Repeated\s+(\d+)\s+times\))?\s*$'
$sameDataRe = 'ID:8001023[4-8]\b.*\(Repeated\s+\d+\s+times\s+-\s+same data\)'

$fwIdSet = New-Object 'System.Collections.Generic.HashSet[string]'
foreach($ef in $fwEfids){ [void]$fwIdSet.Add("800" + $ef.Substring(2)) }   # 0x10234 -> 80010234

$hasLegacyMarkers = $false
$skippedProgress = 0
foreach($ln in $lines){
    if($ln -match $sameDataRe){ $skippedProgress++; continue }   # progress only - not a frame
    if($ln -notmatch $lineRe){ continue }
    # Read every group out of $Matches BEFORE any further -match/-notmatch test runs: a second
    # regex test (even one that only inspects $id) overwrites $Matches as a side effect, which
    # silently dropped $repField here in an earlier version of this fix and made every legacy
    # "(Repeated N times)" line look like a plain 1-frame line instead.
    $id=$Matches[3]; $hUs=[uint64]$Matches[2]; $ds=$Matches[4]; $repField=$Matches[5]
    if(-not $fwIdSet.Contains($id)){ continue }
    if($null -eq $ds){ $ds="" }
    $b=@()
    if($ds.Trim().Length -gt 0){ $b=@($ds.Trim() -split '\s+' | Where-Object {$_ -ne ""} | ForEach-Object { try{[Convert]::ToByte($_,16)}catch{0} }) }
    $ef = "0x" + $id.Substring(3)
    if($repField){
        $hasLegacyMarkers = $true
        $rep = [math]::Max(0, [int]$repField - 1)   # N total, first line already added -> N-1 more
    } else {
        $rep = 1
    }
    for($k=0;$k -lt $rep;$k++){ [void]$idLists[$ef].Add([pscustomobject]@{ Ef=$ef; Us=($hUs+$k); B=$b }) }
}

$counts = @($fwEfids | ForEach-Object { $idLists[$_].Count })
$n = ($counts | Measure-Object -Sum).Sum
if(-not $OutputJson){
    Write-Output ("LOG: " + $Log + "  fw117_trace frames after reconstruction=" + $n + " (skipped " + $skippedProgress + " progress marker(s))")
    Write-Output ("  per-ID reconstructed counts: " + (($fwEfids | ForEach-Object { $i=$fwEfids.IndexOf($_); "$_=$($counts[$i])" }) -join '  '))
}

# ---- 2. assemble into samples (header + its 4 fragments), one of two ways. ----
$samples = @()
if($hasLegacyMarkers){
    # LEGACY (compressed) log: the true interleaved arrival order is gone - accumulation
    # happened independently per ID, so the synthetic timestamps reconstructed above cannot be
    # trusted to interleave the five IDs back into the right order. The only safe reconstruction
    # is INDEX pairing (the i-th header goes with the i-th of each fragment ID) - and that is
    # only valid if all five IDs produced the SAME NUMBER of frames, proving nothing was lost or
    # over/under-counted relative to the others. If the counts disagree, some ID's compression
    # was reconstructed wrong (or the capture was truncated mid-stream) and there is no way to
    # tell which samples would be mispaired - so this refuses to guess and stops instead of
    # printing samples that look plausible but pair the wrong header with the wrong fragments.
    $distinctCounts = @($counts | Select-Object -Unique)
    if($distinctCounts.Count -ne 1){
        throw ("Legacy log: FW-117 ID counts disagree after reconstruction (" + `
            (($fwEfids | ForEach-Object { $i=$fwEfids.IndexOf($_); "$_=$($counts[$i])" }) -join ', ') + `
            ") - index pairing would mispair headers with fragments. Refusing to display samples.")
    }
    $count = $counts[0]
    for($i=0;$i -lt $count;$i++){
        $hdr = $idLists['0x10234'][$i]
        $frag = @{}
        foreach($ef in @('0x10235','0x10236','0x10237','0x10238')){ $frag[$ef] = $idLists[$ef][$i] }
        $samples += [pscustomobject]@{ Hdr=$hdr; Frag=$frag; Dups=@() }
    }
    if(-not $OutputJson){ Write-Output ("  LEGACY log: all five IDs agree at $count - paired by index") }
} else {
    # RAW (uncompressed) log: every frame is real and in true chronological order already
    # (see sniffer.js's FW-117 bypass) - plain assembly: sort by timestamp, a header opens a
    # sample and the next 4 fragments close it. Robust against missing/duplicate/reordered
    # fragments, exactly as before this fix.
    $sorted = @()
    foreach($ef in $fwEfids){ $sorted += $idLists[$ef] }
    $sorted = @($sorted | Sort-Object Us)
    $cur = $null
    foreach($f in $sorted){
        if($f.Ef -eq "0x10234"){
            if($null -ne $cur){ $samples += $cur }
            $cur = [pscustomobject]@{ Hdr=$f; Frag=@{}; Dups=@() }
            continue
        }
        if($null -eq $cur){ continue }
        $k = $f.Ef
        if($cur.Frag.ContainsKey($k)){
            $cur.Dups += $k
        } else {
            $cur.Frag[$k] = $f
        }
    }
    if($null -ne $cur){ $samples += $cur }
    if(-not $OutputJson){ Write-Output ("  RAW log: assembled $($samples.Count) sample(s) in chronological order") }
}

# ---- 3. decode each sample ----
$caps = @{}
foreach($s in $samples){
    $h = $s.Hdr.B
    if($h.Count -lt 8){ $h = $h + @(0,0,0,0,0,0,0,0) }
    $schema = $h[0]; $sid = $h[1]; $capId = $h[3]; $evt = $h[4]
    $fragsOk = ($s.Frag.Count -eq 4)
    $note = @()
    if($s.Dups.Count -gt 0){ $note += ("duplicate:" + (($s.Dups | ForEach-Object { $_.Substring(2) }) -join ",")) }
    if(-not $fragsOk){ $note += ("missing:" + ((@("0x10235","0x10236","0x10237","0x10238") | Where-Object { -not $s.Frag.ContainsKey($_) }) | ForEach-Object { $_.Substring(2) }) -join ",") }

    $obj = [ordered]@{ Schema=$schema; Session=$sid; CapId=$capId; Event=$evt; Frag=$fragsOk; Note=($note -join "; ") }
    if($schema -ne 1){ $obj["Warn"] = "SCHEMA != 1 - decoder assumes FW117_TRACE_SCHEMA_VERSION 1" }
    if($fragsOk){
        $f1=$s.Frag["0x10235"].B; $f2=$s.Frag["0x10236"].B; $f3=$s.Frag["0x10237"].B; $f4=$s.Frag["0x10238"].B
        $st = $f1[5] -band 0x07; $hall = ($f1[5] -shr 3) -band 0x07
        $obj["S"] = [ordered]@{
            Tick=(ToU32 $f1 0); Flags=$f1[4]; State=$st; Hall=$hall
            PiQ=(ToS16 $f1 6); PiD=(ToS16 $f2 0)
            IqReq=(ToS16 $f2 2); IqPre=(ToS16 $f2 4); IqSet=(ToS16 $f2 6)
            IqMeas=(ToS16 $f3 0); IdMeas=(ToS16 $f3 2); Vq=(ToS16 $f3 4); Vd=(ToS16 $f3 6)
            Angle=(ToU16 $f4 0); Ccr1=(ToU16 $f4 2); Ccr2=(ToU16 $f4 4); Ccr3=(ToU16 $f4 6)
        }
    }
    $key = "${sid}:${capId}:${evt}"
    if(-not $caps.ContainsKey($key)){ $caps[$key] = @() }
    $caps[$key] += $obj
}

# ---- 4. report per capture ----
$order = @($caps.Keys | Sort-Object)
if($OutputJson){
    $machineRecords = @()
    foreach($key in $order){
        $machineRecords += @($caps[$key] | Sort-Object { if($_.S){ $_.S.Tick } else { -1 } })
    }
    ConvertTo-Json -InputObject @($machineRecords) -Depth 7 -Compress
    return
}
foreach($key in $order){
    $recs = $caps[$key]
    $recs = @($recs | Sort-Object { if($_.S){ $_.S.Tick } else { -1 } })
    $parts = $key -split ':'
    $capId = [int]$parts[1]; $evt = [int]$parts[2]
    $good = @($recs | Where-Object { $_.S })
    $incomplete = @($recs | Where-Object { -not $_.S })
    $first = $good | Select-Object -First 1
    $last = $good | Select-Object -Last 1
    $schema = $recs[0].Schema
    Write-Output ("=== CAPTURE #{0}  session {1}  event={2}  schema {3}  samples {4}/{5} ===" -f $capId,$parts[0],$evtName[$evt],$schema,$good.Count,$recs.Count)

    if(-not $good){
        Write-Output "  EMPTY CAPTURE (no decodable samples)"
        Write-Output ""
        continue
    }
    $t0 = $first.S.Tick; $t1 = $last.S.Tick
    Write-Output ("  span: {0} -> {1}  ({2:N1} ms @1 kHz)" -f $t0,$t1,(($t1-$t0)/$TICKS_PER_MS))

    # trigger sample (START_CAP flag) = offset 0
    $trig = $good | Where-Object { ($_.S.Flags -band 0x10) -ne 0 } | Select-Object -First 1
    if($null -eq $trig){ $trig = $good[ [int]([math]::Floor($good.Count/2)) ] }
    $trigTick = $trig.S.Tick

    # find MOE edges in the capture
    $moeOn = @($good | Where-Object { ($_.S.Flags -band 0x01) -ne 0 })
    $moeOff = @($good | Where-Object { ($_.S.Flags -band 0x01) -eq 0 })
    $cenDrop = @($good | Where-Object { ($_.S.Flags -band 0x02) -eq 0 -and ($_.S.Flags -band 0x04) -ne 0 })
    Write-Output ("  trigger sample: tick {0}  flags=[{1}]  state={2}  hall={3}" -f $trigTick,(Bits $trig.S.Flags $flagName),$stateName[[int]$trig.S.State],$trig.S.Hall)

    if($cenDrop.Count -gt 0){
        Write-Output ("  !! CEN DISABLE DURING PWM: {0} sample(s) with CEN=0 while PWM_ON - see rows" -f $cenDrop.Count)
    } else {
        Write-Output "  CEN: never disabled while PWM_ON  [OK]"
    }

    # acceptance metrics across the whole capture
    $vqMin = ($good | ForEach-Object { $_.S.Vq } | Measure-Object -Minimum).Minimum
    $vqMax = ($good | ForEach-Object { $_.S.Vq } | Measure-Object -Maximum).Maximum
    $iqMin = ($good | ForEach-Object { $_.S.IqMeas } | Measure-Object -Minimum).Minimum
    $iqMax = ($good | ForEach-Object { $_.S.IqMeas } | Measure-Object -Maximum).Maximum
    $ccrMin = ($good | ForEach-Object { [math]::Min($_.S.Ccr1,[math]::Min($_.S.Ccr2,$_.S.Ccr3)) } | Measure-Object -Minimum).Minimum
    $ccrMax = ($good | ForEach-Object { [math]::Max($_.S.Ccr1,[math]::Max($_.S.Ccr2,$_.S.Ccr3)) } | Measure-Object -Maximum).Maximum
    Write-Output ("  ranges: Vq {0}..{1}  Iq_meas {2}..{3}  CCR1/2/3 {4}..{5}" -f $vqMin,$vqMax,$iqMin,$iqMax,$ccrMin,$ccrMax)

    # the MOE transition rows: find the state-change ticks and print +/-5 samples around them,
    # plus the trigger row; -Full prints every row.
    $trans = New-Object System.Collections.Generic.List[int]
    [void]$trans.Add($trigTick)
    for($i=1;$i -lt $good.Count;$i++){
        $a = $good[$i-1].S; $b = $good[$i].S
        if((($a.Flags -band 0x01) -ne (($b.Flags -band 0x01))) -or
           (($a.Flags -band 0x04) -ne (($b.Flags -band 0x04))) -or
           (($a.Flags -band 0x08) -ne (($b.Flags -band 0x08))) -or
           ($a.State -ne $b.State)){
            [void]$trans.Add($b.Tick)
        }
    }
    $showIdx = New-Object System.Collections.Generic.HashSet[int]
    foreach($tk in $trans){
        for($d=-5;$d -le 5;$d++){
            $idx = [Array]::FindIndex($good, [Predicate[object]]({ param($x) $x.S.Tick -eq $tk }))
            if($idx -ge 0){
                $j = $idx+$d
                if($j -ge 0 -and $j -lt $good.Count){ [void]$showIdx.Add($j) }
            }
        }
    }
    if($Full){ for($j=0;$j -lt $good.Count;$j++){ [void]$showIdx.Add($j) } }

    $allIdx = 0..($good.Count-1) | Where-Object { $showIdx.Contains($_) }
    Write-Output "  #  off(ms)  tick      flags                     st   hall  iq_req  iq_pre  iq_set  iq_meas  id  vq    vd    ang    ccr1  ccr2  ccr3  pi_q  pi_d"
    foreach($j in $allIdx){
        $s = $good[$j].S
        $off = $s.Tick - $trigTick
        $row = ("{0,3}  {1,7}  {2,-8}  {3,-22} {4,-3}  {5}   {6,6}  {7,6}  {8,6}  {9,7}  {10,3}  {11,5}  {12,5}  {13,5}  {14,5}  {15,5}  {16,5}  {17,5}  {18,5}" -f `
            $j,$s.Tick,(TicksMs $off),(Bits $s.Flags $flagName),$stateName[[int]$s.State],$s.Hall,`
            $s.IqReq,$s.IqPre,$s.IqSet,$s.IqMeas,$s.IdMeas,$s.Vq,$s.Vd,$s.Angle,$s.Ccr1,$s.Ccr2,$s.Ccr3,$s.PiQ,$s.PiD)
        Write-Output $row
    }

    foreach($r in $recs | Where-Object { -not $_.S }){
        Write-Output ("  <malformed sample: {0}>" -f $r.Note)
    }

    # ---- acceptance checks ----
    Write-Output "  --- checks ---"
    if($evt -eq 0){
        # START: CCR equal + Vq/Iq quiet before MOE ON
        $onIdx = -1
        for($i=1;$i -lt $good.Count;$i++){
            if((($good[$i].S.Flags -band 0x01) -ne 0) -and (($good[$i-1].S.Flags -band 0x01) -eq 0)){ $onIdx=$i; break }
        }
        if($onIdx -lt 0){
            Write-Output "  START: no MOE 0->1 edge in capture  [?]"
        } else {
            $pre = $good[$onIdx-1].S; $on = $good[$onIdx].S
            $eq = ($pre.Ccr1 -eq $pre.Ccr2) -and ($pre.Ccr2 -eq $pre.Ccr3)
            Write-Output ("  START: MOE ON at row {0} (tick {1}); pre-edge CCR1/2/3={2}/{3}/{4} equal={5}" -f $onIdx,$on.Tick,$pre.Ccr1,$pre.Ccr2,$pre.Ccr3,$eq)
            $dVq = [math]::Abs([int]$on.Vq - [int]$pre.Vq)
            $dVqMax = 0
            for($i=$onIdx;$i -lt [math]::Min($onIdx+10,$good.Count);$i++){
                $d=[math]::Abs([int]$good[$i].S.Vq - [int]$good[$i-1].S.Vq); if($d -gt $dVqMax){ $dVqMax=$d }
            }
            Write-Output ("  START: |dVq| at MOE ON = {0} (max within 10 rows {1}); Iq_meas just after = {2}" -f $dVq,$dVqMax,$on.IqMeas)
            $neg = @($good | Where-Object { $_.S.Tick -ge $on.Tick -and $_.S.Tick -le ($on.Tick+40) -and $_.S.IqMeas -lt 0 -and $_.S.IqSet -ge 0 })
            if($neg.Count -gt 0){ Write-Output "  !! START: negative Iq_meas impulse after bridge-on (inverted torque?)" } else { Write-Output "  START: no negative Iq_meas impulse after bridge-on  [OK]" }
        }
    } else {
        # STOP: ramp 0 + CCR neutral before MOE OFF
        $offIdx = -1
        for($i=1;$i -lt $good.Count;$i++){
            if((($good[$i].S.Flags -band 0x01) -eq 0) -and (($good[$i-1].S.Flags -band 0x01) -ne 0)){ $offIdx=$i; break }
        }
        if($offIdx -lt 0){
            Write-Output "  STOP: no MOE 1->0 edge in capture  [?]"
        } else {
            $pre = $good[$offIdx-1].S; $on = $good[$offIdx].S
            $eq = ($pre.Ccr1 -eq $pre.Ccr2) -and ($pre.Ccr2 -eq $pre.Ccr3)
            Write-Output ("  STOP: MOE OFF at row {0} (tick {1}); pre-edge Iq_set={2} (ramp to 0?) Ccr1/2/3={3}/{4}/{5} equal={6}" -f $offIdx,$on.Tick,$pre.IqSet,$pre.Ccr1,$pre.Ccr2,$pre.Ccr3,$eq)
            $dVq = [math]::Abs([int]$on.Vq - [int]$pre.Vq)
            $dVqMax = 0
            for($i=$offIdx;$i -lt [math]::Min($offIdx+10,$good.Count);$i++){
                $d=[math]::Abs([int]$good[$i].S.Vq - [int]$good[$i-1].S.Vq); if($d -gt $dVqMax){ $dVqMax=$d }
            }
            Write-Output ("  STOP: |dVq| at MOE OFF = {0} (max within 10 rows {1}); Iq_meas just after = {2}" -f $dVq,$dVqMax,$on.IqMeas)
        }
    }
    Write-Output ""
}
