# AP-01 disturbances-001 - indeks wykresow

Krotkie wyjasnienie dla czytelnika spoza firmware, po polsku. Kazdy wykres to plik SVG - otworz go w przegladarce.

## Co to jest 'zadanie Iq' (Iq ref / iq_ref itd.)

`Iq` to wewnetrzna wielkosc regulatora silnika (skladowa pradu w osi q wektorowego sterowania FOC) uzywana jako 'ile wspomagania silnik ma teraz dostarczyc'. Kanaly `iq_before_profile_limit -> iq_mode_request -> iq_requested -> iq_allowed -> iq_ref` to KOLEJNE etapy skladania tego zadania (limity profilu, tryb, dopuszczenie, ostateczna wartosc przy 16 kHz) - **nie** sa to Nm (moment) ani A (prad rzeczywisty silnika). Ten harness (Controller Lab) nie zamyka petli PMSM/FOC: `u_abs` i zmierzony prad baterii sa stale zerem, wiec te wykresy pokazuja WYLACZNIE kompozycje zadania, nie rzeczywiste zachowanie elektryczne/mechaniczne.

## Jak czytac te wykresy

Panele sa pogrupowane WEDLUG SKALI, nie tematu: flaga 0/1 i licznik kumulacyjny rzedu 20 000 nie maja wspolnej osi Y, wiec narysowane razem nic nie pokazuja. Pionowe czerwone linie przerywane to czasy ZADANE (wejscie); reakcja produkcji jest mierzona osobno w `results/disturbances-metrics.json` i zwykle nastepuje pozniej. Rysowana jest KAZDA probka z zakresu - bez wygladzania, bez decymacji, bez usredniania.

Kazda kategoria ma przypadek kontrolny (bez zaklocenia), wstrzykniecie w trakcie ustalonej jazdy i powrot wejscia do normy przed koncem przebiegu.

## Przypadki kontrolne (bez zadnego zaklocenia)

- **control_8s_cad72_speed18** (odniesienie: ta sama jazda bez zaklocenia): control: undisturbed 8 s ride, 72 rpm, rolling -> [control_8s_cad72_speed18.svg](control_8s_cad72_speed18.svg)
- **control_6s_cad30_speed0** (odniesienie: ta sama jazda bez zaklocenia): control: undisturbed 6 s ride, 30 rpm, standing -> [control_6s_cad30_speed0.svg](control_6s_cad30_speed0.svg)

## Cofanie pedalow (reverse)

- **reverse_clean_cad30_speed0** (czyste cofanie): clean reverse at 30 rpm, standing -> [reverse_clean_cad30_speed0.svg](reverse_clean_cad30_speed0.svg)
- **reverse_clean_cad72_speed18** (czyste cofanie): clean reverse at 72 rpm, rolling 18 km/h -> [reverse_clean_cad72_speed18.svg](reverse_clean_cad72_speed18.svg)
- **reverse_bounce_cad72_speed0** (pojedyncze krotkie odbicie (3 ticki)): reverse bounce (3-tick) at 72 rpm, standing -> [reverse_bounce_cad72_speed0.svg](reverse_bounce_cad72_speed0.svg)
- **reverse_bounce_cad72_speed18** (pojedyncze krotkie odbicie (3 ticki)): reverse bounce (3-tick) at 72 rpm, rolling 18 km/h -> [reverse_bounce_cad72_speed18.svg](reverse_bounce_cad72_speed18.svg)

## Cofanie w pelnej rozdzielczosci 4 kHz - dowod do P-1 (komenda / krawedz / inhibit)

- **hires_reverse_cad72_speed18** (komenda, pierwsza odwrotna krawedz linii i inhibit to trzy rozne chwile): clean reverse, FULL 4 kHz: command 1.0 s, first reverse line edge 1.005 s, inhibit same tick -> [hires_reverse_cad72_speed18_zoom.svg](hires_reverse_cad72_speed18_zoom.svg) , [hires_reverse_cad72_speed18_full.svg](hires_reverse_cad72_speed18_full.svg)
- **hires_reverse_cad30_speed0** (komenda, pierwsza odwrotna krawedz linii i inhibit to trzy rozne chwile): clean reverse, FULL 4 kHz at 30 rpm: first reverse line edge 20.75 ms after the command -> [hires_reverse_cad30_speed0_zoom.svg](hires_reverse_cad30_speed0_zoom.svg) , [hires_reverse_cad30_speed0_full.svg](hires_reverse_cad30_speed0_full.svg)

## Odbicie linii PAS w pelnej rozdzielczosci 4 kHz (3 ticki = 0,75 ms)

