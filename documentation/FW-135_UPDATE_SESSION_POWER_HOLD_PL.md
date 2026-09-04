# FW-135 — sterownik nie może odciąć zasilania podczas aktualizacji wyświetlacza

**Status: ZAAKCEPTOWANE PRZEZ WŁAŚCICIELA I WDROŻONE W KODZIE. NIE ZBUDOWANE KANONICZNIE,
NIE NA ROWERZE.**
Data: 2026-09-04. Zgłoszenie właściciela po jeździe na 0.502:
*„dając teraz obsługę wyłączenia sterownika bez komunikacji spowodowało to wyłączenie zasilania
podczas aktualizacji wyświetlacza"*.

Kontekst: `EVistDrive_CAN_HMI_UPDATE_SHUTDOWN_WYTYCZNE_DLA_AGENTA.md` §3.2, §3.3, §4, §9 (P4).

---

## 1. Co się stało — prostym językiem

Sterownik ma własny bezpiecznik: **„jeżeli przez 10 sekund nikt się do mnie nie odezwał, a rower
stoi — wyłącz się"**. Wyłączenie oznacza dosłownie odcięcie przetwornicy **i zasilania
wyświetlacza**.

Podczas aktualizacji wyświetlacza:

- wyświetlacz jest w bootloaderze i **milczy**,
- programator rozmawia z **wyświetlaczem** (adresat 3), a nie ze sterownikiem (adresat 2),
- sterownik nie widzi ani jednej ramki skierowanej **do siebie**,
- po 10 sekundach uznaje, że jest sam, i **odcina zasilanie w trakcie flashowania**.

To jedyny scenariusz w całym projekcie, w którym możemy fizycznie uszkodzić wyświetlacz.

## 2. Dlaczego to wyszło dopiero teraz — a nie na 0.500

Bezpiecznik jest w kodzie od dawna (commit `706fcc3 feat(safety): auto-off after inactivity +
HMI comms watchdog`). Do 0.500 **nigdy nie zdążył zadziałać podczas aktualizacji**, bo:

```text
0.500 i wcześniej:  updater ogłasza sesję ramką 0x85FF3005 (broadcast)
                 -> nasz kod robił NVIC_SystemReset()
                 -> restart zerował comm_seen
                 -> okres karencji startowej zaczynał się od nowa
                 -> licznik 10 s nigdy nie dochodził do końca
```

Czyli **restart maskował bezpiecznik**. FW-132 słusznie przestał reagować na broadcast — i tym
samym odsłonił mechanizm, który był tam przez cały czas.

Ważne rozróżnienie: **FW-132 nie dodał wyłączania.** Usunął przypadkowy restart, który je
zasłaniał. Obie rzeczy były błędami; naprawiliśmy jedną i zobaczyliśmy drugą.

**Aktualizacja sterownika działa** (potwierdzone na rowerze) i to się zgadza: tam programator
adresuje `target = 2`, więc jego ramki zerują licznik, a `0x3005` jest obsługiwane normalnie.

## 3. Jak to jest robione fabrycznie — uczciwa odpowiedź

W całym materiale z reverse — `BAFANG_CAN_STOCK_VERIFIED_REFERENCE.md` i
`M820_STOCK_COMPLETE_REFERENCE.md` — **nie ma ani jednego śladu**, żeby fabryczny sterownik w
ogóle wyłączał się z powodu braku komunikacji. Jedyny znaleziony timeout to timeout **sesji
transportowej CAN** (500 tików → przerwanie transferu wieloramkowego), co jest czymś zupełnie
innym niż odcięcie zasilania.

Historia git potwierdza to samo z drugiej strony: ten bezpiecznik jest **naszym własnym
dodatkiem**, nie odwzorowaniem fabryki.

**Wniosek: fabryka prawdopodobnie nie ma tego problemu, bo nie ma tego mechanizmu.** Nie musimy
więc szukać, „jak oni to obchodzą" — mamy zawęzić własny wynalazek tak, żeby nie strzelał we
własny sprzęt.

## 4. Propozycja — rozdzielić dwa różne pytania

Dziś **jeden licznik** odpowiada na dwa zupełnie różne pytania. To jest źródło błędu.

| Pytanie | Czego naprawdę wymaga | Próg | Skutek |
|---|---|---:|---|
| Czy wolno **wspomagać**? | żywy **wyświetlacz** (`source == 3`) | 3 s | `assist = 0` |
| Czy wolno **zostać włączonym**? | żywa **magistrala** (dowolna ramka) | 10 s + postój | wyłączenie |

Obecny warunek `target == 2` jest jednocześnie:

- **za szeroki** dla pierwszego pytania — CANable albo konfigurator udaje żywy wyświetlacz, więc
  wspomaganie zostaje dopuszczone przy martwym HMI (to jest dokładnie zarzut §3.2 karty
  wytycznych);
- **za wąski** dla drugiego — programator wyświetlacza wypełnia magistralę ruchem, a mimo to nie
  liczy się jako oznaka życia.

Rozdzielenie naprawia oba naraz i jest zmianą **wyłącznie w warunkach zerowania dwóch liczników**.

## 5. Dodatkowo — zatrzask sesji aktualizacji

Sama „żywa magistrala" ma dziurę: przy kasowaniu pamięci wyświetlacza może wystąpić cisza dłuższa
niż 10 sekund.

Dlatego drugi element: po zobaczeniu `0x3005` (**dowolny adresat, także broadcast**) blokujemy
wyłączenie z powodu ciszy na **5 minut** (`UPDATE_HOLD_TICKS`).

