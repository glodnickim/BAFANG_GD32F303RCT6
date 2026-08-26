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

$DUMP_LEN = 55
$REQUIRED_SAMPLES = 128     # FW-126 hardware test, step 4

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
    # FW-126.3: 0x602E carries the TIMER-triggered probe in the SAME 55-byte framing, so the
    # reassembly below serves both and the magic decides which layout the payload is.
    if($op -eq 4 -and $cmd -ne 0x602D -and $cmd -ne 0x602E){ continue }  # a START for another command
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
    Write-Output "VERDICT: NO DATA - no 0x602D/0x602E reply START (022C602D / 822C602E ...) in this log."
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
    if($len -ne $DUMP_LEN){ $r.error = "declared length $len, expected $DUMP_LEN for a 0x602D dump"; return $r }
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
# "The last transfer" is the wrong unit when a log can hold BOTH replies: picking one dropped
# the 0x602E probe on the floor whenever a 0x602D dump happened to come after it, and said
# nothing about having done so. Select the newest of EACH KIND instead, decided by the payload
# magic - the same authority the display below uses.
if($AllTransfers){
    $toShow = $good
} else {
    $lastProbe = @($good | Where-Object { $_.payload.bytes[0] -eq 0x43 -and $_.payload.bytes[1] -eq 0x50 })
    $lastCal   = @($good | Where-Object { -not ($_.payload.bytes[0] -eq 0x43 -and $_.payload.bytes[1] -eq 0x50) })
    $toShow = @()
    if($lastProbe.Count){ $toShow += $lastProbe[-1] }
    if($lastCal.Count){   $toShow += $lastCal[-1] }
}

# ---- 3. decode the payload ------------------------------------------------------------------
function Get-CalDump($d){
    $r = [ordered]@{}
    $r.magic_ok    = ($d[0] -eq 0x43 -and $d[1] -eq 0x43 -and $d[2] -eq 1)
    $crcCalc = Crc16Ccitt $d 53
    $crcWire = ToU16LE $d 53
    $r.crc_ok      = ($crcCalc -eq $crcWire)
    $r.crc_wire    = ("0x{0:X4}" -f $crcWire)
    $r.crc_calc    = ("0x{0:X4}" -f $crcCalc)
    $fl = [int]$d[3]
    $r.flags_raw   = ("0x{0:X2}" -f $fl)
    $r.valid       = (($fl -band 0x01) -ne 0)
    $r.fallback    = (($fl -band 0x02) -ne 0)
    $r.selftest_valid = (($fl -band 0x04) -ne 0)
    $r.moe_off_verified = (($fl -band 0x08) -ne 0)
    $r.status      = [int]$d[4]; $r.status_name = $(if($statusName.ContainsKey([int]$d[4])){$statusName[[int]$d[4]]}else{"?"})
    $r.failure_reason = [int]$d[5]; $r.failure_name = $(if($statusName.ContainsKey([int]$d[5])){$statusName[[int]$d[5]]}else{"?"})
    $r.source      = [int]$d[6]; $r.source_name = $(if($sourceName.ContainsKey([int]$d[6])){$sourceName[[int]$d[6]]}else{"?"})
    $r.attempts    = [int]$d[7]
    $r.last_sample_count = ToU16LE $d 8
    $r.selftest_samples  = ToU16LE $d 10
    $r.selftest_moe_on   = [int]$d[12]
    $r.trigger_mode      = [int]$d[13]
    $r.conversions_issued        = ToI32LE $d 14
    $r.conversions_confirmed_fresh = ToI32LE $d 18
    $r.offset        = @(0,1,2 | ForEach-Object { ToI16LE $d (22 + 2*$_) })
    $r.residual_mean = @(0,1,2 | ForEach-Object { ToI16LE $d (28 + 2*$_) })
    $r.p2p           = @(0,1,2 | ForEach-Object { ToU16LE $d (34 + 2*$_) })
    $r.selftest_mean = @(0,1,2 | ForEach-Object { ToI16LE $d (40 + 2*$_) })
    $r.selftest_p2p  = @(0,1,2 | ForEach-Object { ToU16LE $d (46 + 2*$_) })
    $r.selftest_status = [int]$d[52]
    return $r
}