- **hires_bounce_cad72_speed18** (pojedyncze odbicie elektryczne; korba caly czas jedzie do przodu): 3-tick PAS bounce, FULL 4 kHz row rate, 72 rpm rolling -> [hires_bounce_cad72_speed18_zoom.svg](hires_bounce_cad72_speed18_zoom.svg) , [hires_bounce_cad72_speed18_full.svg](hires_bounce_cad72_speed18_full.svg)
- **hires_bounce_cad30_speed0** (pojedyncze odbicie elektryczne; korba caly czas jedzie do przodu): 3-tick PAS bounce, FULL 4 kHz row rate, 30 rpm standing -> [hires_bounce_cad30_speed0_zoom.svg](hires_bounce_cad30_speed0_zoom.svg) , [hires_bounce_cad30_speed0_full.svg](hires_bounce_cad30_speed0_full.svg)
- **hires_control_cad72_speed18** (kontrola dla przebiegu 4 kHz): control for the full-rate bounce, 72 rpm rolling -> [hires_control_cad72_speed18.svg](hires_control_cad72_speed18.svg)

## Niewaznosc czujnikow (sensor invalid)

- **tq_invalid_cad72_speed0** (nieprawidlowosc momentu): torque sensor invalid at 72 rpm, standing -> [tq_invalid_cad72_speed0.svg](tq_invalid_cad72_speed0.svg)
- **pas_invalid_cad72_speed18** (nieprawidlowosc PAS): pas sensor invalid at 72 rpm, rolling -> [pas_invalid_cad72_speed18.svg](pas_invalid_cad72_speed18.svg)
- **pas_invseq_cad72_speed0** (nieprawidlowa sekwencja PAS): PAS invalid sequence at 72 rpm, standing -> [pas_invseq_cad72_speed0.svg](pas_invseq_cad72_speed0.svg)
- **tq_invalid_cad30_speed18** (nieprawidlowosc momentu): torque sensor invalid at 30 rpm, rolling -> [tq_invalid_cad30_speed18.svg](tq_invalid_cad30_speed18.svg)
- **pas_invalid_cad30_speed0** (nieprawidlowosc PAS): pas sensor invalid at 30 rpm, standing -> [pas_invalid_cad30_speed0.svg](pas_invalid_cad30_speed0.svg)

## Zaklocenie krawedzi PAS (elektryczne: zgubiona / spozniona krawedz)

- **pas_edge_drop3_cad72_speed18** (zgubiona krawedz - do ISR nie dociera co N-ta zmiana): PAS missed edge (every 3rd) at 72 rpm, rolling -> [pas_edge_drop3_cad72_speed18.svg](pas_edge_drop3_cad72_speed18.svg)
- **pas_edge_drop3_cad30_speed0** (zgubiona krawedz - do ISR nie dociera co N-ta zmiana): PAS missed edge (every 3rd) at 30 rpm, standing -> [pas_edge_drop3_cad30_speed0.svg](pas_edge_drop3_cad30_speed0.svg)
- **pas_edge_jitter8_cad72_speed18** (spozniona krawedz - liczba zmian zachowana, zmienia sie tylko ich czas): PAS late edge (8/4 ticks) at 72 rpm, rolling -> [pas_edge_jitter8_cad72_speed18.svg](pas_edge_jitter8_cad72_speed18.svg)
- **pas_edge_jitter2_cad30_speed0** (spozniona krawedz - liczba zmian zachowana, zmienia sie tylko ich czas): PAS late edge (2/1 ticks) at 30 rpm, standing -> [pas_edge_jitter2_cad30_speed0.svg](pas_edge_jitter2_cad30_speed0.svg)

## Zaklocenia czasu (timebase: opozniona obsluga foreground)

