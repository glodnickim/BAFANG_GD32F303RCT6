param(
    [Parameter(Mandatory=$true)][string]$Log,
    [switch]$AllTransfers,
    [switch]$OutputJson
)
# FW-126.1: phase-current calibration dump (0x602D) decoder - reads an EXISTING raw log.
#
# It sends nothing. The request is made by hand from the sniffer's Custom Frame
# (ID 0511602D, extended, DLC 0); this script only reassembles the reply already in the log.
#
# REPLY FRAMING - src/can_multiframe.c build_efid()/build_current_fragment(), with the reply's
# target/source fixed by src/CAN_Display.c send_multiframe_tracked(): target = requester (5),
# source = controller (2).
#
#   efid = command | (operation << 16) | (target << 19) | (source << 24)
#
#   START  0x022C602D  DLC 1   data[0] = TOTAL PAYLOAD LENGTH (55 for this dump)
#   DATA   0x022D000n  DLC 8   the low 16 bits are the FRAGMENT INDEX n, 0..nbrofframes-1
#   END    0x022E000N  DLC r   the low 16 bits are nbrofframes N; r = length % 8, or 8 when 0
#
#   nbrofframes = (length % 8) ? length >> 3 : (length >> 3) - 1     <- can_multiframe.c:183
#   For length 55: N = 6, so DATA 0..5 carry 48 bytes and END carries the last 7.
#
# The sniffer prefixes the SOURCE byte with 0x80 (bafang-parser.js CAN_CHANNEL_PREFIX), so the
# same three frames appear in a log as 822C602D / 822D000n / 822E000N. Both forms are accepted.
#
# FRAGMENTS ARE PLACED BY INDEX, NOT BY ARRIVAL ORDER. The index is in the id, so a reordered or
# duplicated frame cannot silently shift the payload; a missing one is reported rather than
# closed over with zeros.
#
# PAYLOAD LAYOUT - src/CAN_Display.c current_cal_serialize_dump(), 55 B, little-endian.
# Read off that function, not from prose. Signedness matters: the offsets and residual means are
# int16 cast through uint16 on the wire, and reading them unsigned turns -3 into 65533.
#
#   0..2   magic 'C','C',1
#   3      flags  bit0 valid   bit1 fallback/LKG (source != RUNTIME)
#                 bit2 self-test valid   bit3 MOE-off verified
#   4      status          (current_cal_status_t)
#   5      failure_reason  (same enum)
#   6      source          (current_cal_source_t)
#   7      attempts
#   8..9   last_sample_count           u16   <- must be 128
#   10..11 self-test samples           u16
#   12     self-test saw MOE ON        u8    <- must be 0
#   13     trigger mode                u8    (1 = software-triggered, main.c:653)
#   14..17 conversions issued          i32
#   18..21 conversions confirmed fresh i32
#   22..27 offset A/B/C                i16 x3
#   28..33 residual mean A/B/C         i16 x3
#   34..39 P2P A/B/C                   u16 x3
#   40..45 self-test mean A/B/C        i16 x3
#   46..51 self-test P2P A/B/C         u16 x3
#   52     self-test status
#   53..54 CRC16-CCITT (LE) over bytes 0..52
$ErrorActionPreference = "Stop"
trap { Write-Output "SCRIPT ERR line $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message) :: $($_.InvocationInfo.Line.Trim())"; break }

$DUMP_LEN = 66      # FW-126.7 calibration report

# current_cal_status_t - inc/current_cal.h:66. failure_reason uses the SAME enum.
$statusName = @{
    0="UNCALIBRATED"; 1="OK"; 2="OUT_OF_RANGE"; 3="TOO_NOISY"; 4="SAMPLE_TIMEOUT"
    5="USING_LKG"; 6="LEGACY_FALLBACK"; 7="HARD_FAILED"; 8="MOE_ON"
}
# current_cal_source_t - inc/current_cal.h:79
$sourceName = @{ 0="NONE (FOC inhibited)"; 1="RUNTIME"; 2="LKG"; 3="LEGACY (hardware offset only)" }

