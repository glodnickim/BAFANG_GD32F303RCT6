param(
    [Parameter(Mandatory=$true)][string]$Log,
    [switch]$OutputJson
)
# FW-112 A/B: rearm-episode logger decoder.
#
# Wire format is the ACTUAL producer serializer (src/main.c diag_fw112ab_frame)
# over the record layout in inc/fw112_ab.h / src/fw112_ab.c:
#
#   header frame 0x0001022F  (8 B):
#       [0]=schema (FW112_AB_SCHEMA_VERSION), [1]=session_id,
#       [2:3]=episode_id BE, [4]=record_type (0=CONFIG,1=SAMPLE),
#       [5]=ms_and_idx (SAMPLE only, 0 for CONFIG), [6:7]=0
#   data frames 0x00010230..0x00010233 (4 x 8 B) - see CONFIG/SAMPLE layout below.
#
# Every field below is decoded from the serializer order exactly; nothing is inferred.
#
# CONFIG (record_type 0), frag1 0x10230:  [0]=assist_level [1]=bank_index [2]=mode_type
#   [3]=emtb_parameter [4:5]=support_ratio_pct BE [6]=max_iq_pct [7]=flags(CFG)
#   frag2 0x10231:  [0]=reserved2 [1:2]=emtb_reference_voltage_mv BE [3:4]=max_motor_power_w BE
#   [5:6]=controller_temperature_c BE [7]=0
#   frag3 0x10232:  [0:1]=battery_voltage_mv_div10 BE [2:3]=load_threshold_centikg BE
#   [4]=required_steps [5]=start_steps [6]=cadence_rpm_at_arm [7]=reserved3
#   frag4 0x10233:  [0:3]=arm_tick BE [4:7]=0
#
# SAMPLE (record_type 1), frag1 0x10230:  [0]=assist_level [1:2]=tick_offset BE(S16)
#   [3:4]=torque_for_assist_mv BE [5:6]=load_centikg BE [7]=cadence_rpm
#   frag2 0x10231:  [0]=session_state [1]=recovery_state [2]=dir_state [3]=flags(FLAG)
#   [4]=assist_hold_ticks [5:6]=iq_request BE(S16) [7]=0
#   frag3 0x10232:  [0:1]=iq_after_latch_floor BE(S16) [2:3]=iq_pre_ramp BE(S16)
#   [4:5]=iq_setpoint BE(S16) [6]=controller_temperature_c(S8)
#   [7]=arun_native_clamped (schema>=2; ALWAYS 0 in schema 1 - was "reserved")
#   frag4 0x10233:  [0:1]=battery_voltage_mv_div10 BE
#   [2:3]=afilt_native BE (schema>=2; ALWAYS 0 in schema 1 - was "reserved2") [4:7]=0
#
# FW-112 TWO-MECHANISM DIAGNOSTIC (schema 2): afilt (assist_delta_filtered_native) and arun
# (torque_run_filtered, clamped to 0..255 native, saturating) are the DIRECT signals - use
# THESE for a root-cause decision, not TorqueMv (torque_for_assist_mv), which is a boosted,
# live-substituted PROXY for arun and can be inflated by the startup-boost curve. flags bit
# 0x20 (RECOVERY_ACTIVE) restates recovery_state != IDLE for convenience; Recovery itself
# (session_state/recovery_state) remains authoritative and can never disagree with it.
# PathClass below is derived PURELY from Recovery, per the card's own classification rule:
#   REARM PATH         Recovery = WAIT_FRESH or TRACK_FAST (afilt/arun are the SAME
#                       live-substituted value by construction - see torque_input.c)
#   ORDINARY RUN PATH   Recovery = IDLE (arun is the independent 48-step window average)
$ErrorActionPreference = "Stop"
trap { Write-Output "SCRIPT ERR line $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message) :: $($_.InvocationInfo.Line.Trim())"; break }

# Tick rate used everywhere in this project (CONTROL_TIMEBASE_HZ).
$TICKS_PER_MS = 4.0

