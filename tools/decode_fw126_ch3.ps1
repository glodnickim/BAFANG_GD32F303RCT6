param(
    [Parameter(Mandatory=$true)][string]$Log,
    [switch]$AllBlocks,
    [switch]$OutputJson
)
# FW-126.0: TIMER0_CH3 trigger-edge decoder (neutral-dwell CH3 sweep, DIAG schema 7).
#
# HISTORICAL LOGS ONLY. The producer this decoder reads - the adc_trigger_diag layer and its
# 0x10240..0x10246 aggregate block - was DELETED in FW-126.5, once its question was answered
# (the CH3 conversion starts on the DOWN-count match, measured, HIGH confidence). Firmware from
# DIAG_SCHEMA_VERSION 8 onwards emits no 0x1024x frames at all. This script is kept so the logs
# that DID contain them still read; the source files it names below no longer exist.
#
# WHAT IT DECIDES. Which edge of TIMER0_CH3 actually starts the injected ADC conversion. The
# whole of FW-127 is blocked on that answer (see documentation/FW-126_TO_FW-127_AGENT_HANDOFF_PL.md
# section 1), and guessing it has a 50% chance of moving the sample OUT of the low-side conduction
# window instead of into the middle of it.
#
# THE DERIVATION, from first principles - NOT copied from prose. _T = 3750 (inc/config.h), the
# timer is center-aligned, CCR3 sits near the top, CONV is the conversion length and L the
# trigger-to-ISR latency. L is unknown but CONSTANT, which is exactly why a SWEEP answers this
# and a single reading cannot:
#
#   UP-count match   the match at CNT=CCR3 happens on the way up; the counter then runs the
#                    remaining (_T - CCR3) counts to the top, turns round, and the ISR is entered
#                    on the DOWN slope:
#                        CNT_isr = 2*_T - CCR3 - (CONV + L)        ->  dCNT/dCCR3 = -1
#
#   DOWN-count match the match happens on the way down and the counter simply keeps descending:
#                        CNT_isr = CCR3 - (CONV + L)               ->  dCNT/dCCR3 = +1
#
# So: CH3 stepped DOWN by 40 and CNT going UP by 40 means the UP-count match; CNT going DOWN by
# 40 means the DOWN-count match. Note the two hypotheses put CNT only 2*(_T - CCR3) = 20 counts
# apart at the production CCR3 - the ABSOLUTE value discriminates nothing, only the SLOPE does.
# The intercept then hands over (CONV + L) for free.
#
# CAUTION, DIR AT ISR ENTRY DOES NOT ANSWER THIS. Under both hypotheses the ISR is entered while
# the counter is descending, so DIR=down is expected either way and is reported as evidence about
# the ISR instant only, never as the verdict.
#
# WARNING - CONFLICTING PROSE. documentation/FW-126_PHASE_CURRENT_ACQUISITION_HARDENING_PL.md
# states the opposite mapping ("CNT malejacy o okolo 40 oznacza UP"). That sentence contradicts
# the derivation above and the one in inc/adc_trigger_diag.h. This decoder follows the derivation
# and prints it, so the verdict can be checked rather than believed.
#
# SECOND, INDEPENDENT DISCRIMINATOR. Under the UP-count hypothesis the two CC3 events are
# 2*(_T - CCR3) counts apart; once that exceeds the conversion length the second one is no longer
# swallowed and a SECOND conversion starts in the same PWM period. Across this sweep the widest
# separation is 2*(3750-3660) = 180 counts, well under the ~408-count conversion, so the ISR rate
# must stay at 4 per 4 kHz control tick throughout. A measured ~8 means BOTH edges are live and is
# a STOP condition, not a verdict.
#
# WIRE FORMAT - the actual producer, src/adc_trigger_diag.c adc_trigger_diag_aggregate_frame():
#
#   0x10240 status : [0] state (0 IDLE 1 RUNNING 2 DONE 3 ABORTED)  [1] completed points
#                    [2:3] total ISR sequence BE(U16)  [4] max ISR per control tick
#                    [5] ADC0_LATE  [6] ADC1_LATE  [7] ADC2_LATE   (all u8-saturated)
#   0x10241..43 pt : [0] point index | 0x80 when complete   [1] CH3 - 3500
#                    [2:3] CNT at ISR entry BE(U16)   [4:5] ISR sequence low16 BE(U16)
#                    [6] bridge lifecycle
#                    [7] bit0 DIR_DOWN  bit1 ADC0_EOIC  bit2 ADC1_EOIC  bit3 ADC2_EOIC
#                        bit4 POEN      bit5 DWELL
#   0x10244..46 raw: [0] 0xA0|phase (A=ADC2, B=ADC1, C=ADC0)
#                    [1:6] three BE(U16) raw JDR, one per sweep point   [7] 0x80 = all 3 points
#
# SCHEMA DISCRIMINATION. These seven ids are shared with the legacy FW-121 dark-bridge sweep
# (schema 6), whose frames mean something else entirely. Data0 of 0x10244 separates them without
# ambiguity: 0xA0 = FW-126 neutral (schema 7), 0xA1 = legacy ABORTED chronology snapshot,
# 0x83 = legacy point frame. Anything else is refused rather than guessed at.
$ErrorActionPreference = "Stop"
trap { Write-Output "SCRIPT ERR line $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message) :: $($_.InvocationInfo.Line.Trim())"; break }