function ToU16LE($d,$o){ return (([int]$d[$o+1] -shl 8) -bor [int]$d[$o]) -band 0xFFFF }
function ToI16LE($d,$o){ $v = ToU16LE $d $o; if($v -ge 0x8000){ $v -= 0x10000 }; return $v }
function ToI32LE($d,$o){
    $v = ([uint32]$d[$o]) -bor ([uint32]$d[$o+1] -shl 8) -bor ([uint32]$d[$o+2] -shl 16) -bor ([uint32]$d[$o+3] -shl 24)
    # NOT `-ge 0x80000000`: in Windows PowerShell 5.1 that literal is Int32 -2147483648, so the
    # test would be true for EVERY value and every count would come back negative. The sign
    # threshold is written as a decimal [uint32] for that reason - do not "tidy" it back to hex.
    if($v -ge [uint32]2147483648){ return [int64]$v - 4294967296 } else { return [int64]$v }
}
function Crc16Ccitt($d,$len){
    $c = 0xFFFF
    for($i=0;$i -lt $len;$i++){
        $c = $c -bxor ([int]$d[$i] -shl 8)
        for($b=0;$b -lt 8;$b++){
            if($c -band 0x8000){ $c = (($c -shl 1) -bxor 0x1021) -band 0xFFFF } else { $c = ($c -shl 1) -band 0xFFFF }
        }
    }
    return $c
}

# ---- 1. pull every candidate frame out of the log -------------------------------------------
# Same two "(Repeated ...)" markers as the other decoders: the "- same data" form is a progress
# marker and is NOT a frame; the plain form is a final summary whose N counts the already-logged
# first line. A one-shot reply should produce neither, so seeing one is worth reporting.
$lines = Get-Content -LiteralPath $Log
$lineRe     = '^\[(\d{2}:\d{2}:\d{2})\]\s+\[INFO\]\s+(\d+)\s+ID:([0-9A-Fa-f]+)\s+DLC:(\d+)\s+Data:((?:[0-9A-Fa-f]{2}\s*)*)(?:\(Repeated\s+(\d+)\s+times\))?\s*$'
$sameDataRe = '\(Repeated\s+\d+\s+times\s+-\s+same data\)'

$frames = New-Object System.Collections.ArrayList
$markerSeen = $false
foreach($ln in $lines){
    if($ln -match $sameDataRe){ continue }
    if($ln -notmatch $lineRe){ continue }
    # Read every group before any further -match: a later test overwrites $Matches.
    $id=$Matches[3].ToUpper(); $us=[uint64]$Matches[2]; $dlc=[int]$Matches[4]; $ds=$Matches[5]; $rep=$Matches[6]
    # Strip the sniffer's 0x80 source-byte prefix so 822C602D and 022C602D are one thing.
    $idv = [Convert]::ToUInt32($id,16) -band 0x7FFFFFFF
    $op  = ($idv -shr 16) -band 0x07
    $tgt = ($idv -shr 19) -band 0x1F
    $src = ($idv -shr 24) -band 0x1F
    if($src -ne 2 -or $tgt -ne 5){ continue }          # controller -> Canable only
    if($op -notin @(4,5,6)){ continue }                # START / DATA / END only
    $cmd = $idv -band 0xFFFF
    # FW-126.7: 0x602E is gone from the firmware - the campaign it carried answered its
    # question (see documentation/FW-126.5 and FW-126.6). Only 0x602D is decoded now.
    if($op -eq 4 -and $cmd -ne 0x602D){ continue }  # a START for another command
    if($null -eq $ds){ $ds="" }
    $b=@()
    if($ds.Trim().Length -gt 0){ $b=@($ds.Trim() -split '\s+' | Where-Object {$_ -ne ""} | ForEach-Object { try{[Convert]::ToByte($_,16)}catch{0} }) }
    if($rep){ $markerSeen = $true }
    [void]$frames.Add([pscustomobject]@{ Us=$us; Op=$op; Cmd=$cmd; Dlc=$dlc; B=$b; Id=$id })
}
$frames = @($frames | Sort-Object Us)

