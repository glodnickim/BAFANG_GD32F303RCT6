# Zlecenie reverse — jak stock rozwiązuje kąt wirnika przy bardzo niskich obrotach

**Dla:** agenta reverse engineering, startującego bez kontekstu tego projektu.
**Data:** 2026-09-03. **Zleceniodawca:** właściciel projektu eVistDrive.

> ## ⚠ ZAKRES: WYŁĄCZNIE OBRAZ G532
>
> Decyzja właściciela: **badamy tylko firmware G532.** To najnowsza generacja sterownika i
> **na rowerze jeździ płynnie** — dlatego jest wzorcem. Starsze obrazy (M820 stock) NIE są
> przedmiotem tego zlecenia i **nie wolno przenosić z nich ustaleń jako faktów o G532**.
>
> Dokumentacja M820 w tym repozytorium służy tu **wyłącznie jako metoda pracy** (narzędzia,
> budowa ELF z surowego BIN-u, konwencja zapisu). Każdy fakt o G532 ma pochodzić z obrazu G532.

---

## 1. Po co to zlecenie — jednym akapitem

W naszym firmware słychać wyraźny trzask (klik) w silniku przy **bardzo niskich obrotach**:
raz na końcu wybiegu, gdy zębatka wizualnie już stoi, i raz przy ruszaniu z postoju. Ustaliliśmy
mechanizm u siebie: firmware ma **dwie różne formuły kąta wirnika** i przełącza się między nimi
twardo, a obie dają w tym samym punkcie inny wynik. Jeżeli w chwili przełączenia płynie prąd,
wektor prądu przeskakuje razem z kątem — to jest ten trzask.

**Fabryczny sterownik tego nie robi.** Pytanie brzmi: jak on to rozwiązuje.

Nie chcemy kopiować kodu. Chcemy **opisu algorytmu** na tyle dokładnego, żeby napisać własną
implementację i wiedzieć, czy w ogóle idziemy w dobrą stronę.

---

## 2. Materiał wejściowy

| Co | Gdzie |
|---|---|
| **Przedmiot badania — jedyny** | `CRX30PC3615F805001.0_G532_250W_25_700-2185_Git-67a37d78_20251117_1756(3).bin` |
| SHA-256 | `93c85ab1db4c4e9108ba07014cccf859ad4e53b3d7ce2371458f3edd0e7398a8` |
| **Metoda** (narzędzia, ELF z surowego BIN-u, Thumb disasm, literal pools, mapy RAM) | `documentation/M820_REVERSE_METHOD_REPRODUCIBILITY.md` |
| Konwencja zapisu i skala pewności | `documentation/M820_REVERSE_KNOWLEDGE_BASE.md`, sekcja 0 |
| Format rejestru dowodów | `documentation/M820_REVERSE_EVIDENCE_LEDGER.md` |

Dokumenty M820 czytaj **tylko dla metody i konwencji**. Ich ustalenia merytoryczne dotyczą innego
sterownika i w tym zleceniu nie mają mocy dowodowej.

### 2.1 Punkty zaczepienia już odzyskane z TEGO obrazu

Z wcześniejszego reverse Walk Assist na tym samym BIN-ie (dokument
`EVistDrive_G532_Walk_Assist_reverse_i_plan_wdrozenia.md`, poza repozytorium) mamy potwierdzone
adresy. Zaczynaj od nich, zamiast szukać od zera:

| Adres / miejsce | Co to jest |
|---|---|
| `0x080015F0` | główna funkcja motor-control, automat stanów |
| `0x080017B4` | handler stanu 5 (Walk Assist) |
| `0x0800353C` | blok walidacji/debounce komendy z HMI |
| `0x080179FA` | parser ramek CAN |
| `0x20001304 + 0x50` | struktura stanu automatu motor-control |

Ustalone tam również: istnieje **osobny końcowy slew prądu Q**, ok. **56,4 ms pełnej skali**,
pracujący przy **16 kHz** — czyli pętla sterowania G532 najpewniej działa z tą częstotliwością.
To jest wskazówka, gdzie szukać toru kąta: w tym samym przerwaniu.

---

## 3. Hipotezy o hardware — DO ZWERYFIKOWANIA NA G532, nie fakty

Poniższe pochodzi z reverse **innego sterownika (M820)** i jest tu wyłącznie po to, żeby wiedzieć,
czego szukać. **Nie zakładaj, że w G532 jest tak samo — sprawdź.**