$T_PERIOD   = 3750     # _T, inc/config.h

$TICK_HZ    = 4000     # CONTROL_TIMEBASE_HZ
$ISR_PER_TICK_EXPECT = 4    # one injected conversion per PWM period at 16 kHz
$SLOPE_TOL  = 0.30     # a 40-count step tolerates ~12 counts of latency jitter per point
$SAMPLES_TARGET = 7    # FW-126.2: accepted conversions per CH3 value
$SAMPLES_MIN = 5       # ...below this a point is not evidence

$stateName = @{0="IDLE";1="RUNNING";2="DONE";3="ABORTED"}
$lifeName  = @{0="IDLE";1="NEUTRAL_COMMIT";2="MOE_ON";3="NEUTRAL_DWELL";4="FOC_RELEASE";5="RUN"}   # src/main.c:566

function ToU16($d,$o){ if($d.Count -le ($o+1)){ return $null }; return (([int]$d[$o] -shl 8) -bor [int]$d[$o+1]) -band 0xFFFF }

# ---- 1. collect the seven ids from the log ------------------------------------------------
#
# The sniffer writes two DIFFERENT "(Repeated ...)" markers and they mean opposite things (see
# the same handling in tools/decode_fw117_trace.ps1):
#   "(Repeated N times - same data)"  progress only, logged every 100th identical repeat while
#                                     the accumulator is still counting. NOT a frame. Its trailing
#                                     text is not hex and parsing it would corrupt the byte array.
#   "(Repeated N times)"              the final summary, written once. N is the TOTAL including
#                                     the plain first line already counted, so it contributes N-1.
# This matters more here than anywhere else: after the sweep reaches DONE its aggregate frames are
# frozen, so EVERY session summary repeats byte-identical values - compression is the normal case.
$lines = Get-Content -LiteralPath $Log
$fwEfids = @('0x10240','0x10241','0x10242','0x10243','0x10244','0x10245','0x10246')
$idSet = New-Object 'System.Collections.Generic.HashSet[string]'
foreach($ef in $fwEfids){ [void]$idSet.Add("800" + $ef.Substring(2)) }   # 0x10240 -> 80010240

$lineRe     = '^\[(\d{2}:\d{2}:\d{2})\]\s+\[INFO\]\s+(\d+)\s+ID:([0-9A-F]+)\s+DLC:\d+\s+Data:((?:[0-9A-Fa-f]{2}\s*)*)(?:\(Repeated\s+(\d+)\s+times\))?\s*$'
$sameDataRe = 'ID:8001024[0-6]\b.*\(Repeated\s+\d+\s+times\s+-\s+same data\)'