if(-not $OutputJson){
    Write-Output ("LOG: " + $Log)
    Write-Output ("  candidate reply frames: " + $frames.Count +
                  "  (START=" + @($frames | Where-Object {$_.Op -eq 4}).Count +
                  " DATA=" + @($frames | Where-Object {$_.Op -eq 5}).Count +
                  " END=" + @($frames | Where-Object {$_.Op -eq 6}).Count + ")")
    if($markerSeen){ Write-Output "  NOTE: a repeat marker appeared on these ids - a one-shot reply should not repeat." }
}
if(@($frames | Where-Object {$_.Op -eq 4}).Count -eq 0){
    Write-Output ""
    Write-Output "VERDICT: NO DATA - no 0x602D reply START (022C602D / 822C602D ...) in this log."
    Write-Output "  The controller answers only when asked. Send the Custom Frame ID 0511602D,"
    Write-Output "  extended, DLC 0, while the DIAG image (0.0429) is running, and log the reply."
    return
}

# ---- 2. cut the frame stream into transfers, one per START ----------------------------------
# A DATA/END frame belongs to the START that precedes it: the fragment id carries only an index,
# never the command, so nothing else ties it to a transfer.
$transfers = New-Object System.Collections.ArrayList
$cur = $null
foreach($f in $frames){
    if($f.Op -eq 4){
        if($null -ne $cur){ [void]$transfers.Add($cur) }
        $cur = [pscustomobject]@{ Start=$f; Data=@{}; End=$null }
    } elseif($null -ne $cur){
        if($f.Op -eq 5){ if(-not $cur.Data.ContainsKey([int]$f.Cmd)){ $cur.Data[[int]$f.Cmd] = $f } }
        elseif($f.Op -eq 6 -and $null -eq $cur.End){ $cur.End = $f }
    }
}
if($null -ne $cur){ [void]$transfers.Add($cur) }

function Get-Payload($t){
    $r = [ordered]@{ ok=$false; error=""; declared_len=$null; frag_expected=$null; bytes=@() }
    if($t.Start.B.Count -lt 1){ $r.error = "START frame carries no length byte"; return $r }
    $len = [int]$t.Start.B[0]
    $r.declared_len = $len
    # Only one payload layout exists now, so a START declaring any other length is refused
    # rather than assembled into something this decoder would then misread.
    if($len -ne $DUMP_LEN){ $r.error = "declared length $len, expected $DUMP_LEN"; return $r }
    # can_multiframe.c:183 - an exact multiple of 8 gets one FEWER data fragment, because END
    # absorbs that last full chunk.
    $n = if($len % 8){ [math]::Floor($len / 8) } else { [math]::Floor($len / 8) - 1 }
    $r.frag_expected = $n
    if($null -eq $t.End){ $r.error = "no END frame - transfer truncated"; return $r }
    if([int]$t.End.Cmd -ne $n){ $r.error = ("END says nbrofframes=" + [int]$t.End.Cmd + ", the declared length needs " + $n); return $r }
    $missing = @(0..([math]::Max(0,$n-1)) | Where-Object { $n -gt 0 -and -not $t.Data.ContainsKey($_) })
    if($n -gt 0 -and $missing.Count -gt 0){ $r.error = ("missing DATA fragment(s): " + ($missing -join ', ')); return $r }
    $buf = New-Object 'System.Collections.Generic.List[byte]'
    for($i=0;$i -lt $n;$i++){
        $fb = $t.Data[$i].B
        if($fb.Count -lt 8){ $r.error = "DATA fragment $i has only $($fb.Count) byte(s), expected 8"; return $r }
        for($k=0;$k -lt 8;$k++){ $buf.Add([byte]$fb[$k]) }
    }
    $rem = if($len % 8){ $len % 8 } else { 8 }
    if($t.End.B.Count -lt $rem){ $r.error = "END frame has $($t.End.B.Count) byte(s), expected $rem"; return $r }
    for($k=0;$k -lt $rem;$k++){ $buf.Add([byte]$t.End.B[$k]) }
    if($buf.Count -ne $len){ $r.error = "assembled $($buf.Count) bytes, declared $len"; return $r }
    $r.bytes = $buf.ToArray()
    $r.ok = $true
    return $r
}

