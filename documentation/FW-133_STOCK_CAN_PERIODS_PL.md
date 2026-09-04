# FW-133 — okresy nadawania do HMI zgodne z fabryką

**Status: WDROŻONE W KODZIE, NIE ZBUDOWANE (build kanoniczny), NIE NA ROWERZE.**
Data: 2026-09-04. Decyzja właściciela: *„poprawiamy tylko rytm nadawania"* — więc **wyłącznie
stałe czasowe i rozdzielenie jednej funkcji**. Żadnych zmian w konfiguracji, sterowaniu ani
protokole.

Podstawa: `BAFANG_CAN_STOCK_VERIFIED_REFERENCE.md` §10 — tabela okresów **odczytana z deskryptorów
fabrycznego firmware**, nie oszacowana z logu.
Kontekst: `RAPORT_ZGODNOSC_CAN_I_KONFIGURACJA_PL.md` §2, `FW-132` §2.3 (kolejka gubi po cichu).

---

## 1. Co było nie tak

Dwie ramki leciały **kilkanaście razy częściej**, niż wyświetlacz ich oczekuje, a trzy z nich
wychodziły **jednocześnie w tym samym takcie**, bo jedna funkcja wysyłała wszystkie trzy.

Trzy ramki naraz do kolejki o 16 miejscach, która przy przepełnieniu **cicho wyrzuca** — to
własnymi rękami zwiększone ryzyko zgubienia podtrzymania, o które nam chodzi.

## 2. Co zmieniono

| Ramka | Fabryka | Było | Jest | Uwaga |
|---|---:|---:|---:|---|
| `0x1200` | 495 ms | 480 ms | 480 ms | bez zmian, już było dobrze |
| `0x320F` | 1980 ms | 480 ms | **2000 ms** | było 4× za często |
| `0x3000` | 9900 ms | 480 ms | **9920 ms** | było **20× za często** |
| `0x3201` | 247 ms | 280 ms | **240 ms** | bliżej fabryki niż wcześniej |
| `0x3200` | 1980 ms | 1480 ms | **2000 ms** | |
| `0x3205` | 3960 ms | 320 ms | **3960 ms** | było **12× za często**; teraz **dokładnie** |
| `0x3202` | 99 ms | 120 ms | 120 ms | patrz niżej |

`sendCAN_status_broadcast()` wysyłała `0x1200` + `0x320F` + `0x3000` jednym wywołaniem. Rozdzielona
na `sendCAN_status_frame(MS, index)`; wersja zbiorcza została **wyłącznie** dla jednorazowej
ścieżki startowej/kalibracyjnej, która nie ma własnego harmonogramu.

## 3. Czego NIE dało się trafić dokładnie i dlaczego

Nasza gałąź cykliczna ma podstawę **40 ms**, a fabryczna wygląda na **~9,9 ms** (wszystkie okresy
są wielokrotnościami 99). Dwóch wartości nie da się więc trafić:

- **`0x3202`, fabrycznie 99 ms.** Zostaje 3 takty = 120 ms (+21 %). Alternatywa to 2 takty = 80 ms,
  czyli **szybciej niż fabryka i więcej ruchu** — dokładnie w stronę, której chcemy uniknąć.
  Świadomie zostawiam wolniej.
- **`0x3201`, fabrycznie 247 ms.** 6 taktów = 240 ms (−3 %), i tak lepiej niż dotychczasowe 280 ms.

Zejście z podstawy 40 ms na ~10 ms dałoby dokładność, ale to **przebudowa harmonogramu**, nie
poprawka rytmu — poza zakresem tej karty.

## 4. Efekt liczbowy

Ruch generowany przez nas w stanie ustalonym:

```text
przed:  ok. 22 ramek/s
po:     ok. 16 ramek/s      -> okolo 27 % mniej
```

Ważniejsze od średniej: **szczyt.** Co 480 ms wychodziły trzy ramki w jednym takcie; teraz w
zdecydowanej większości przejść wychodzi **jedna**. To bezpośrednio zmniejsza ryzyko przepełnienia
16-miejscowej kolejki, czyli mechanizm z FW-132 §2.3.

## 5. Czego ta karta NIE robi

- **nie dodaje** brakujących ramek (`0x3204` co 50 ms, `0x320E`/`0x3210`/`0x3213`,
  `0x1201`–`0x1204`) — ich zawartość jest w dokumencie oznaczona jako `SEMANTIC_UNKNOWN`,
  a wysłanie zmyślonych danych byłoby gorsze niż niewysłanie żadnych;
- **nie rusza** konfiguracji (assist, max power, banki) — właściciel świadomie odłożył ten temat;
- **nie rusza** sterowania silnikiem w żadnym miejscu.

## 6. Stan

Kompilacja sprawdzona w trybie deweloperskim, **oba warianty `RESULT: PASS`**, numer wersji
**nie zużyty**, katalog tymczasowy usunięty. Pełny przebieg hostowy **bez nowych awarii** —
zostaje wyłącznie znana rodzina diag schema-3.

**Build kanoniczny nie wykonany.**

## 7. Testy