$msName = @{
    0="SCHEDULED"; 1="REVOKED"; 2="GRANTED"; 3="TORQUE_POSITIVE"; 4="MODE_DEMAND_POSITIVE";
    5="IQ_REQUEST_POSITIVE"; 6="PRE_RAMP_POSITIVE"; 7="SETPOINT_POSITIVE"
}
$recTypeName = @{0="CONFIG"; 1="SAMPLE"}
$cfgFlagName = @{0x01="BASED_ON_POWER"; 0x02="CADENCE_COMP"; 0x04="ASSIST_WITHOUT_ROT"; 0x08="LEVEL_ZERO"}
$flagName = @{0x01="LATCHED"; 0x02="PWM_ON"; 0x04="SENSOR_VALID"; 0x08="WHEEL_VALID"; 0x10="ROLLING_COAST"; 0x20="RECOVERY_ACTIVE"}
$sessionName = @{0="COLD"; 1="ACTIVE"; 2="SUSP_DIR"; 3="WAIT_REARM"}
$recovName = @{0="IDLE"; 1="WAIT_FRESH"; 2="TRACK_FAST"}
$dirName = @{0="FWD_SAFE"; 1="DIR_INHIBIT"; 2="FWD_CONFIRM"}
$modeName = @{0="LINEAR"; 1="EBIKE"; 2="EBIKE_EMTB"; 3="SPORT"; 4="DEFAULT"; 5="STOCK"; 6="POWER_CURVE"}

function ToU16($d,$o){ return (([int]$d[$o] -shl 8) -bor [int]$d[$o+1]) -band 0xFFFF }
function ToS16($d,$o){ $v = ToU16 $d $o; if($v -ge 0x8000){ $v = $v - 0x10000 }; return $v }
function ToU32($d,$o){ return (([int]$d[$o] -shl 24) -bor ([int]$d[$o+1] -shl 16) -bor ([int]$d[$o+2] -shl 8) -bor [int]$d[$o+3]) }
function ToS8($d,$o){ $v = [int]$d[$o]; if($v -ge 0x80){ $v = $v - 0x100 }; return $v }
function Bits($v,$names){ return (($names.GetEnumerator() | Where-Object { ($v -band $_.Key) -ne 0 } | ForEach-Object { $_.Value }) -join "+") }
function TicksMs($v){ return ("{0} ticks = {1:N1} ms" -f $v, ($v / $TICKS_PER_MS)) }

# ---- 1. collect frames from the log, honouring the sniffer's two "(Repeated ...)" markers
#         correctly (see tools/decode_fw117_trace.ps1's header for the full reasoning this
#         mirrors): "(Repeated N times - same data)" is a PROGRESS marker, never a frame;
#         the final "(Repeated N times)" means N occurrences total INCLUDING the plain line
#         already counted once, so reconstruction adds N-1, not N+1. A second -match/-notmatch
#         test (e.g. filtering by id) must never run between reading $lineRe's match and
#         reading $Matches, or it silently clobbers $Matches - use a HashSet check instead. ----
$lines = Get-Content -LiteralPath $Log
$wantIds = New-Object 'System.Collections.Generic.HashSet[string]'
foreach($id in @("8001022F","80010230","80010231","80010232","80010233")){ [void]$wantIds.Add($id) }
$lineRe = '^\[(\d{2}:\d{2}:\d{2})\]\s+\[INFO\]\s+(\d+)\s+ID:([0-9A-F]+)\s+DLC:\d+\s+Data:((?:[0-9A-Fa-f]{2}\s*)*)(?:\(Repeated\s+(\d+)\s+times\))?\s*$'
$sameDataRe = '\(Repeated\s+\d+\s+times\s+-\s+same data\)'
$frames = New-Object System.Collections.ArrayList
$skippedProgress = 0
foreach($ln in $lines){
    if($ln -match $sameDataRe){ $skippedProgress++; continue }
    if($ln -notmatch $lineRe){ continue }
    $id=$Matches[3]; $hUs=[uint64]$Matches[2]; $ds=$Matches[4]; $repField=$Matches[5]
    if(-not $wantIds.Contains($id)){ continue }
    $b=@()
    if($ds.Trim().Length -gt 0){ $b=@($ds.Trim() -split '\s+' | Where-Object {$_ -ne ""} | ForEach-Object { try{[Convert]::ToByte($_,16)}catch{0} }) }
    $ef = "0x" + $id.Substring(3)
    $rep = 1
    if($repField){ $rep = [math]::Max(0,[int]$repField - 1) }
    for($k=0;$k -lt $rep;$k++){ [void]$frames.Add([pscustomobject]@{ Ef=$ef; Us=($hUs+$k); B=$b }) }
}
$sorted = @($frames | Sort-Object Us)
$n = $sorted.Count
if(-not $OutputJson){
    Write-Output ("LOG: " + $Log + "  fw112_ab frames=" + $n + " (skipped " + $skippedProgress + " progress marker(s))")
}