$decoded = @()
$idx = 0
foreach($t in $transfers){
    $idx++
    $p = Get-Payload $t
    $decoded += [pscustomobject]@{ n=$idx; payload=$p }
}
$good = @($decoded | Where-Object { $_.payload.ok })
if(-not $OutputJson){
    Write-Output ("  transfers: " + $decoded.Count + " started, " + $good.Count + " fully reassembled")
    foreach($d in $decoded){ if(-not $d.payload.ok){ Write-Output ("    transfer #" + $d.n + " REJECTED: " + $d.payload.error) } }
}
if($good.Count -eq 0){
    Write-Output ""
    Write-Output "VERDICT: INCOMPLETE - no 0x602D transfer reassembled cleanly. Refusing to decode a partial payload."
    return
}
# One payload kind again (FW-126.7 retired 0x602E), so "the newest transfer" is once more
# the right unit. -AllTransfers shows every one.
if($AllTransfers){ $toShow = $good } else { $toShow = @($good[-1]) }

# ---- 3. decode the payload ------------------------------------------------------------------
# FW-126.7 calibration report (0x602D). THE LAYOUT IS NOT WRITTEN HERE: it is read from
# protocol/fw1267_cal_schema.json, the same file ui/js/evistdrive/fw126-decode.js mirrors and
# tests/fw1267_cal_parity.js pins. Two decoders that each hand-wrote the layout drifted once
# before, and one of them reported a verdict opposite to the truth. Never again by construction.
$script:Fw1267Schema = $null
function Get-Fw1267Schema {
    if($null -ne $script:Fw1267Schema){ return $script:Fw1267Schema }
    $p = Join-Path (Split-Path -Parent $PSCommandPath) "..\protocol\fw1267_cal_schema.json"
    if(-not (Test-Path $p)){
        throw "FW-126.7 schema not found at $p - the decoder refuses to guess a layout"
    }
    $script:Fw1267Schema = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json
    return $script:Fw1267Schema
}

function Read-Fw1267Field($d, $fld){
    switch($fld.type){
        "u8"  { return [int]$d[$fld.offset] }
        "u16" { return (ToU16LE $d $fld.offset) }
        "i16" { return (ToI16LE $d $fld.offset) }
        default { throw "unknown field type $($fld.type)" }
    }
}

function Get-CalDump($d){
    $sch = Get-Fw1267Schema
    $crcCalc = Crc16Ccitt $d ([int]$sch.crc.over)
    $crcWire = ToU16LE $d ([int]$sch.crc.offset)
    $r = [ordered]@{
        magic_ok = ($d[0] -eq [int]$sch.magic[0] -and $d[1] -eq [int]$sch.magic[1])
        schema   = [int]$d[2]
        crc_ok   = ($crcCalc -eq $crcWire)
        crc_wire = ("0x{0:X4}" -f $crcWire)
        crc_calc = ("0x{0:X4}" -f $crcCalc)
        hard_fail = $false
    }
    # Unknown magic or a schema this decoder does not implement is a HARD FAIL: no fields are
    # read, so there is nothing to be confident about.
    if(-not $r.magic_ok){
        $r.hard_fail = $true; $r.verdict = "REFUSED"
        $r.reason = "payload magic is not 'CC' - this is not a calibration report"
        return $r
    }
    if($r.schema -ne [int]$sch.schema){
        $r.hard_fail = $true; $r.verdict = "REFUSED"
        $r.reason = "schema $($r.schema) is not the one this decoder implements ($([int]$sch.schema)) - decoding it with these offsets would produce confident nonsense"
        return $r
    }

    $f = @{}
    foreach($fld in $sch.fields){ $f[$fld.name] = Read-Fw1267Field $d $fld }
    $r.fields = $f
    $fl = $f["flags"]
    $r.state          = $f["state"]
    $r.state_name     = $sch.state_names."$($f['state'])"
    $r.failure        = $f["failure_reason"]
    $r.failure_name   = $sch.failure_names."$($f['failure_reason'])"
    $r.source         = $f["source"]
    $r.source_name    = $sch.source_names."$($f['source'])"
    $r.attempts       = $f["attempts"]
    $r.flags_raw      = ("0x{0:X2}" -f $fl)
    $r.valid          = (($fl -band 0x01) -ne 0)
    $r.timeout_hit    = (($fl -band 0x02) -ne 0)
    $r.neutral_ok     = (($fl -band 0x04) -ne 0)
    $r.foc_blocked    = (($fl -band 0x08) -ne 0)
    $r.diag_stop      = (($fl -band 0x10) -ne 0)
    $r.cycles         = $f["cycles"]
    $r.stable_count   = $f["stable_count"]
    $r.eligible       = $f["eligible"]
    $r.restarts       = $f["restarts"]

    $ph = @($sch.phases)
    $r.offset   = @($ph | ForEach-Object { $f["offset_$_"] })
    $r.mean     = @($ph | ForEach-Object { $f["mean_$_"] })
    $r.min      = @($ph | ForEach-Object { $f["min_$_"] })
    $r.max      = @($ph | ForEach-Object { $f["max_$_"] })
    $r.p2p      = @($ph | ForEach-Object { $f["p2p_$_"] })
    $r.ioff     = @($ph | ForEach-Object { $f["ioff_$_"] })
    $r.midpoint = @($ph | ForEach-Object { $f["midpoint_$_"] })
    $r.gate = [ordered]@{
        stable_cycles   = $f["gate_stable_cycles"]
        collect_samples = $f["gate_collect_samples"]
        max_cycles      = $f["gate_max_cycles"]
        residual_window = $f["gate_residual_window"]
    }
    return $r
}