$frames = New-Object System.Collections.ArrayList
$skippedProgress = 0
$hasLegacyMarkers = $false
foreach($ln in $lines){
    if($ln -match $sameDataRe){ $skippedProgress++; continue }
    if($ln -notmatch $lineRe){ continue }
    # Read every capture group out of $Matches BEFORE any further -match runs: a second regex test
    # overwrites $Matches as a side effect, which is how an earlier decoder silently lost its
    # repeat field and under-counted every compressed run.
    $id=$Matches[3]; $hUs=[uint64]$Matches[2]; $ds=$Matches[4]; $repField=$Matches[5]
    if(-not $idSet.Contains($id)){ continue }
    if($null -eq $ds){ $ds="" }
    $b=@()
    if($ds.Trim().Length -gt 0){ $b=@($ds.Trim() -split '\s+' | Where-Object {$_ -ne ""} | ForEach-Object { try{[Convert]::ToByte($_,16)}catch{0} }) }
    $ef = "0x" + $id.Substring(3)
    if($repField){ $hasLegacyMarkers = $true; $rep = [math]::Max(0, [int]$repField - 1) } else { $rep = 1 }
    for($k=0;$k -lt $rep;$k++){ [void]$frames.Add([pscustomobject]@{ Ef=$ef; Us=($hUs+$k); B=$b }) }
}

$frames = @($frames | Sort-Object Us)
if(-not $OutputJson){
    Write-Output ("LOG: " + $Log)
    Write-Output ("  FW-126 frames after reconstruction=" + $frames.Count + " (skipped " + $skippedProgress + " progress marker(s))")
    # $_ inside the inner Where-Object would rebind to the inner pipeline element, so the id being
    # counted is captured in a named variable first.
    $perId = foreach($ef in $fwEfids){ $c = @($frames | Where-Object { $_.Ef -eq $ef }).Count; "$ef=$c" }
    Write-Output ("  per-ID: " + ($perId -join '  '))
}
if($frames.Count -eq 0){
    Write-Output ""
    Write-Output "VERDICT: NO DATA - the log contains no 0x10240..0x10246 frame at all."
    Write-Output "  A DIAG image (0.0429) emits these only inside a SESSION SUMMARY, which is sent"
    Write-Output "  after ~3 s of quiet following a ride. Log a real assist start, then stop and wait."
    return
}

# ---- 2. assemble the aggregate block ---------------------------------------------------------
#
# The seven ids are ONE snapshot of the sweep state, re-sent with every session summary. Once the
# sweep reaches DONE that state is frozen, so the normal log contains N byte-identical copies of
# each id and there is exactly one thing to decode. That is the case handled first, because it is
# both the common one and the only one a COMPRESSED log can be read from at all: the accumulator
# counted each id independently, so the timestamps reconstructed above cannot re-interleave the
# ids into their true arrival order, and pairing frames chronologically would be pairing on
# fiction. Same refusal-to-guess as tools/decode_fw117_trace.ps1's legacy branch.
$hex = { param($b) ($b | ForEach-Object { "{0:X2}" -f $_ }) -join ' ' }
$variants = @{}
foreach($ef in $fwEfids){
    $variants[$ef] = @($frames | Where-Object { $_.Ef -eq $ef } | ForEach-Object { & $hex $_.B } | Select-Object -Unique)
}
$missing = @($fwEfids | Where-Object { $variants[$_].Count -eq 0 })
$changed = @($fwEfids | Where-Object { $variants[$_].Count -gt 1 })

if($missing.Count -gt 0){
    Write-Output ""
    Write-Output ("VERDICT: INCOMPLETE - these ids never appeared: " + ($missing -join ', ') + ". Refusing to guess.")
    Write-Output "  All seven are built together by diag_build_aggregate(); missing ones mean a truncated capture."
    return
}