# FW-126 hardware test, step 4 - the pass criteria, each checked separately so a failure names
# itself instead of collapsing into one "FAIL".
function Get-CalChecks($r){
    $c = @()
    $c += [pscustomobject]@{ name="magic 'CC' v1";              pass=$r.magic_ok;                       detail=$(if($r.magic_ok){"ok"}else{"payload is not a 0x602D dump"}) }
    $c += [pscustomobject]@{ name="CRC16-CCITT over 0..52";     pass=$r.crc_ok;                         detail=("wire " + $r.crc_wire + ", computed " + $r.crc_calc) }
    $c += [pscustomobject]@{ name="last_sample_count = 128";    pass=($r.last_sample_count -eq $REQUIRED_SAMPLES); detail=("" + $r.last_sample_count) }
    $c += [pscustomobject]@{ name="valid = 1";                  pass=$r.valid;                          detail=$(if($r.valid){"offsets are being applied"}else{"ISR is on the legacy hardware-offset path"}) }
    $c += [pscustomobject]@{ name="fallback = 0";               pass=(-not $r.fallback);                detail=("source = " + $r.source_name) }
    $c += [pscustomobject]@{ name="self-test valid = 1";        pass=$r.selftest_valid;                 detail=("" + $r.selftest_samples + " sample(s)") }
    $c += [pscustomobject]@{ name="MOE-off verified = 1";       pass=$r.moe_off_verified;               detail=$(if($r.moe_off_verified){"bridge proven dark during calibration"}else{"NOT proven - the dump cannot close FW-126"}) }
    $c += [pscustomobject]@{ name="self-test saw MOE ON = 0";   pass=($r.selftest_moe_on -eq 0);        detail=("" + $r.selftest_moe_on) }
    return $c
}

# FW-126.4 probe (0x602E). The LAYOUT IS NOT WRITTEN HERE: it is read from
# protocol/fw1264_probe_schema.json, the same file ui/js/evistdrive/fw126-decode.js mirrors and
# the Canable golden test pins. FW-126.3 kept the layout hand-written in both decoders, they
# drifted, and this script read a schema 2 payload with schema 1 offsets - it printed BAD MAGIC
# and then printed a verdict anyway, the opposite of the truth. Never again by construction.
$script:Fw1264Schema = $null
function Get-Fw1264Schema {
    if($null -ne $script:Fw1264Schema){ return $script:Fw1264Schema }
    $p = Join-Path (Split-Path -Parent $PSCommandPath) "..\protocol\fw1264_probe_schema.json"
    if(-not (Test-Path $p)){
        throw "FW-126.4 schema not found at $p - the decoder refuses to guess a layout"
    }
    $script:Fw1264Schema = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json
    return $script:Fw1264Schema
}

function Read-Fw1264Field($d, $fld){
    switch($fld.type){
        "u8"  { return [int]$d[$fld.offset] }
        "u16" { return (ToU16LE $d $fld.offset) }
        "i16" { return (ToI16LE $d $fld.offset) }
        default { throw "unknown field type $($fld.type)" }
    }
}