- **miss_tick4_cad72_speed0** (pomijanie tickow w oknie): miss_tick_every_n=4 at 72 rpm, standing -> [miss_tick4_cad72_speed0.svg](miss_tick4_cad72_speed0.svg)
- **fg_delay4_cad72_speed18** (zastoj foreground): fg_delay=4 ticks at 72 rpm, rolling -> [fg_delay4_cad72_speed18_zoom.svg](fg_delay4_cad72_speed18_zoom.svg) , [fg_delay4_cad72_speed18_full.svg](fg_delay4_cad72_speed18_full.svg)
- **fg_delay20_cad72_speed0** (zastoj foreground): fg_delay=20 ticks at 72 rpm, standing -> [fg_delay20_cad72_speed0_zoom.svg](fg_delay20_cad72_speed0_zoom.svg) , [fg_delay20_cad72_speed0_full.svg](fg_delay20_cad72_speed0_full.svg)
- **miss_tick20_cad30_speed18** (pomijanie tickow w oknie): miss_tick_every_n=20 at 30 rpm, rolling -> [miss_tick20_cad30_speed18.svg](miss_tick20_cad30_speed18.svg)
- **fg_delay1_cad30_speed0** (zastoj foreground): fg_delay=1 tick at 30 rpm, standing -> [fg_delay1_cad30_speed0_zoom.svg](fg_delay1_cad30_speed0_zoom.svg) , [fg_delay1_cad30_speed0_full.svg](fg_delay1_cad30_speed0_full.svg)
- **hires_fg_delay1_cad72_speed18** (zastoj foreground, PELNA rozdzielczosc 4 kHz): fg_delay=1 tick, FULL 4 kHz row rate -> [hires_fg_delay1_cad72_speed18_zoom.svg](hires_fg_delay1_cad72_speed18_zoom.svg) , [hires_fg_delay1_cad72_speed18_full.svg](hires_fg_delay1_cad72_speed18_full.svg)
- **hires_fg_delay4_cad72_speed18** (zastoj foreground, PELNA rozdzielczosc 4 kHz): fg_delay=4 ticks, FULL 4 kHz row rate -> [hires_fg_delay4_cad72_speed18_zoom.svg](hires_fg_delay4_cad72_speed18_zoom.svg) , [hires_fg_delay4_cad72_speed18_full.svg](hires_fg_delay4_cad72_speed18_full.svg)
- **hires_fg_delay20_cad72_speed18** (zastoj foreground, PELNA rozdzielczosc 4 kHz): fg_delay=20 ticks, FULL 4 kHz row rate -> [hires_fg_delay20_cad72_speed18_zoom.svg](hires_fg_delay20_cad72_speed18_zoom.svg) , [hires_fg_delay20_cad72_speed18_full.svg](hires_fg_delay20_cad72_speed18_full.svg)

## Regresje (nowe opcje niewlaczone - stare kolumny musza byc identyczne)

- **regr_linear72_baseline** (regresja wobec przyjetej serii): regression: linear72 baseline -> [regr_linear72_baseline.svg](regr_linear72_baseline.svg)
- **regr_stop_release_cad72** (regresja wobec przyjetej serii): regression: stop/release at 72 rpm -> [regr_stop_release_cad72.svg](regr_stop_release_cad72.svg)

## Legenda stanow (na kazdym wykresie, na dole)

- session: 0=RIDE_SESSION_COLD | 1=RIDE_SESSION_ACTIVE | 2=RIDE_SESSION_SUSPENDED_BY_DIRECTION | 3=RIDE_SESSION_WAIT_REARM_LOAD
- debug_flags (maska bitowa): 0x01=RIDE_DBG_WALK | 0x02=RIDE_DBG_CALIBRATION | 0x04=RIDE_DBG_HARD_CUT | 0x08=RIDE_DBG_LEVEL_ZERO | 0x10=RIDE_DBG_NOT_LATCHED | 0x20=RIDE_DBG_MODE_UNSUPPORTED | 0x40=RIDE_DBG_LIMITER_ZEROED | 0x80=RIDE_DBG_COAST_RELEASE
- pas_direction_state: 0=FORWARD_SAFE, 1=DIRECTION_INHIBIT, 2=FORWARD_CONFIRMING
- pas_inhibit_reason: 0=brak, 1=cofanie, 2=nieprawidlowa sekwencja
- ride_interval: 0=nie pedaluje, 1=pierwszy odcinek jazdy, 2=po restarcie
- crank_direction: -1=cofanie, 0=brak ruchu, 1=do przodu

## Ograniczenia dowodowe

Ten harness NIE zamyka petli PMSM/FOC (u_abs=0, brak realnego pradu baterii). Kadencja/moment/predkosc to wymuszone wejscia syntetyczne. Wykresy pokazuja kompozycje zadania (request), nie osiagi mechaniczne/elektryczne ani zachowanie sprzetu. Zadna dlugosc zastoju (1/4/20 tickow) nie jest limitem bezpiecznym dla sprzetu - to punkty cwiczenia API. `motor_erps` jest w harnessie wyprowadzone z kadencji i jest BEZ ZNAKU, wiec proby cofania nie dotykaja obslugi kierunku po stronie silnika.

Proponowane (niezatwierdzone) kryteria: [../PROPOSED_CRITERIA.md](../PROPOSED_CRITERIA.md).