$toDecode = @()
if($changed.Count -eq 0){
    # One frozen state, however many times it was sent. Take the last copy of each id.
    $blk = @{}
    foreach($ef in $fwEfids){ $blk[$ef] = @($frames | Where-Object { $_.Ef -eq $ef })[-1] }
    $toDecode = @($blk)
    if(-not $OutputJson){
        Write-Output ("  one frozen aggregate state (every id byte-identical across the capture)")
    }
} elseif($hasLegacyMarkers){
    # Contents changed AND the log is compressed: chronological pairing is not available and
    # index pairing is only valid if every id changed in lock-step, which nothing here proves.
    Write-Output ""
    Write-Output ("VERDICT: AMBIGUOUS - the sweep state changed during a COMPRESSED capture (ids that vary: " +
                  ($changed -join ', ') + ").")
    Write-Output "  The sniffer's repeat accumulator counts each id independently, so the true interleaving is"
    Write-Output "  gone and any pairing of these frames would be invented. Re-capture with the FW-126 ids"
    Write-Output "  bypassing the accumulator, or capture a single power cycle with one ride only."
    foreach($ef in $changed){
        Write-Output ("    " + $ef + " distinct payloads:")
        foreach($v in $variants[$ef]){ Write-Output ("      " + $v) }
    }
    return
} else {
    # Contents changed in an UNCOMPRESSED log: arrival order is real, so a 0x10240 opens a block.
    $blocks = New-Object System.Collections.ArrayList
    $cur = $null
    foreach($f in $frames){
        if($f.Ef -eq '0x10240'){
            if($null -ne $cur){ [void]$blocks.Add($cur) }
            $cur = @{ '0x10240' = $f }
        } elseif($null -ne $cur -and -not $cur.ContainsKey($f.Ef)){
            $cur[$f.Ef] = $f
        }
    }
    if($null -ne $cur){ [void]$blocks.Add($cur) }
    $complete = @($blocks | Where-Object { $b=$_; @($fwEfids | Where-Object { -not $b.ContainsKey($_) }).Count -eq 0 })
    if($complete.Count -eq 0){
        Write-Output ""
        Write-Output "VERDICT: INCOMPLETE - no aggregate block carried all seven ids. Refusing to guess."
        return
    }
    if(-not $OutputJson){
        Write-Output ("  aggregate blocks: " + $blocks.Count + " seen, " + $complete.Count + " complete (all 7 ids)")
        Write-Output ("  NOTE: the sweep state CHANGED during the capture (ids that vary: " + ($changed -join ', ') + ").")
        Write-Output ("        Only the LAST complete block is used for the verdict; -AllBlocks shows them all.")
    }
    $toDecode = if($AllBlocks){ $complete } else { @($complete[-1]) }
}