function Get-ProbeDump($d){
    $sch = Get-Fw1264Schema
    $crcCalc = Crc16Ccitt $d 53
    $crcWire = ToU16LE $d 53
    $r = [ordered]@{
        magic_ok = ($d[0] -eq 0x43 -and $d[1] -eq 0x50)
        schema   = [int]$d[2]
        crc_ok   = ($crcCalc -eq $crcWire)
        crc_wire = ("0x{0:X4}" -f $crcWire)
        crc_calc = ("0x{0:X4}" -f $crcCalc)
        hard_fail = $false
    }
    if(-not $r.magic_ok){
        $r.hard_fail = $true; $r.verdict = "REFUSED"
        $r.reason = "payload magic is not 'CP' - this is not a probe dump, so no field would mean anything"
        return $r
    }
    if($r.schema -gt [int]$sch.schema){
        $r.hard_fail = $true; $r.verdict = "REFUSED"
        $r.reason = "schema $($r.schema) is newer than this decoder knows ($($sch.schema)) - update the decoder rather than guessing at the layout"
        return $r
    }
    if($r.schema -lt 3){
        $r.legacy = $true; $r.verdict = "SUPERSEDED SCHEMA"
        $r.reason = "schema $($r.schema) predates the FW-126.4 A/B probe; reflash the schema 3 image to get a trigger-parity answer"
        return $r
    }

    $f = @{}
    foreach($fld in $sch.fields){ $f[$fld.name] = Read-Fw1264Field $d $fld }
    $r.fields = $f
    $fl = $f["flags"]
    $r.moe_off       = (($fl -band 0x01) -ne 0)
    $r.poen_off      = (($fl -band 0x02) -ne 0)
    $r.timer_running = (($fl -band 0x04) -ne 0)
    $r.restore_ok    = (($fl -band 0x08) -ne 0)
    $r.done          = (($fl -band 0x10) -ne 0)
    $r.ch3 = $f["ch3"]
    $r.before   = [ordered]@{ adc0 = $f["adc0_src_before"];   adc2 = $f["adc2_src_before"];   trgo = $f["trgo_before"] }
    $r.during   = [ordered]@{ trgo = $f["trgo_during"] }
    $r.restored = [ordered]@{ adc0 = $f["adc0_src_restored"]; adc2 = $f["adc2_src_restored"]; trgo = $f["trgo_restored"] }

    $sw = @(); $tr = @(); $delta = @()
    foreach($p in $sch.phases){
        $sw += [ordered]@{ phase=$p; n=$f["sw_n_$p"]; median=$f["sw_med_$p"]; min=$f["sw_min_$p"]; max=$f["sw_max_$p"]; spread=($f["sw_max_$p"] - $f["sw_min_$p"]) }
        $tr += [ordered]@{ phase=$p; n=$f["trgo_n_$p"]; median=$f["trgo_med_$p"]; span=$f["trgo_span_$p"] }
        $delta += ($f["trgo_med_$p"] - $f["sw_med_$p"])
    }
    $r.sw = $sw; $r.trgo = $tr; $r.delta = $delta
    $r.trgo_events = @($f["trgo_ev_adc0"], $f["trgo_ev_adc1"], $f["trgo_ev_adc2"])

    $ev = $r.trgo_events
    $near = [int]$sch.verdict_thresholds.near_zero_abs_max
    $high = [int]$sch.verdict_thresholds.high_abs_min
    $swHigh = (@($sw | Where-Object { [math]::Abs($_.median) -ge $high }).Count -eq 3)
    if($ev[0] -eq 0 -and $ev[1] -eq 0 -and $ev[2] -eq 0){
        $r.verdict = "C"; $r.reason = "TRGO PARITY NOT ACHIEVED - no ADC produced an event on TIMER0 TRGO either"
    } elseif($ev[0] -ne 0 -and $ev[1] -ne 0 -and $ev[2] -eq 0){
        $r.verdict = "D"; $r.reason = "ADC2 TRIGGER TOPOLOGY NOT EQUIVALENT - the ADC0+ADC1 pair fires on TRGO, ADC2 does not"
    } elseif($ev[0] -eq 0){
        $r.verdict = "C"; $r.reason = "TRGO PARITY NOT ACHIEVED - ADC0, the dual master and reference, stayed silent"
    } elseif(@($tr | Where-Object { $_.n -eq 0 }).Count -gt 0){
        $r.verdict = "E"; $r.reason = "TRGO FIRES, COHERENCY OPEN - events arrived but at least one phase captured no sample, so freshness is not proven for it"
    } elseif((@($tr | Where-Object { [math]::Abs($_.median) -le $near }).Count -eq 3) -and $swHigh){
        $r.verdict = "A"; $r.reason = "TRIGGER/ACQUISITION PATH DIFFERENCE CONFIRMED - software trigger reads high, TIMER0 TRGO reads the runtime-neutral domain, with nothing else changed"
    } elseif(@($delta | Where-Object { [math]::Abs($_) -le $near }).Count -eq 3){
        $r.verdict = "B"; $r.reason = "TRIGGER SOURCE CLEARED - both producers see the same JDR state with MOE off; the runtime ~0 must come from something the bridge enable changes"
    } else {
        $r.verdict = "E"; $r.reason = "TRGO FIRES, COHERENCY OPEN - the deltas match neither classification; report the numbers as they are"
    }
    if(-not $r.restore_ok){ $r.reason += ". WARNING: registers did not read back equal to their snapshot" }
    return $r
}