# ---- 2. split the stream into records: EACH header's 4 data fragments are matched by NEAREST
#         TIMESTAMP among not-yet-claimed frames of that EFID (same technique
#         tools/decode_fw112_diag.ps1 already uses), not by "the next 4 frames in the stream".
#         With many episodes' records interleaved in one dump (this project routinely has 20+
#         open episodes), the plain "next frame of each type" approach silently pairs a header
#         with a LATER record's fragment whenever another episode's frame of the same EFID
#         lands in between - every one of those records then reports as INCOMPLETE even though
#         its real fragments are present a little further down the stream. Nearest-timestamp
#         matching is robust to that interleaving because main.c serializes one record's 5
#         frames back-to-back (header, frag1..frag4) before moving to the next queued record, so
#         a record's own fragments are always its header's closest unclaimed candidates. ----
$headers = @($sorted | Where-Object { $_.Ef -eq "0x1022F" })
$fragPool = @{}
foreach($fid in @("0x10230","0x10231","0x10232","0x10233")){
    $fragPool[$fid] = @($sorted | Where-Object { $_.Ef -eq $fid })
}
$usedFrag = New-Object 'System.Collections.Generic.HashSet[string]'
$records = @()
foreach($h in $headers){
    $t0 = $h.Us
    $frag = @{}
    foreach($fid in @("0x10230","0x10231","0x10232","0x10233")){
        $best = $null; $bestD = [uint64]::MaxValue; $bestIdx = -1
        $pool = $fragPool[$fid]
        for($i=0;$i -lt $pool.Count;$i++){
            $key = "${fid}:$i"
            if($usedFrag.Contains($key)){ continue }
            $f = $pool[$i]
            $d = if($f.Us -ge $t0){ $f.Us - $t0 } else { [uint64]($t0 - $f.Us) }
            if($d -lt $bestD){ $bestD = $d; $best = $f; $bestIdx = $i }
        }
        if($null -ne $best){ [void]$usedFrag.Add("${fid}:$bestIdx"); $frag[$fid] = $best }
    }
    $records += [pscustomobject]@{ Hdr=$h; Frag=$frag; Dups=@() }
}
$records | ForEach-Object {
    if($_.Frag.Count -lt 4){
        $r = $_
        $missing = @()
        foreach($want in @("0x10230","0x10231","0x10232","0x10233")){ if(-not $r.Frag.ContainsKey($want)){ $missing += $want } }
        $r | Add-Member -NotePropertyName Missing -NotePropertyValue $missing
    }
}