function Get-CalChecks($r){
    if($r.hard_fail){ return @() }
    $w = [int]$r.gate.residual_window
    $c = @()
    $c += [pscustomobject]@{ name="CRC";              pass=$r.crc_ok;        detail=($r.crc_wire + " / " + $r.crc_calc) }
    $c += [pscustomobject]@{ name="state";            pass=($r.state -eq 3); detail=$r.state_name }
    $c += [pscustomobject]@{ name="source";           pass=($r.source -eq 1);detail=($r.source_name + " (the only valid source)") }
    $c += [pscustomobject]@{ name="valid flag";       pass=$r.valid;         detail=$r.flags_raw }
    $c += [pscustomobject]@{ name="bridge neutral";   pass=$r.neutral_ok;    detail="MOE on + compares neutral, every sample" }
    $c += [pscustomobject]@{ name="FOC blocked";      pass=$r.foc_blocked;   detail="structural: sampled from the dwell branch" }
    $c += [pscustomobject]@{ name="no timeout";       pass=(-not $r.timeout_hit); detail=("" + $r.cycles + " of " + $r.gate.max_cycles + " cycles") }
    $c += [pscustomobject]@{ name="eligible samples"; pass=($r.eligible -ge $r.gate.collect_samples); detail=("" + $r.eligible + " of " + $r.gate.collect_samples + " required") }
    $c += [pscustomobject]@{ name="residual window";  pass=((@($r.mean | Where-Object { [math]::Abs($_) -gt $w }).Count) -eq 0); detail=("JDR mean " + ($r.mean -join " / ") + ", window +-" + $w) }
    $c += [pscustomobject]@{ name="noise";            pass=((@($r.p2p | Where-Object { $_ -gt 200 }).Count) -eq 0); detail=("JDR P2P " + ($r.p2p -join " / ")) }
    # The check the old calibration could never have failed and should have: a saturated
    # amplifier is quiet and near full scale, so only the PHYSICAL result says whether the
    # measurement was taken in a valid electrical state at all.
    $c += [pscustomobject]@{ name="midpoint sanity";  pass=((@($r.midpoint | Where-Object { [math]::Abs($_ - 2048) -gt 400 }).Count) -eq 0); detail=("physical ADC (IOFF+JDR) " + ($r.midpoint -join " / ") + " - mid-scale is 2048; a saturated sense amplifier would read ~3900") }
    return $c
}


$out = @()
foreach($g in $toShow){
    $r = Get-CalDump $g.payload.bytes
    $out += [pscustomobject]@{ transfer=$g.n; dump=$r; checks=(Get-CalChecks $r) }
}

if($OutputJson){ $out | ConvertTo-Json -Depth 8; return }