# ---- 3. decode one block -------------------------------------------------------------------
function Get-DecodedBlock($blk){
    $st = $blk['0x10240'].B
    $d4 = $blk['0x10244'].B
    $schemaTag = if($d4.Count -gt 0){ [int]$d4[0] } else { -1 }
    # Data0 of 0x10244 identifies the layout with no state at all:
    #   0xB1 = FW-126.2 schema 8 (point 1 frame B)   0xA0 = FW-126.0 schema 7 (raw phase A)
    #   0xA1 / 0x83 = legacy FW-121 dark-bridge frames, which mean something else entirely.
    $schema = switch($schemaTag){
        0xB1 { "FW-126.2 neutral dwell, medians (schema 8)" }
        0xA0 { "FW-126.0 neutral dwell, single sample (schema 7)" }
        0xA1 { "LEGACY FW-121 dark-bridge, ABORTED (schema 6)" }
        0x83 { "LEGACY FW-121 dark-bridge, points (schema 6)" }
        default { "UNKNOWN" }
    }
    $ver = switch($schemaTag){ 0xB1 { 8 } 0xA0 { 7 } default { 0 } }
    $r = [ordered]@{
        schema         = $schema
        schema_tag     = ("0x{0:X2}" -f $schemaTag)
        schema_version = $ver
        state          = [int]$st[0]
        state_name     = $stateName[[int]$st[0]]
        points_done    = [int]$st[1]
        isr_seq_total  = (ToU16 $st 2)
        isr_per_tick   = [int]$st[4]
        adc0_late      = [int]$st[5]
        adc1_late      = [int]$st[6]
        adc2_late      = [int]$st[7]
        points         = @()
    }

    # FW-126.2 schema 8: TWO frames per point. The MEDIAN is the point's answer; min/max and the
    # sample counts travel beside it so "the readings were stable" is visible, not assumed.
    if($ver -eq 8){
        for($i=0;$i -lt 3;$i++){
            $a = $blk[("0x1024" + (1 + 2*$i))].B
            $b = $blk[("0x1024" + (2 + 2*$i))].B
            $fl = [int]$a[7]
            $r.points += [ordered]@{
                index          = $i
                ccr3           = (3500 + [int]$a[1])
                ccr3_readback  = (3500 + [int]$a[2])
                accepted       = [int]$a[3]
                cnt            = (ToU16 $a 4)      # the median - the slope is computed from this
                rejected       = [int]$a[6]
                dir_all_down   = (($fl -band 0x01) -ne 0)
                dir_any_up     = (($fl -band 0x02) -ne 0)
                eoic_all       = (($fl -band 0x04) -ne 0)
                complete       = (($fl -band 0x08) -ne 0)
                poen           = (($fl -band 0x10) -ne 0)
                dwell          = (($fl -band 0x20) -ne 0)
                cnt_min        = (ToU16 $b 1)
                cnt_max        = (ToU16 $b 3)
                spread         = [int]$b[5]
                isr_seq        = (ToU16 $b 6)
            }
        }
        return $r
    }

    # FW-126.0 schema 7, kept so the logs already captured stay readable. One sample per point -
    # which is precisely the weakness FW-126.2 exists to remove.
    if($ver -ne 7){ return $r }
    for($i=0;$i -lt 3;$i++){
        $p = $blk[("0x1024" + (1+$i))].B
        $fl = [int]$p[7]
        $c = (ToU16 $p 2)
        $r.points += [ordered]@{
            index          = ([int]$p[0] -band 0x7F)
            ccr3           = (3500 + [int]$p[1])
            ccr3_readback  = $null
            accepted       = $(if(([int]$p[0] -band 0x80) -ne 0){1}else{0})
            cnt            = $c
            rejected       = 0
            dir_all_down   = (($fl -band 0x01) -ne 0)
            dir_any_up     = (($fl -band 0x01) -eq 0)
            eoic_all       = ((($fl -band 0x02) -ne 0) -and (($fl -band 0x04) -ne 0) -and (($fl -band 0x08) -ne 0))
            complete       = ((([int]$p[0]) -band 0x80) -ne 0)
            poen           = (($fl -band 0x10) -ne 0)
            dwell          = (($fl -band 0x20) -ne 0)
            cnt_min        = $c
            cnt_max        = $c
            spread         = 0
            isr_seq        = (ToU16 $p 4)
            lifecycle      = [int]$p[6]
            life_name      = $(if($lifeName.ContainsKey([int]$p[6])){$lifeName[[int]$p[6]]}else{"?"})
            raw_a          = $null; raw_b = $null; raw_c = $null
        }
    }
    foreach($ph in @(@(4,'raw_a'),@(5,'raw_b'),@(6,'raw_c'))){
        $d = $blk[("0x1024" + $ph[0])].B
        for($i=0;$i -lt 3;$i++){ $r.points[$i][$ph[1]] = (ToU16 $d (1 + 2*$i)) }
    }
    return $r
}