- rotor jako 3-bitowy stan z trzech pinów GPIO, timer w trybie hall-interface (XOR/capture/reset)
  mierzący czas między przejściami, 6 przejść na obrót elektryczny;
- możliwy **enkoder MT6816 w trybie UVW** zamiast trzech osobnych Halli.

### P0 — najpierw ustal, czym G532 w ogóle mierzy położenie wirnika

Zanim odpowiesz na cokolwiek innego: **które piny, który timer, jaki tryb, ile przejść na obrót
elektryczny** w TYM obrazie. Jeżeli okaże się, że G532 czyta wirnik inaczej niż M820, to jest
pierwszy i najważniejszy wynik zlecenia.

Liczby par biegunów **nie zgaduj**.

---

## 4. Pytania — w kolejności wartości

### P1 (NAJWAŻNIEJSZE) — czy stock w ogóle używa tylko UVW?

MT6816 potrafi oddać **absolutny kąt** przez SPI, ABZ albo PWM. Jeżeli stock czyta absolutny kąt
choćby pomocniczo, to **cały problem u nas nie istnieje w nim wcale** — bo absolutny czujnik ma
poprawny kąt także przy zerowych obrotach i żadna formuła zastępcza nie jest potrzebna.

Do sprawdzenia:
- czy inicjalizowany jest SPI/USART w trybie master do czujnika (piny, prescaler, ramka 16-bit),
- czy jest odczyt cykliczny (DMA?) z rejestru kąta,
- czy w torze FOC pojawia się wartość kąta o rozdzielczości wyraźnie lepszej niż 60°,
- czy jest obsługa bitów statusu MT6816 (np. flaga słabego pola / no-mag).

**To jest pytanie, na które odpowiedź TAK zmienia wszystko inne.** Odpowiedz na nie najpierw
i zaraportuj natychmiast, nawet zanim skończysz resztę.

### P2 — czy istnieje próg prędkości przełączający sposób liczenia kąta?

- Znajdź porównanie prędkości/okresu, które wybiera sposób wyliczania kąta.
- Podaj **stałą liczbową z BIN-u** oraz jej **jednostkę fizyczną** (tiki timera → obr. elektryczne/s),
  z pokazanym przeliczeniem i częstotliwością zegara timera.
- Czy jest **histereza** (dwa różne progi w górę i w dół)? Podaj oba.

### P3 — twarde przełączenie czy przenikanie?

To jest sedno zlecenia.

- Czy w okolicy progu kąt jest liczony jako **mieszanka** dwóch źródeł (np. `a*x + (1-a)*y`,
  mnożenie przez współczynnik rosnący z prędkością, tabela wag)?
- Czy widać instrukcje mnożenia/przesunięcia realizujące wagę zależną od prędkości?
- Jeżeli przełączenie jest twarde — jak stock unika skoku? (patrz P4 i P5)

### P4 — czy korekta/offset kąta jest stosowana w OBU gałęziach?

U nas gałąź „wolna" pomija stałą korektę kąta, którą gałąź „szybka" dodaje — więc same formuły
różnią się o stały offset, niezależnie od interpolacji. **Sprawdź, czy stock stosuje tę samą
korektę w obu ścieżkach.** Jeśli tak, to samo w sobie jest ważnym wynikiem.

### P5 — co stock robi z PRĄDEM w tej strefie?

- Czy przy **zwalnianiu** wymusza zero prądu poniżej jakiejś prędkości (odpowiednik naszego
  zabezpieczenia)? Jaki próg?
- Czy przy **ruszaniu** pozwala na prąd w tej samej strefie (u nas musi, inaczej nie ruszy)?
- Czy jest jawna **asymetria release vs start** — dwie różne ścieżki dla tego samego zakresu
  prędkości? Jeśli tak, opisz warunek, który je rozróżnia.

### P6 — postój i pierwszy kąt po postoju

- Czy przy zatrzymanym wirniku kąt jest **zamrażany** (ostatnia znana wartość), czy podstawiany
  jako środek sektora?
- Czy przy załączeniu mostka jest jakaś **procedura ustawienia wirnika** (prąd wyrównujący,
  alignment) zanim popłynie moment?
- Czy mostek w ogóle zostaje załączony na postoju, czy jest rozłączany?

### P7 — kolejność w przerwaniu

W którym miejscu cyklu sterowania kąt jest liczony względem próbkowania prądu i wystawienia PWM?
Skok kąta boli tym bardziej, im później jest wprowadzony. Interesuje nas kolejność, nie
mikrosekundy.

---

## 5. Format odpowiedzi

