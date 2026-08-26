param(
  [Parameter(Mandatory=$true)][string]$Log
)

$frames = @{}
foreach($ef in @("1021F","10220","10221","10222","10223","10224","10225","10226","10227")){
  $frames[$ef] = @()
}
Select-String -Path $Log -Pattern "ID:800102[12][0-9A-F]" | ForEach-Object {
  $l = $_.Line
  if($l -match 'ID:800102([12][0-9A-F]).*?Data:([0-9A-F ]+)'){
    $id = "102" + $Matches[1]
    if($frames.ContainsKey($id)){
      $d = $Matches[2].Trim().Split(' ') | ForEach-Object { [Convert]::ToInt32($_,16) }
      $frames[$id] += ,$d
    }
  }
}

function BE16($d,$o){ return ($d[$o] -shl 8) -bor $d[$o+1] }
function BE32($d,$o){ return ($d[$o] -shl 24) -bor ($d[$o+1] -shl 16) -bor ($d[$o+2] -shl 8) -bor $d[$o+3] }
function S16($d,$o){ $v = BE16 $d $o; if($v -ge 32768){ $v -= 65536 }; return $v }
function TS($v){ if($v -eq 65535){ return "NEVER" } else { return ("{0} ticks = {1:N1} ms" -f $v, ($v*1000.0/4000.0)) } }

$hdr = $frames["1021F"]
$t0 = $frames["10220"]; $t1 = $frames["10221"]; $t2 = $frames["10222"]
$cap = $frames["10227"]

$reasonName = @{1='WAIT_LONG';2='WEAK_TARGET';4='NO_PERMISSION';8='NO_LOAD'}
$capStatus = @{0='NONE';1='FULL';2='TRACE_ONLY';3='NO_TRACE_BUSY';4='NO_TRACE_NO_HISTORY'}
$msName = @{1='ENTER_SUSPEND';2='PRESSURE';3='FILTER_READY';4='RUN_READY';5='DEMAND';6='PERMISSION';7='TARGET_RECOVERED';8='SETPOINT_RECOVERED';9='PWM_ON';11='RECORD_CLOSE';12='PROBLEM'}

for($i=0;$i -lt $hdr.Count;$i++){
  $h = $hdr[$i]
  $schema = $h[0]; $sid = $h[1]; $rid = $h[2]; $rsn = $h[3]; $snapCnt = $h[4]; $preIq = S16 $h 5

  "=== record {0} session {1} ===" -f $rid,$sid
  "reason=0x{0:X2} ({1})  snapshots={2}  pre_reverse_iq={3}" -f $rsn, (($reasonName.Keys | Where-Object { $rsn -band $_ } | ForEach-Object { $reasonName[$_] }) -join "+"), $snapCnt, $preIq
  "timing0: t_pressure={0}  t_filter_ready={1}  t_run_ready={2}  t_demand={3}" -f (TS (BE16 $t0[$i] 0)),(TS (BE16 $t0[$i] 2)),(TS (BE16 $t0[$i] 4)),(TS (BE16 $t0[$i] 6))
  "timing1: t_permission={0}  t_target_recovered={1}  t_setpoint_recovered={2}  t_pwm_on={3}" -f (TS (BE16 $t1[$i] 0)),(TS (BE16 $t1[$i] 2)),(TS (BE16 $t1[$i] 4)),(TS (BE16 $t1[$i] 6))
  "timing2: t_standstill_enter={0}  t_standstill_exit={1}  t_weak_start={2}  t_close={3}" -f (TS (BE16 $t2[$i] 0)),(TS (BE16 $t2[$i] 2)),(TS (BE16 $t2[$i] 4)),(TS (BE16 $t2[$i] 6))
  if($cap -and $i -lt $cap.Count){
    "capture: id=0x{0:X2} status={1}" -f $cap[$i][0], ($capStatus[$cap[$i][1]])
  }
  for($s=0;$s -lt $snapCnt;$s++){
    $b = $frames["10223"][$i*4+$s]; $c = $frames["10224"][$i*4+$s]; $d = $frames["10225"][$i*4+$s]; $e = $frames["10226"][$i*4+$s]
    if(-not $b){ continue }
    $el = (BE32 $b 0); $raw = BE16 $b 4; $zero = BE16 $b 6
    $corr = S16 $c 0; $delta = BE16 $c 2; $adel = BE16 $c 4; $afilt = BE16 $c 6
    $arun = BE16 $d 0; $load = BE16 $d 2; $db = BE16 $d 4; $iqr = S16 $d 6
    $iqp = S16 $e 0; $iqs = S16 $e 2; $flags = $e[4]; $ms = $e[5]
    "  snap[{0}] {1} el={2} raw={3} zero={4} corr={5} delta={6} a_delta={7} a_filt={8} a_run={9} load_cg={10} db={11} iq_req={12} iq_pre={13} iq_set={14} flags=0x{15:X2}" -f $s,$msName[$ms],$el,$raw,$zero,$corr,$delta,$adel,$afilt,$arun,$load,$db,$iqr,$iqp,$iqs,$flags
  }
  ""
}