$out = @()
foreach($g in $toShow){
    $b = $g.payload.bytes
    # Dispatch on the magic, never on which command we think we asked for: a log can hold both.
    if($b[0] -eq 0x43 -and $b[1] -eq 0x50){
        $out += [pscustomobject]@{ transfer=$g.n; kind="probe"; dump=(Get-ProbeDump $b); checks=@() }
    } else {
        $r = Get-CalDump $b
        $out += [pscustomobject]@{ transfer=$g.n; kind="cal"; dump=$r; checks=(Get-CalChecks $r) }
    }
}

if($OutputJson){ $out | ConvertTo-Json -Depth 8; return }

foreach($o in @($out | Where-Object { $_.kind -eq "probe" })){
    $r = $o.dump
    Write-Output ""
    Write-Output "=============================================================================="
    Write-Output ("0x602E TRGO PARITY A/B PROBE  (FW-126.4, transfer #" + $o.transfer + ")")
    Write-Output ""
    Write-Output ("  magic/CRC : " + $(if($r.magic_ok){"CP v" + $r.schema}else{"BAD MAGIC"}) +
                  "   CRC " + $r.crc_wire + "/" + $r.crc_calc + " -> " + $(if($r.crc_ok){"OK"}else{"MISMATCH"}))
    if($r.hard_fail -or $r.legacy){
        # Nothing was decoded, so there is nothing to print and no verdict to give.
        Write-Output ("VERDICT: " + $r.verdict + " - " + $r.reason)
        continue
    }
    if(-not $r.crc_ok){ Write-Output "  CRC MISMATCH - do not quote these numbers." }
    $sch = Get-Fw1264Schema
    $sn = { param($v) $sch.trigger_source_names."$v" }
    $tn = { param($v) $sch.trgo_mode_names."$v" }
    Write-Output ("  conditions: MOE off " + $(if($r.moe_off){"YES"}else{"NO"}) +
                  "   TIMER0 running " + $(if($r.timer_running){"YES"}else{"NO"}) + "   CH3 " + $r.ch3)
    Write-Output ("  BEFORE    : ADC0 trig " + (& $sn $r.before.adc0) + "   ADC2 trig " + (& $sn $r.before.adc2) +
                  "   TIMER0 TRGO " + (& $tn $r.before.trgo))
    Write-Output ("  DURING B  : TIMER0 TRGO " + (& $tn $r.during.trgo))
    Write-Output ("  RESTORED  : ADC0 trig " + (& $sn $r.restored.adc0) + "   ADC2 trig " + (& $sn $r.restored.adc2) +
                  "   TIMER0 TRGO " + (& $tn $r.restored.trgo) +
                  "   -> " + $(if($r.restore_ok){"VERIFIED"}else{"MISMATCH"}))
    Write-Output ""
    Write-Output "  TEST A - software trigger"
    Write-Output "    phase   n    median      min      max   spread"
    foreach($p in $r.sw){ Write-Output ("      {0}  {1,4} {2,9} {3,8} {4,8} {5,8}" -f $p.phase,$p.n,$p.median,$p.min,$p.max,$p.spread) }
    Write-Output ""
    Write-Output ("  TEST B - TIMER0 TRGO from O3CPRE   EOIC events: ADC0=" + $r.trgo_events[0] +
                  "  ADC1=" + $r.trgo_events[1] + "  ADC2=" + $r.trgo_events[2])
    Write-Output "    phase   n    median     span"
    foreach($p in $r.trgo){ Write-Output ("      {0}  {1,4} {2,9} {3,8}" -f $p.phase,$p.n,$p.median,$p.span) }
    Write-Output ""
    Write-Output ("  DELTA TRGO-SW : A=" + $r.delta[0] + "  B=" + $r.delta[1] + "  C=" + $r.delta[2])
    Write-Output ""
    Write-Output ("VERDICT: CASE " + $r.verdict)
    Write-Output ("REASON : " + $r.reason)
}
$out = @($out | Where-Object { $_.kind -eq "cal" })
if($out.Count -eq 0){ return }