- **T1 — HMI działa normalnie.** Prędkość, kadencja, poziom wspomagania, ikona Walk Assist,
  bateria. Nic nie może zniknąć ani zacząć mrugać.
- **T2 — Walk Assist.** To ta funkcja, która najbardziej zależy od podtrzymania: `0x320F` idzie
  teraz 4× rzadziej. Jeżeli WA zacznie wypadać po kilku sekundach, ta ramka jest pierwszym
  podejrzanym i wracamy dla niej do krótszego okresu.
- **T3 — rytm w logu.** Odstępy `0x02F83000` mają być ~9,9 s, `0x02F8320F` ~2 s, `0x02F83205`
  ~3,96 s.
- **T4 — zgubione ramki.** W buildzie DIAG `0x1022B` Data1 (z FW-132) powinno rosnąć **wolniej**
  niż przed tą zmianą, albo wcale.

---

# FW-134 — nadajemy `0x3210` z licznikiem ruchu (próba migającej ikony WA)

**Status: WDROŻONE W KODZIE, NIE ZBUDOWANE (build kanoniczny), NIE NA ROWERZE.**
Data: 2026-09-04. Podstawa: log właściciela `ON - WA - OFF` z fabrycznego **M510 na tym samym
wyświetlaczu** — analiza w `RAPORT_ZGODNOSC_CAN_I_KONFIGURACJA_PL.md`, ANEKS.

## 1. Po co

Ikona Walk Assist na eVistDrive **nigdy nie zamigała**. Na fabrycznym rowerze pojawia się przy
wejściu w tryb i **miga, gdy rower jedzie**.

Log pokazał, dlaczego. Pojawianie się nie wymaga od nas niczego — wyświetlacz sam wysłał
`6300[1]=06`, więc wie. Ale miganie pokrywa się co do sekundy z oknem, w którym fabryczny
sterownik **przyrasta licznik w ramce `0x3210`, bajty 4–5**:

```text
okno TRYBU      869983,2 -> 869999,5 s   (16,3 s)
okno PRZYROSTU  869986,6 -> 869996,7 s   (10,1 s), +1 co 1,00 s
```

Przyrost zaczyna się **3,4 s po** wciśnięciu i kończy **2,8 s przed** puszczeniem — czyli licznik
śledzi **ruch**, a nie tryb. Rower na obu końcach stał, mimo trzymanego przycisku.

`0x3210` nie występowało w naszym kodzie ani razu.

## 2. Co wysyłamy

Ramka rozgłoszeniowa `0x82F83210`, DLC 8, co **1000 ms** (fabryka 990).

| Bajty | Co wysyłamy | Dlaczego |
|---|---|---|
| 0..3 | **zera** | w logu stałe (1080 i 4242), w dokumencie `SEMANTIC_UNKNOWN`. Stałe z cudzego roweru to nie są nasze dane |
| **4..5** | **sekundy ruchu, LE16** | jedyne pole, którego zachowanie log **udowadnia** |
| 6..7 | zera | w logu też zera |

Licznik jest **sesyjny, nie trwały**. Fabryczna wartość 18424 s (~5,1 h) wygląda na licznik
całkowitego czasu jazdy, ale dokument oznacza znaczenie jako nieudowodnione — wymyślanie zapisu
w pamięci trwałej byłoby twierdzeniem więcej, niż wiemy.

## 3. Definicja „jedzie" — celowo z koła **lub** silnika

```c
if(MS.Speedx100 > 0 || ui16_erps >= RIDE_COAST_RELEASE_ERPS) ride_seconds++;
```

Sama prędkość koła by nie wystarczyła: przy tempie marszu jeden impuls koła przypada co ~2,6 s,
czyli **dokładnie na progu wygaszania prędkości**. Wskazanie migałoby do zera w jedynym przypadku,
dla którego ten licznik powstaje. Silnik jest tam jednoznaczny.

## 4. Ryzyko

**Niskie.** Nowa ramka rozgłoszeniowa, jedna na sekundę, przez normalną kolejkę. Nie dotyka
sterowania silnikiem ani konfiguracji. Ruch rośnie o ~1 ramkę/s — po FW-133 mamy zapas
(~16 ramek/s zamiast ~22 przed).

## 5. Czego to NIE gwarantuje

To jest **próba oparta na korelacji**, nie na potwierdzonej semantyce. Wiadomo, że licznik tyka
dokładnie w oknie migania; **nie** wiadomo, czy wyświetlacz wiąże go z ikoną.

Jeżeli nie zamiga, kolejni kandydaci w tej samej kolejności:
1. brakujące `0x1203` / `0x1204` (rodzina status/event, których nie wysyłamy),
2. nasz `0x320F` — wysyłamy `0x01` w bajcie 0, fabryka wysyła **osiem zer**,
3. brakujące `0x320E` / `0x3213`.

## 6. Test

Wejść w Walk Assist i **postać kilka sekund bez ruchu** (ikona ma się pojawić, ale nie migać),
potem ruszyć (ma zamigać), potem znowu stanąć trzymając przycisk (ma przestać migać). Ten wzorzec
jest ważniejszy niż samo „miga/nie miga" — odtwarza dokładnie to, co widać w logu fabrycznym.