function Get-Verdict($r){
    $v = [ordered]@{
        edge = "INCONCLUSIVE"; confidence = "NONE"; reason = ""
        slopes = @(); conv_plus_latency = $null; conv_plus_latency_ns = $null
        both_edges = $false; stop_conditions = @()
    }
    if($r.schema_version -eq 0){
        $v.reason = "block is not an FW-126 neutral-dwell layout (" + $r.schema + ") - the fields mean something else"
        return $v
    }
    # A point is evidence only when it is complete, held the dwell, and - on schema 8 - carries
    # at least SAMPLES_MIN accepted conversions.
    $minSamples = if($r.schema_version -ge 8){ $SAMPLES_MIN } else { 1 }
    $usable = @($r.points | Where-Object { $_.complete -and $_.dwell -and $_.accepted -ge $minSamples })

    # THREE usable points, i.e. TWO independent segments. One pair cannot separate a real slope
    # from a single disturbed reading - that is exactly what the 17:44 ride was left with, and
    # FW-126.2 forbids calling UP or DOWN on it. A polarity that is 50% likely to be backwards
    # is not a weaker answer, it is a wrong one.
    if($usable.Count -lt 3){
        $v.reason = "only $($usable.Count) usable point(s) - a verdict needs three, i.e. two independent segments"
        $v.stop_conditions += $(if($usable.Count -lt 2){
            "fewer than 2 usable sweep points"
        } else {
            "only one usable pair - a single pair is not a verdict (FW-126.2)"
        })
        return $v
    }

    # Hard gates. A verdict read off a run with late conversions, a lost interlock or a compare
    # that was not the one requested is evidence about the fault, not about the trigger.
    if($r.adc0_late -gt 0){ $v.stop_conditions += "ADC0_LATE=$($r.adc0_late) - phase C conversion had not finished at ISR entry" }
    if($r.adc1_late -gt 0 -and $r.schema_version -lt 8){ $v.stop_conditions += "ADC1_LATE=$($r.adc1_late) - ISR entered without a completed ADC1 group" }
    if($r.adc2_late -gt 0){ $v.stop_conditions += "ADC2_LATE=$($r.adc2_late) - phase A conversion had not finished at ISR entry" }
    if($r.isr_per_tick -ge ($ISR_PER_TICK_EXPECT * 2)){
        $v.both_edges = $true
        $v.stop_conditions += "ISR/control tick = $($r.isr_per_tick) (expected $ISR_PER_TICK_EXPECT) - two conversions per PWM period, i.e. BOTH edges are live"
    }
    if($r.schema_version -ge 8){
        foreach($p in $usable){
            # The firmware discards the first interrupt after MOE ON, so an accepted sample
            # entered while counting UP is something neither hypothesis predicts.
            if($p.dir_any_up){ $v.stop_conditions += "point $($p.index): an accepted sample entered while counting UP" }
            if(-not $p.eoic_all){ $v.stop_conditions += "point $($p.index): not every accepted sample had all three EOIC" }
            if($p.ccr3_readback -ne $p.ccr3){ $v.stop_conditions += "point $($p.index): CH3 readback $($p.ccr3_readback) is not the requested $($p.ccr3)" }
            if($p.accepted -lt $SAMPLES_TARGET){ $v.stop_conditions += "point $($p.index): only $($p.accepted) of $SAMPLES_TARGET conversions accepted" }
        }
    }

    for($i=1;$i -lt $usable.Count;$i++){
        $dc = $usable[$i].ccr3 - $usable[$i-1].ccr3
        if($dc -eq 0){ continue }
        $v.slopes += [math]::Round((($usable[$i].cnt - $usable[$i-1].cnt) / [double]$dc), 3)
    }
    if($v.slopes.Count -eq 0){ $v.reason = "all usable points share one CH3 value - no slope to measure"; return $v }

    $isUp   = @($v.slopes | Where-Object { [math]::Abs($_ - (-1.0)) -le $SLOPE_TOL }).Count -eq $v.slopes.Count
    $isDown = @($v.slopes | Where-Object { [math]::Abs($_ - ( 1.0)) -le $SLOPE_TOL }).Count -eq $v.slopes.Count

    if($v.both_edges){
        $v.edge = "BOTH"; $v.confidence = "MEASURED"
        $v.reason = "doubled ISR rate - the second CC3 event is no longer being swallowed"
    } elseif($isUp){
        $v.edge = "UP"; $v.reason = "dCNT/dCCR3 = -1: the conversion starts at the UP-count match and the ISR lands on the down slope"
        $v.conv_plus_latency = [int]([math]::Round((@($usable | ForEach-Object { 2*$T_PERIOD - $_.ccr3 - $_.cnt }) | Measure-Object -Average).Average))
    } elseif($isDown){
        $v.edge = "DOWN"; $v.reason = "dCNT/dCCR3 = +1: the conversion starts at the DOWN-count match and the counter keeps descending"
        $v.conv_plus_latency = [int]([math]::Round((@($usable | ForEach-Object { $_.ccr3 - $_.cnt }) | Measure-Object -Average).Average))
    } else {
        $v.reason = "slopes " + ($v.slopes -join ', ') + " match neither -1 nor +1 within +/-$SLOPE_TOL"
        $v.stop_conditions += "slope does not resolve to a single hypothesis"
    }
    if($null -ne $v.conv_plus_latency){
        # TIMER0 runs at 120 MHz, so one count is 8.333 ns.
        $v.conv_plus_latency_ns = [math]::Round($v.conv_plus_latency * (1000.0/120.0), 1)
    }
    if($v.edge -in @("UP","DOWN")){
        $v.confidence = if($v.stop_conditions.Count -eq 0 -and $v.slopes.Count -ge 2){ "HIGH" } else { "LOW - see stop conditions" }
    }
    return $v
}