foreach($o in $out){
    $r = $o.dump
    Write-Output ""
    Write-Output "=============================================================================="
    Write-Output ("0x602D CALIBRATION DUMP  (transfer #" + $o.transfer + ", 55 B reassembled)")
    Write-Output ""
    Write-Output ("  magic/CRC     : " + $(if($r.magic_ok){"CC v1"}else{"BAD MAGIC"}) + "   CRC wire " + $r.crc_wire + " computed " + $r.crc_calc + " -> " + $(if($r.crc_ok){"OK"}else{"MISMATCH"}))
    if(-not $r.crc_ok){
        Write-Output "  CRC MISMATCH - every field below is suspect. Do not quote these numbers in the report."
    }
    Write-Output ("  status        : " + $r.status + " " + $r.status_name + "   (last failure: " + $r.failure_reason + " " + $r.failure_name + ")")
    Write-Output ("  source        : " + $r.source + " " + $r.source_name + "   attempts " + $r.attempts)
    Write-Output ("  flags         : " + $r.flags_raw +
                  "  valid=" + [int]$r.valid + " fallback=" + [int]$r.fallback +
                  " selftest_valid=" + [int]$r.selftest_valid + " MOE_off_verified=" + [int]$r.moe_off_verified)
    Write-Output ("  samples       : last_sample_count " + $r.last_sample_count + " (need " + $REQUIRED_SAMPLES + ")   self-test " + $r.selftest_samples + "   self-test saw MOE ON: " + $r.selftest_moe_on)
    Write-Output ("  trigger mode  : " + $r.trigger_mode + $(if($r.trigger_mode -eq 1){" (software-triggered)"}else{""}))
    Write-Output ("  conversions   : issued " + $r.conversions_issued + "   confirmed fresh " + $r.conversions_confirmed_fresh)
    Write-Output ("  self-test st. : " + $r.selftest_status)
    Write-Output ""
    Write-Output "                        phase A      phase B      phase C"
    Write-Output ("  offset          " + ("{0,10} {1,12} {2,12}" -f $r.offset[0], $r.offset[1], $r.offset[2]))
    Write-Output ("  residual mean   " + ("{0,10} {1,12} {2,12}" -f $r.residual_mean[0], $r.residual_mean[1], $r.residual_mean[2]))
    Write-Output ("  P2P             " + ("{0,10} {1,12} {2,12}" -f $r.p2p[0], $r.p2p[1], $r.p2p[2]))
    Write-Output ("  verify mean     " + ("{0,10} {1,12} {2,12}" -f $r.selftest_mean[0], $r.selftest_mean[1], $r.selftest_mean[2]))
    Write-Output ("  verify P2P      " + ("{0,10} {1,12} {2,12}" -f $r.selftest_p2p[0], $r.selftest_p2p[1], $r.selftest_p2p[2]))
    Write-Output ""
    Write-Output "PASS CRITERIA (FW-126 hardware test, step 4):"
    foreach($c in $o.checks){
        Write-Output ("  [" + $(if($c.pass){"PASS"}else{"FAIL"}) + "] " + $c.name.PadRight(28) + $c.detail)
    }
    $failed = @($o.checks | Where-Object { -not $_.pass })
    Write-Output ""
    if($failed.Count -eq 0){
        Write-Output "VERDICT: PASS - the calibration dump satisfies every FW-126 criterion."
    } else {
        Write-Output ("VERDICT: FAIL - " + $failed.Count + " criterion/criteria not met: " + (($failed | ForEach-Object { $_.name }) -join '; '))
    }
    Write-Output ""
    Write-Output "Feeds handoff section 21 items 12 (CAL dump), 13 (offsets A/B/C), 14 (MOE during calibration)."
}