# ---- 3. decode each record and group into episodes ----
$eps = @{}
foreach($r in $records){
    $h = $r.Hdr.B
    if($h.Count -lt 8){ $h = $h + @(0,0,0,0,0,0,0,0) }
    $schema = $h[0]; $sid = $h[1]; $epid = (ToU16 $h 2); $rtype = $h[4]; $msidx = $h[5]
    $fragsOk = ($r.Frag.Count -eq 4)
    $note = @()
    if($r.Dups.Count -gt 0){ $note += ("duplicate:" + (($r.Dups | ForEach-Object { $_.Substring(2) }) -join ",")) }
    if(-not $fragsOk){ $note += ("missing:" + (($r.Missing | ForEach-Object { $_.Substring(2) }) -join ",")) }

    $obj = [ordered]@{ Schema=$schema; Session=$sid; Episode=$epid; Type=$recTypeName[[int]$rtype]; MsIdx=("0x{0:X2}" -f $msidx); Frag=$fragsOk; Note=($note -join "; ") }
    # Schema handling: 1 is the pre-diagnostic wire format (frag3[7]/frag4[2:3] are genuinely
    # always 0, never written - decoding them as afilt/arun would silently show a false "0"
    # reading instead of UNKNOWN). 2 adds real afilt/arun in those same bytes. Anything else is
    # unrecognized and flagged, but still decoded on the schema-2 assumption (forward-compatible
    # best effort) since the record layout itself has not moved.
    $hasDirectSignals = ($schema -ge 2)
    if($schema -lt 1 -or $schema -gt 2){
        $obj["Warn"] = "SCHEMA $schema not recognized (decoder knows 1 and 2) - decoding as schema 2 best-effort"
    }
    if($rtype -eq 0){
        if($fragsOk){
            $f1=$r.Frag["0x10230"].B; $f2=$r.Frag["0x10231"].B; $f3=$r.Frag["0x10232"].B; $f4=$r.Frag["0x10233"].B
            $obj["Config"] = [ordered]@{
                AssistLevel=$f1[0]; Bank=$f1[1]; Mode=$modeName[[int]$f1[2]]; EmtbParam=$f1[3]
                SupportRatioPct=(ToU16 $f1 4); MaxIqPct=$f1[6]; Flags=(Bits $f1[7] $cfgFlagName)
                EmtbRefVoltageMv=(ToU16 $f2 1); MaxMotorPowerW=(ToU16 $f2 3); CtlTempC=(ToS16 $f2 5)
                BatteryVx10=(ToU16 $f3 0); LoadThresholdCkg=(ToU16 $f3 2); RequiredSteps=$f3[4]
                StartSteps=$f3[5]; CadenceAtArm=$f3[6]; ArmTick=("0x{0:X8}" -f (ToU32 $f4 0))
            }
        }
        $obj["Milestone"] = "REVOKED"
    } else {
        $ms = $msidx -band 0x07; $idx = ($msidx -shr 3)
        $obj["Milestone"] = $msName[[int]$ms]; $obj["SampleIdx"] = $idx
        if($fragsOk){
            $f1=$r.Frag["0x10230"].B; $f2=$r.Frag["0x10231"].B; $f3=$r.Frag["0x10232"].B; $f4=$r.Frag["0x10233"].B
            $recov = $f2[1]
            $pathClass = if($recov -eq 0){ "ORDINARY_RUN" } else { "REARM" }
            $sample = [ordered]@{
                AssistLevel=$f1[0]; TickOffset=(ToS16 $f1 1); TorqueMv=(ToU16 $f1 3); LoadCkg=(ToU16 $f1 5)
                Cadence=$f1[7]; SessionState=$sessionName[[int]$f2[0]]; Recovery=$recovName[[int]$recov]
                Dir=$dirName[[int]$f2[2]]; Flags=(Bits $f2[3] $flagName); AssistHold=$f2[4]
                IqRequest=(ToS16 $f2 5); IqAfterLatch=(ToS16 $f3 0); IqPreRamp=(ToS16 $f3 2)
                IqSetpoint=(ToS16 $f3 4); CtlTempC=(ToS8 $f3 6); BatteryVx10=(ToU16 $f4 0)
                PathClass=$pathClass
            }
            if($hasDirectSignals){
                $sample["ArunNative"] = $f3[7]           # 0..255, saturating clamp - see header comment
                $sample["AfiltNative"] = (ToU16 $f4 2)   # full 16-bit precision
            } else {
                $sample["ArunNative"] = "UNKNOWN(schema1)"
                $sample["AfiltNative"] = "UNKNOWN(schema1)"
            }
            $obj["Sample"] = $sample
        }
    }
    $epKey = "${sid}:${epid}"
    if(-not $eps.ContainsKey($epKey)){ $eps[$epKey] = @() }
    $eps[$epKey] += $obj
}