Trzymaj się konwencji tego projektu — **fakty oddzielnie od metody**, każdy wynik z etykietą
pewności:

- **PEWNE** — wynika wprost z disasm, pokazany adres i instrukcje;
- **BARDZO MOCNE** — spójna interpretacja z więcej niż jednego miejsca, ale bez bezpośredniego dowodu;
- **OTWARTE** — hipoteza, wymaga dalszej pracy. Napisz wprost, czego brakuje.

Dla każdego ustalenia podaj:
1. **adres funkcji / instrukcji** w BIN-ie,
2. **wartość stałej** tak, jak leży w binarce,
3. **jednostkę fizyczną** i przeliczenie,
4. krótki fragment disasm jako dowód,
5. etykietę pewności.

Nie pisz „prawdopodobnie tak jest" bez etykiety. Wolimy uczciwe OTWARTE niż ładne zdanie.

---

## 6. Weryfikacja — jak sprawdzić samego siebie

- **Sanity na jednostkach.** Wyliczony próg powinien wypaść w okolicy **kilku obrotów elektrycznych
  na sekundę** — czyli prędkości, przy której zębatka roweru wizualnie już stoi. Jeżeli wychodzi
  Ci próg rzędu setek obr/s, jednostka jest pomylona.
- **Spójność wewnątrz obrazu.** Ten sam rejestr/zmienna prędkości jest zwykle używana w kilku
  miejscach (limiter, wyświetlacz, ochrona). Sprawdź, czy Twoja interpretacja jednostki daje
  sensowne wyniki także tam. Jeżeli nie — jednostka jest zła.
- **Bez porównań z innymi obrazami.** Zlecenie jest ograniczone do G532 (patrz ramka na górze).
  Jeżeli uważasz, że drugi obraz rozstrzygnąłby sprawę, **napisz to jako rekomendację**, ale nie
  wciągaj jego ustaleń do wyniku.

---

## 7. Czego NIE robić

- **Nie kopiować kodu z binarki.** Zlecenie dotyczy opisu algorytmu; implementację piszemy sami.
  Wynik ma być na tyle ogólny, żeby dało się go zaimplementować niezależnie.
- **Nie zgadywać liczby par biegunów** ani innych rzeczy oznaczonych w bazie jako OTWARTE.
- **Nie mieszać** ustaleń z tego BIN-u z ustaleniami dotyczącymi naszego firmware. To dwa różne
  źródła i mają zostać rozdzielone w raporcie.
- **Nie rozszerzać zakresu.** Walk Assist, PAS, moment, bateria — poza zleceniem. Interesuje nas
  wyłącznie **kąt wirnika przy prędkości bliskiej zeru i prąd w tej strefie**.

---

## 8. Kryterium zaliczenia

Zlecenie jest wykonane, jeżeli po lekturze raportu potrafimy odpowiedzieć:

1. Czy stock używa absolutnego kąta z enkodera, czy tylko trzech bitów UVW? *(P1)*
2. Czy ma jeden sposób liczenia kąta, czy dwa? *(P2)*
3. Jeżeli dwa — czy przechodzi między nimi płynnie, i po jakiej wielkości? *(P3)*
4. Czy dopuszcza prąd w strefie przejścia, i czy inaczej przy ruszaniu niż przy zwalnianiu? *(P5)*

Odpowiedź „stock ma ten sam problem i go nie rozwiązuje" **też jest wynikiem** i też jest
wartościowa — pod warunkiem, że jest udowodniona, a nie założona.

---

## 9. Kontekst z naszej strony (dla porównania, nie do naśladowania)

Podajemy nasze liczby, żeby agent wiedział, czego szuka rzędem wielkości. **To nie są liczby
z fabrycznego BIN-u** — to nasze własne wartości.

| U nas | Wartość |
|---|---|
| przejście na formułę zastępczą przy zwalnianiu | poniżej ~5,6 obr. elektr./s |
| powrót na formułę interpolowaną przy przyspieszaniu | powyżej ~8,3 obr. elektr./s |
| koniec podawania prądu przy zwalnianiu | ~10 obr. elektr./s |
| przy ruszaniu | prąd w tej strefie jest konieczny — brak ochrony |
| korekta kąta w gałęzi zastępczej | **nie jest dodawana** (podejrzewamy, że to część problemu) |

Nasza robocza hipoteza, którą chcemy potwierdzić albo obalić: **kąt trzeba przenikać, a nie
przełączać**, i korektę stosować w obu gałęziach. Jeżeli stock robi coś mądrzejszego —
to jest właśnie to, po co jest to zlecenie.