foreach($o in $out){
    $r = $o.dump
    Write-Output ""
    Write-Output "=============================================================================="
    Write-Output ("0x602D PHASE-CURRENT CALIBRATION  (FW-126.7, transfer #" + $o.transfer + ")")
    Write-Output ""
    Write-Output ("  magic/CRC     : " + $(if($r.magic_ok){"CC v" + $r.schema}else{"BAD MAGIC"}) +
                  "   CRC " + $r.crc_wire + "/" + $r.crc_calc + " -> " + $(if($r.crc_ok){"OK"}else{"MISMATCH"}))
    if($r.hard_fail){
        Write-Output ("VERDICT: " + $r.verdict + " - " + $r.reason)
        continue
    }
    if(-not $r.crc_ok){ Write-Output "  CRC MISMATCH - every field below is suspect. Do not quote these numbers." }
    Write-Output ("  state         : " + $r.state + " " + $r.state_name +
                  "   source " + $r.source + " " + $r.source_name + "   attempts " + $r.attempts)
    Write-Output ("  failure       : " + $r.failure + " " + $r.failure_name)
    Write-Output ("  flags         : " + $r.flags_raw + "  valid=" + [int]$r.valid +
                  " neutral=" + [int]$r.neutral_ok + " foc_blocked=" + [int]$r.foc_blocked +
                  " timeout=" + [int]$r.timeout_hit + " diag_stop=" + [int]$r.diag_stop)
    Write-Output ("  gate          : " + $r.cycles + " of " + $r.gate.max_cycles + " cycles   stable " +
                  $r.stable_count + "/" + $r.gate.stable_cycles + "   eligible " + $r.eligible + "/" +
                  $r.gate.collect_samples + "   restarts " + $r.restarts)
    Write-Output ""
    # The three domains, as three labelled groups. Reading JDR as "raw ADC" is the mistake that
    # hid a saturated amplifier behind four cards' worth of green checks.
    Write-Output "                          phase A      phase B      phase C"
    Write-Output ("  JDR mean        " + ("{0,10} {1,12} {2,12}" -f $r.mean[0], $r.mean[1], $r.mean[2]))
    Write-Output ("  JDR min         " + ("{0,10} {1,12} {2,12}" -f $r.min[0], $r.min[1], $r.min[2]))
    Write-Output ("  JDR max         " + ("{0,10} {1,12} {2,12}" -f $r.max[0], $r.max[1], $r.max[2]))
    Write-Output ("  JDR P2P         " + ("{0,10} {1,12} {2,12}" -f $r.p2p[0], $r.p2p[1], $r.p2p[2]))
    Write-Output ""
    Write-Output ("  software offset " + ("{0,10} {1,12} {2,12}" -f $r.offset[0], $r.offset[1], $r.offset[2]))
    Write-Output "    ^ what the FOC ISR subtracts from JDR - this IS the calibration result"
    Write-Output ""
    Write-Output ("  hardware IOFF   " + ("{0,10} {1,12} {2,12}" -f $r.ioff[0], $r.ioff[1], $r.ioff[2]))
    Write-Output ("  physical ADC    " + ("{0,10} {1,12} {2,12}" -f $r.midpoint[0], $r.midpoint[1], $r.midpoint[2]))
    Write-Output "    ^ IOFF + JDR mean. Mid-scale is 2048; a SATURATED sense amplifier reads"
    Write-Output "      ~3900, which is exactly what every calibration before FW-126.7 measured."
    Write-Output ""
    Write-Output "PASS CRITERIA:"
    foreach($c in $o.checks){
        Write-Output ("  [" + $(if($c.pass){"PASS"}else{"FAIL"}) + "] " + $c.name.PadRight(20) + $c.detail)
    }
    $failed = @($o.checks | Where-Object { -not $_.pass })
    Write-Output ""
    if($failed.Count -eq 0){
        Write-Output "VERDICT: PASS - calibrated in a valid electrical state."
    } else {
        Write-Output ("VERDICT: FAIL - " + $failed.Count + " criterion/criteria not met: " + (($failed | ForEach-Object { $_.name }) -join '; '))
    }
    if($r.diag_stop){
        Write-Output ""
        Write-Output "NOTE: the DIAG post-validation stop was spent - this boot deliberately refused the"
        Write-Output "      FOC release once, so no torque followed the calibration."
    }
}
