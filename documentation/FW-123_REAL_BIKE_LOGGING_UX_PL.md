# FW-123 — real-bike logging UX: FROZEN → jawny CAN dump

## Wynik

`ROLLING_NO_ASSIST` po przejściu do `FROZEN` zachowuje pełne 256 próbek w RAM.
Zamknięcie sesji diagnostycznej nie może już go automatycznie opróżnić. RAM może
zostać skasowany po power-cycle; nie ma trwałego zapisu do flash.

Dotychczas `diag_session_dump_step()` traktował recorder jak zwykły rekord sesji.
Po udanym auto-dumpie wywoływał `queue_release_session()`, a 256. release przełączał
FSM z `FROZEN` do `IDLE`. To znaczyło, że log mógł zniknąć zanim operator uruchomił
capture Canable. FW-123 odłącza RNA od automatycznego source scan: licznik odmów
pozostaje widoczny w trailerze sesji, lecz dane RNA są obsługiwane wyłącznie przez
osobny, read-only replay.

## Protokół żądania

Wyłącznie obraz diagnostyczny (`CAN_DIAGNOSTICS_ENABLE=1`) przyjmuje zero-bajtowy
`WRITE` od Canable/BESST:

| Pole | Wartość |
|---|---:|
| CAN extended EFID request | `0x0510602C` |
| command | `0x602C` |
| operation | `WRITE` (`0`) |
| target / source | controller `2` / Canable `5` |
| DLC | `0` |
| normal ACK | `0x022A602C`, DLC `0` |
| error ACK | `0x022B602C`, DLC `0` |

`NORMAL_ACK` oznacza: istnieje FROZEN capture i nie ma już uzbrojonego ani aktywnego
replay. `ERROR_ACK` oznacza brak FROZEN capture, trwający już dump albo nieprawidłowy
nadawca/DLC. Kolejny request po zakończeniu dumpu jest prawidłowy i zaczyna od sample
0; request w trakcie jest celowo odrzucony, aby nie powstał niejednoznaczny, częściowy
capture.

Przykład z low-level API właściwego fork-a Canable:

```js
await canBus.sendRawFrame('0510602C', '');
```

Sam request tylko uzbraja transport. Nowe ramki są nadawane wyłącznie poza aktywną
sesją jazdy, z tym samym priorytetem krytycznych ramek i pacingiem 10 ms, co reszta
diagnostyki. Jeżeli rower zacznie jechać w trakcie replay, aktualnie rozpoczęta ramka
jest jedynie potwierdzana; kolejna nie startuje aż do postoju. Żadna próbka i cursor
nie są wtedy odrzucane.

## Oczekiwany transport

Jeden dump ma dokładnie `256 × 7 = 1792` ramek:

| na sample | EFID | liczba w dumpie |
|---|---:|---:|
| HEADER | `0x10248` | 256 |
| DATA 0..5 | `0x10249..0x1024E` | po 256 |

Przy 10 ms na start ramki czas wynosi około 17,92 s plus czas arbitrażu. Nie zatrzymywać
Canable capture przed pojawieniem się ostatniego `0x1024E`; praktycznie zostawić 25 s
od `NORMAL_ACK`.

## Obowiązkowy test real-bike

1. Wgrać obraz `DIAG`, wywołać realny trigger i poczekać aż recorder będzie `FROZEN`.
2. Uruchomić physical CAN/Canable capture **przed** requestem.
3. Wysłać `0x0510602C` DLC 0 i potwierdzić `0x022A602C`.
4. Zostawić capture aktywny co najmniej 25 s.
5. Zakończyć capture i uruchomić ścisły decoder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\decode_rolling_no_assist.ps1 `
  -InputFile <canable-capture.log> -OutputFile <rolling.csv> -RequireCompleteCapture
```

`-RequireCompleteCapture` kończy się `Transport PASS` tylko gdy log zawiera dokładnie
1792 ramki RNA, 256 kompletnych schema-v2 samples, każdą składową HEADER/DATA0..5 po
raz, indeksy `0..255` oraz odstęp kolejnych próbek 16 ticków (4 ms). Brak pojedynczej
ramki, fragment przed HEADER, duplikat fragmentu, mieszanie captures lub luka indeksu
kończy decoder błędem — nie częściowym CSV udającym sukces.

Do testu wybiera się pojedynczy request i pojedynczy dump. Aby sprawdzić replay,
wykonać drugi osobny capture i ponowić request; oba CSV powinny przejść strict PASS i
mieć identyczne dane. Żaden udany dump nie usuwa pierwszego capture z RAM.

## Granice zmiany

Zmiana obejmuje wyłącznie recorder RNA, diagnostyczny CAN request i decoder. Nie
zmienia firmware motor-control, FOC, permission, PWM/MOE ani żadnego parametru jazdy.