$results = @()
foreach($blk in $toDecode){
    $r = Get-DecodedBlock $blk
    $v = Get-Verdict $r
    $results += [pscustomobject]@{ decoded=$r; verdict=$v }
}

if($OutputJson){
    $results | ConvertTo-Json -Depth 8
    return
}

foreach($res in $results){
    $r = $res.decoded; $v = $res.verdict
    Write-Output ""
    Write-Output "=============================================================================="
    Write-Output ("SCHEMA : " + $r.schema + "   (0x10244 Data0 = " + $r.schema_tag + ")")
    Write-Output ("STATE  : " + $r.state + " " + $r.state_name + "   points completed: " + $r.points_done + "/3")
    Write-Output ("ISR    : total sequence " + $r.isr_seq_total + "   max per control tick " + $r.isr_per_tick +
                  " (expected " + $ISR_PER_TICK_EXPECT + " = one conversion per PWM period)")
    Write-Output ("LATE   : ADC0=" + $r.adc0_late + "  ADC1=" + $r.adc1_late + "  ADC2=" + $r.adc2_late + "   (all must be 0)")
    if($r.schema_version -eq 0){
        Write-Output ""
        Write-Output ("VERDICT: INCONCLUSIVE - " + $v.reason)
        continue
    }
    Write-Output ""
    # One row per CH3 value. On schema 8 the answer is the MEDIAN and the range beside it says
    # how far the samples moved; on schema 7 there was only ever one reading, so n=1 and the
    # range collapses onto it. That difference is the whole of FW-126.2.
    Write-Output "  pt  CH3req CH3back  n  rej   MEDIAN    dMED     min     max  spr  ISRseq   DIR   EOIC DWELL"
    $prev = $null
    foreach($p in $r.points){
        $dc = if($null -eq $prev){ "      -" } else { "{0,7}" -f ($p.cnt - $prev.cnt) }
        $rb = if($null -eq $p.ccr3_readback){ "      -" } else { "{0,7}" -f $p.ccr3_readback }
        Write-Output ("  {0}{1} {2,6} {3} {4,2} {5,4} {6,8} {7} {8,7} {9,7} {10,4} {11,7}  {12,-5} {13,-4} {14}" -f `
            $p.index, $(if($p.complete){"*"}else{"!"}), $p.ccr3, $rb, $p.accepted, $p.rejected,
            $p.cnt, $dc, $p.cnt_min, $p.cnt_max, $p.spread, $p.isr_seq,
            $(if($p.dir_any_up){"MIXED"}else{"down"}),
            $(if($p.eoic_all){"YES"}else{"NO"}),
            $(if($p.dwell){"YES"}else{"NO"}))
        $prev = $p
    }
    Write-Output "  (* = complete, ! = incomplete.  n = accepted conversions, rej = refused here."
    Write-Output "   DIR is the counter at ISR ENTRY and discriminates NOTHING - both hypotheses are"
    Write-Output "   entered on the down slope. Only the SLOPE below decides.)"
    Write-Output ""
    Write-Output ("SLOPE dCNT/dCCR3 : " + ($v.slopes -join ', ') + "    (-1 => UP-count match, +1 => DOWN-count match)")
    if($null -ne $v.conv_plus_latency){
        Write-Output ("CONV + LATENCY   : " + $v.conv_plus_latency + " counts = " + $v.conv_plus_latency_ns + " ns at 120 MHz")
    }
    Write-Output ""
    Write-Output ("VERDICT          : " + $v.edge + "    confidence: " + $v.confidence)
    Write-Output ("REASON           : " + $v.reason)
    if($v.stop_conditions.Count -gt 0){
        Write-Output ""
        Write-Output "STOP CONDITIONS (handoff section 9 - do NOT implement FW-127 while any of these stands):"
        foreach($s in $v.stop_conditions){ Write-Output ("  - " + $s) }
    }
    Write-Output ""
    Write-Output "ANSWERS TO HANDOFF SECTION 8 (only what THIS frame set can prove):"
    # The sweep closes after three accepted points, so it can span FEWER interrupts than one
    # 4 kHz control tick contains. Dividing by the expected count then presents a truncated
    # window as a rate - "0.75 conversions per PWM period" is an artifact, not a measurement.
    $rateOk = ($r.isr_seq_total -ge ($ISR_PER_TICK_EXPECT * 2))
    Write-Output ("  inserted conversions per PWM period : " + $(if($rateOk -and $r.isr_per_tick -gt 0){ (($r.isr_per_tick / [double]$ISR_PER_TICK_EXPECT)).ToString("0.##",[cultureinfo]::InvariantCulture) }else{"NOT MEASURABLE"}) +
                  "   (from " + $r.isr_per_tick + " ISR per " + $TICK_HZ + " Hz control tick, " + $r.isr_seq_total + " ISR total)")
    if(-not $rateOk){
        Write-Output ("    the sweep spanned only " + $r.isr_seq_total + " interrupt(s) - too short to hold a full control tick,")
        Write-Output ("    so this is a window artifact, not a rate")
    }
    Write-Output ("  conversion on UP   : " + $(if($v.edge -eq "UP"){"YES"}elseif($v.edge -eq "BOTH"){"YES"}elseif($v.edge -eq "DOWN"){"NO"}else{"?"}))
    Write-Output ("  conversion on DOWN : " + $(if($v.edge -eq "DOWN"){"YES"}elseif($v.edge -eq "BOTH"){"YES"}elseif($v.edge -eq "UP"){"NO"}else{"?"}))
    Write-Output ("  BOTH / two triggers: " + $(if($v.both_edges){"YES"}elseif($v.edge -in @("UP","DOWN")){"NO"}else{"?"}))
    # On schema 8 a non-zero ADC1_LATE is EXPECTED and harmless: that interrupt was REFUSED,
    # never sampled, and the point simply waited for another one - which is the whole point of
    # FW-126.2. What must hold is that every sample which DID enter a median had all three
    # groups complete. Counting the refusal as incoherence (as schema 7 did, correctly for its
    # own contract) contradicts the panel and reads as a fault where there is none.
    $eoicOk = if($r.schema_version -ge 8){
        (($r.adc0_late + $r.adc2_late) -eq 0) -and (@($r.points | Where-Object {$_.eoic_all}).Count -eq 3)
    } else {
        (($r.adc0_late + $r.adc1_late + $r.adc2_late) -eq 0) -and (@($r.points | Where-Object {$_.eoic_all}).Count -eq 3)
    }
    Write-Output ("  EOIC coherent with the observed conversion : " + $(if($eoicOk){"YES"}else{"NO"}) +
                  $(if($r.schema_version -ge 8 -and $r.adc1_late -gt 0){
                        "   (ADC1_LATE=$($r.adc1_late) refused before sampling - by design, not a fault)"
                    }else{""}))
    $seqs = @($r.points | Where-Object {$_.complete} | ForEach-Object { $_.isr_seq })
    $mono = ($seqs.Count -ge 2) -and (@(1..([math]::Max(1,$seqs.Count-1)) | Where-Object { $seqs[$_] -le $seqs[$_-1] }).Count -eq 0)
    Write-Output ("  sample/state sequence unambiguous          : " + $(if($mono){"YES (strictly increasing: " + ($seqs -join ' < ') + ")"}else{"NO"}))
    Write-Output ""
    Write-Output "NOT ANSWERED HERE - read 0x602D for these (handoff section 21 items 12-14, 16):"
    Write-Output "  calibration sample count, MOE-off proof, offsets A/B/C stability, stale Iq/Id in START TRACE."
}
