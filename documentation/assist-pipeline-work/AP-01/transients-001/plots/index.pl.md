# AP-01 transients-001 - indeks wykresow

Krotkie wyjasnienie dla czytelnika spoza firmware, po polsku. Kazdy wykres to plik SVG - otworz go w przegladarce.

## Co to jest 'zadanie Iq' (Iq ref / iq_ref itd.)

`Iq` to wewnetrzna wielkosc regulatora silnika (skladowa prądu w osi q wektorowego sterowania FOC) uzywana jako 'ile wspomagania silnik ma teraz dostarczyc'. Kanaly `iq_before_profile_limit -> iq_mode_request -> iq_requested -> iq_allowed -> iq_ref` to KOLEJNE etapy skladania tego zadania (limity profilu, tryb, dopuszczenie, ostateczna wartosc przy 16 kHz) - **nie** sa to Nm (moment) ani A (prad rzeczywisty silnika). Ten harness (Controller Lab) nie zamyka petli PMSM/FOC: `u_abs` i zmierzony prad baterii sa stale zerem, wiec te wykresy pokazuja WYLACZNIE kompozycje zadania, nie rzeczywiste zachowanie elektryczne/mechaniczne.

## Skok momentu (torque step)

- **step_up_ripple_cad72** (wzrost momentu): torque step 15 -&gt; 40 Nm at 72 rpm, baseline ripple/asymmetry -> [step_up_ripple_cad72.svg](step_up_ripple_cad72.svg)
- **step_down_ripple_cad72** (spadek momentu): torque drop 40 -&gt; 15 Nm at 72 rpm, baseline ripple/asymmetry -> [step_down_ripple_cad72.svg](step_down_ripple_cad72.svg)
- **step_up_clean_cad72** (wzrost momentu): torque step 15 -&gt; 40 Nm at 72 rpm, no ripple/asymmetry -> [step_up_clean_cad72.svg](step_up_clean_cad72.svg)
- **step_down_clean_cad72** (spadek momentu): torque drop 40 -&gt; 15 Nm at 72 rpm, no ripple/asymmetry -> [step_down_clean_cad72.svg](step_down_clean_cad72.svg)
- **step_up_clean_cad30** (wzrost momentu): torque step 15 -&gt; 40 Nm at 30 rpm, no ripple/asymmetry -> [step_up_clean_cad30.svg](step_up_clean_cad30.svg)
- **step_up_clean_cad120** (wzrost momentu): torque step 15 -&gt; 40 Nm at 120 rpm, no ripple/asymmetry -> [step_up_clean_cad120.svg](step_up_clean_cad120.svg)

## Restart (stop i ponowne pedalowanie)

- **restart_gap0195ms_speed0**: 195 ms scripted gap, standing (speed 0) - Iq still nonzero at the last pre-restart sample -> [restart_gap0195ms_speed0_zoom.svg](restart_gap0195ms_speed0_zoom.svg) , [restart_gap0195ms_speed0_full.svg](restart_gap0195ms_speed0_full.svg)
- **restart_gap0195ms_speed18**: 195 ms scripted gap, ROLLING (18 km/h) - Iq still nonzero at the last pre-restart sample -> [restart_gap0195ms_speed18_zoom.svg](restart_gap0195ms_speed18_zoom.svg) , [restart_gap0195ms_speed18_full.svg](restart_gap0195ms_speed18_full.svg)
- **restart_gap0200ms_speed0**: 200 ms scripted gap, standing - zero-crossing bracket contains the restart (order undetermined) -> [restart_gap0200ms_speed0_zoom.svg](restart_gap0200ms_speed0_zoom.svg) , [restart_gap0200ms_speed0_full.svg](restart_gap0200ms_speed0_full.svg)
- **restart_gap0200ms_speed18**: 200 ms scripted gap, ROLLING - zero-crossing bracket contains the restart (order undetermined) -> [restart_gap0200ms_speed18_zoom.svg](restart_gap0200ms_speed18_zoom.svg) , [restart_gap0200ms_speed18_full.svg](restart_gap0200ms_speed18_full.svg)
- **restart_gap1500ms_speed0**: control case: 1500 ms gap, standing - Iq observed at zero before the restart -> [restart_gap1500ms_speed0.svg](restart_gap1500ms_speed0.svg)
- **restart_gap1500ms_speed18**: control case: 1500 ms gap, ROLLING - Iq observed at zero before the restart -> [restart_gap1500ms_speed18.svg](restart_gap1500ms_speed18.svg)
- **restart_then_stop2_speed18**: stop, restart, second stop (ride_stop2_s), rolling -> [restart_then_stop2_speed18.svg](restart_then_stop2_speed18.svg)

## Jak czytac opis restartu

Kolejnosc zdarzen rozstrzyga PRZEDZIAL PRZEKROCZENIA ZERA - odcinek miedzy ostatnia probka niezerowa a pierwsza zerowa - porownany z zadana chwila restartu:

- **zero zaobserwowane przed restartem**: kolejnosc jest ZNANA. Niepewna zostaje tylko dokladna chwila przekroczenia wewnatrz przedzialu i dlugosc potwierdzenia.
- **kolejnosc nierozstrzygnieta**: przedzial ZAWIERA chwile restartu, wiec eksport (co 5 ms) nie mowi, co bylo pierwsze. Tak jest w probie 200 ms.
- **Iq niezerowe przy ostatniej probce przed restartem**: to tylko stwierdzenie faktu. Pojedyncza niezerowa probka NIE dowodzi, ze wspomaganie wlasnie opadalo ani ze trwal release. Tak jest w probie 195 ms.

Zero przed restartem to **nie to samo** co potwierdzony stop - stop jest liczony osobno.

## Legenda stanow (na kazdym wykresie, na dole)

- session: 0=RIDE_SESSION_COLD | 1=RIDE_SESSION_ACTIVE | 2=RIDE_SESSION_SUSPENDED_BY_DIRECTION | 3=RIDE_SESSION_WAIT_REARM_LOAD
- debug_flags (maska bitowa): 0x01=RIDE_DBG_WALK | 0x02=RIDE_DBG_CALIBRATION | 0x04=RIDE_DBG_HARD_CUT | 0x08=RIDE_DBG_LEVEL_ZERO | 0x10=RIDE_DBG_NOT_LATCHED | 0x20=RIDE_DBG_MODE_UNSUPPORTED | 0x40=RIDE_DBG_LIMITER_ZEROED | 0x80=RIDE_DBG_COAST_RELEASE

## Ograniczenia dowodowe

Ten harness NIE zamyka petli PMSM/FOC (u_abs=0, brak realnego pradu baterii). Kadencja/moment/predkosc to wymuszone wejscia syntetyczne. Wykresy pokazuja kompozycje zadania (request), nie osiagi mechaniczne/elektryczne ani zachowanie sprzetu.

Zrodlo: [REVIEW-EVD-AP-01-006](../../../../../../integration/task-reports/REVIEW-EVD-AP-01-006.md), poprawka w [EXEC-EVD-AP-01-007](../../../../../../integration/task-reports/EXEC-EVD-AP-01-007.md). Poprawione metryki: `rework-002/transients-metrics-rework-002.json`.