**Zmiana wobec pierwszej wersji tej karty:** zatrzask **nie puszcza wcześniej** po powrocie ramki
od wyświetlacza. Byłaby to dziura — wyświetlacz zwykle wysyła jeszcze ramkę czy dwie **po**
ogłoszeniu sesji, zanim faktycznie wejdzie do bootloadera, więc zatrzask zdjąłby się natychmiast
i nigdy już nie został uzbrojony. Zatrzask po prostu wygasa z czasem.

Zatrzask blokuje **wyłącznie wyłączanie z powodu ciszy**. Przycisk on/off i auto-off po
bezczynności są nietknięte, więc rower nadal wyłącza się sam i nie da się go tym zablokować
w stanie włączonym. Test T5 pilnuje dokładnie tego.

## 6. Czego ta karta NIE robi

- **nie dotyka** sterowania silnikiem w żadnym miejscu;
- **nie zmienia** przycisku on/off ani auto-off po bezczynności;
- **nie wprowadza** maszyny stanów update z §4/§9 karty wytycznych — to osobny, większy temat.
  Ta karta ma tylko przestać niszczyć sprzęt.

## 7. Ryzyko

**Niskie, z jednym miejscem do sprawdzenia.**

Zawężenie pierwszego licznika do `source == 3` znaczy, że gdyby nasz wyświetlacz nie odzywał się
jako źródło 3, wspomaganie nigdy nie byłoby ucinane przy jego śmierci. Sprawdziłem to w Twoim
logu z tego samego HMI:

```text
ID:83106300   ->  source = 3, target = 2     (wyświetlacz -> sterownik, co ~100 ms)
```

Zgadza się. Dodatkowo licznik uzbraja się dopiero po pierwszej ramce od HMI — tak jak dziś — więc
brak wyświetlacza nie tworzy fałszywego alarmu przy starcie.

## 8. DO CZASU WDROŻENIA

⚠ **Nie aktualizuj wyświetlacza na 0.502.** Aktualizacja **sterownika** jest bezpieczna
i potwierdzona.

Jeżeli musisz zaktualizować wyświetlacz wcześniej — wróć na czas aktualizacji do buildu sprzed
FW-132. Tam kontroler się zrestartuje (co też jest błędem, HMI potrafi wrócić z błędem
komunikacji), ale **nie odetnie zasilania w trakcie flashowania**.

## 9. Testy po wdrożeniu

- **T1 — aktualizacja wyświetlacza.** Musi przejść do końca, bez wyłączenia sterownika i bez
  jego restartu. To jest cały cel karty.
- **T2 — aktualizacja sterownika.** Nadal działa (dziś działa — nie wolno tego zepsuć).
- **T3 — odłączony wyświetlacz.** Rower włączony, wtyczka HMI wypięta: po ~3 s ma zniknąć
  wspomaganie, po ~10 s na postoju sterownik ma się wyłączyć. Bezpiecznik musi nadal działać.
- **T4 — sam CANable, bez HMI.** Podłączony konfigurator ma **utrzymać zasilanie** (żywa
  magistrala), ale **nie ma dawać prawa do wspomagania**. To jest różnica, której dziś nie ma.
- **T5 — normalna jazda.** Bez zmian.

---

## 10. Co dokładnie zmieniono w kodzie

| Plik | Zmiana |
|---|---|
| `inc/config.h` | nowa stała `UPDATE_HOLD_TICKS 7500` (5 min); komentarz przy `COMM_OFF_TICKS` mówi teraz o **ciszy na magistrali**, nie o braku ramek HMI |
| `src/CAN_Display.c` | `bus_lost_ticks`/`bus_seen` zerowane przy **każdej** odebranej ramce, **przed** bramką `target == 2`; `hmi_lost_ticks`/`hmi_seen` tylko przy `source == 3`; `0x3005` uzbraja zatrzask |
| `src/main.c` | jeden licznik `comm_*` zastąpiony dwoma; wyłączenie z powodu ciszy zależy od licznika **magistrali** + postoju + braku zatrzasku; odcięcie wspomagania odpala **którykolwiek** z liczników |
| `src/main.c` (DIAG) | ramka `0x1022B` bajt7: doszły bit3 (magistrala uzbrojona) i bit4 (zatrzask aktywny) |
| `tests/host/fw135_update_power_hold_host.c` | nowy zestaw testów T1–T7 |

## 11. Dlaczego odcięcie wspomagania odpala też licznik magistrali

Zawężenie licznika wspomagania do `source == 3` samo w sobie **osłabiłoby** bezpiecznik: gdyby
wyświetlacz nigdy nie przedstawił się jako źródło 3, licznik nigdy by się nie uzbroił i urwany
kabel przestałby zatrzymywać silnik.

Dlatego wspomaganie ucina **którykolwiek** z dwóch liczników. Zupełnie cicha magistrala to
wyrwana wtyczka i musi zatrzymać silnik niezależnie od tego, czy wyświetlacz kiedykolwiek się
odezwał. Test T6 pilnuje tej własności.

## 12. Stan weryfikacji

- kompilacja **Developer** (numer wersji **nie zużyty**): NORMAL `RESULT: PASS`,
  DIAG `RESULT: PASS`;
- nowy zestaw hostowy **T1–T7 PASS**;
- pełny przebieg hostowy bez **nowych** awarii — zostaje wyłącznie znana rodzina diag schema-3
  (`rolling_no_assist_diag_host`), ta sama co przed tą kartą.

**Build kanoniczny NIE wykonany, firmware NIE na rowerze.**