# ---- 4. report per episode ----
$order = @($eps.Keys | Sort-Object)
if($OutputJson){
    $machineRecords = @()
    foreach($epKey in $order){ $machineRecords += @($eps[$epKey]) }
    ConvertTo-Json -InputObject @($machineRecords) -Depth 8 -Compress
    return
}
foreach($epKey in $order){
    $recs = $eps[$epKey]
    $parts = $epKey -split ':'
    $ep = [int]$parts[1]; $epSid = [int]$parts[0]
    $cfg = $recs | Where-Object { $_.Type -eq "CONFIG" } | Select-Object -First 1
    $granted = $recs | Where-Object { $_.Milestone -eq "GRANTED" } | Select-Object -First 1
    $setpoint = $recs | Where-Object { $_.Milestone -eq "SETPOINT_POSITIVE" } | Select-Object -First 1
    $incomplete = @($recs | Where-Object { -not $_.Frag })
    $last = $recs[-1]
    $epSchema = $recs[0].Schema
    Write-Output ("=== EPISODE {0}  session {1}  schema {2}  records {3} ===" -f $ep, $epSid, $epSchema, $recs.Count)
    if($cfg){
        $c = $cfg.Config
        Write-Output ("  CONFIG: level={0} bank={1} mode={2} emtb={3} support={4}% max_iq={5} flags={6}" -f $c.AssistLevel,$c.Bank,$c.Mode,$c.EmtbParam,$c.SupportRatioPct,$c.MaxIqPct,$c.Flags)
        Write-Output ("          refV={0}mV maxP={1}W ctlT={2}C batt={3}.{4}V thr={5}ckg req_steps={6} start_steps={7} cad@arm={8} arm_tick={9}" -f $c.EmtbRefVoltageMv,$c.MaxMotorPowerW,$c.CtlTempC,[int]($c.BatteryVx10/10),($c.BatteryVx10%10),$c.LoadThresholdCkg,$c.RequiredSteps,$c.StartSteps,$c.CadenceAtArm,$c.ArmTick)
    } else {
        Write-Output "  CONFIG: MISSING (no CONFIG record for this episode)"
    }
    if($granted){ Write-Output ("  GRANTED anchor: offset {0} (grant reference)" -f $granted.Sample.TickOffset) }

    foreach($r in $recs){
        $line = "  [{0}] {1} ms_idx={2} frag={3}" -f $r.Type,$r.Milestone,$r.MsIdx,$r.Frag
        if($r.Type -eq "SAMPLE" -and $r.Frag){
            $s = $r.Sample
            $line += (" off={0} ({1}) ses={2} recov={3} path={4} dir={5} flags={6} hold={7} tq={8}mV(proxy) load={9}ckg cad={10}" -f `
                $s.TickOffset,(TicksMs $s.TickOffset),$s.SessionState,$s.Recovery,$s.PathClass,$s.Dir,$s.Flags,$s.AssistHold,$s.TorqueMv,$s.LoadCkg,$s.Cadence)
            $line += (" | AFILT={0} ARUN={1} | iq_req={2} iq_latch={3} iq_pre={4} iq_set={5} ctlT={6}C batt={7}.{8}V" -f `
                $s.AfiltNative,$s.ArunNative,$s.IqRequest,$s.IqAfterLatch,$s.IqPreRamp,$s.IqSetpoint,$s.CtlTempC,[int]($s.BatteryVx10/10),($s.BatteryVx10%10))
        }
        if($r.Note){ $line += "  <" + $r.Note + ">" }
        if($r.Warn){ $line += "  <" + $r.Warn + ">" }
        Write-Output $line
    }

    # ---- close reason + D deltas (same derivation the card specifies) ----
    if($incomplete.Count -gt 0){
        Write-Output ("  CLOSE: INCOMPLETE CAPTURE - {0} record(s) malformed (missing/duplicate frames)" -f $incomplete.Count)
    } elseif($null -ne $setpoint){
        Write-Output ("  CLOSE: SETPOINT_POSITIVE at offset {0} ({1})" -f $setpoint.Sample.TickOffset,(TicksMs $setpoint.Sample.TickOffset))
    } elseif($null -ne $granted){
        $offs = @($recs | Where-Object { $_.Frag } | ForEach-Object { $_.Sample.TickOffset } | Where-Object { $_ -ge 0 })
        if($offs.Count -gt 0){
            Write-Output ("  CLOSE: WINDOW/TIMEOUT - last sample offset {0} ({1}), no positive setpoint within FW112_AB_WINDOW_TICKS=8000" -f ($offs | Measure-Object -Maximum).Maximum, (TicksMs ($offs | Measure-Object -Maximum).Maximum))
        } else {
            Write-Output "  CLOSE: WINDOW/TIMEOUT - granted but no sample offsets, no positive setpoint"
        }
    } else {
        Write-Output ("  CLOSE: WINDOW/TIMEOUT - never granted, last offset {0} ({1}) open-relative" -f $last.Sample.TickOffset,(TicksMs $last.Sample.TickOffset))
    }

    # D1..D5 deltas relative to the GRANTED anchor, exactly as inc/fw112_ab.h defines them.
    if($granted){
        $gOff = $granted.Sample.TickOffset
        $dq = @{}
        foreach($r in $recs){ if($r.Type -eq "SAMPLE" -and $r.Frag){ $dq[$r.Milestone] = $r.Sample.TickOffset } }
        $dPerm = $gOff                                # D1 permission (GRANTED offset relative to open)
        $torq = $dq["TORQUE_POSITIVE"]; $mode = $dq["MODE_DEMAND_POSITIVE"]; $iqr = $dq["IQ_REQUEST_POSITIVE"]
        $pre = $dq["PRE_RAMP_POSITIVE"]; $set = $dq["SETPOINT_POSITIVE"]
        Write-Output ("  D1(permission)= {0}   torque={1} mode_demand={2} iq_request={3} pre_ramp={4} setpoint={5}" -f $dPerm,$torq,$mode,$iqr,$pre,$set)
    }

    # ---- FW-112 TWO-MECHANISM DIAGNOSTIC: per-episode path summary from the direct signals ----
    $sampled = @($recs | Where-Object { $_.Type -eq "SAMPLE" -and $_.Frag })
    if($sampled.Count -gt 0){
        $rearmSamples = @($sampled | Where-Object { $_.Sample.PathClass -eq "REARM" })
        $runSamples = @($sampled | Where-Object { $_.Sample.PathClass -eq "ORDINARY_RUN" })
        Write-Output ("  PATH SUMMARY: {0} sample(s) REARM (recovery active), {1} sample(s) ORDINARY_RUN (recovery idle)" -f $rearmSamples.Count, $runSamples.Count)
    }
    Write-Output ""
}
