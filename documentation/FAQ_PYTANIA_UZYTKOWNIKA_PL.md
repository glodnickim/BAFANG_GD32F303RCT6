# FAQ — pytania właściciela o konfigurację

Zbiór pytań, które właściciel zadał w trakcie pracy nad firmware, razem z odpowiedzią i — co
najważniejsze — z informacją, **co poprawiliśmy w aplikacji Canable**, żeby następna osoba nie
musiała pytać o to samo. To materiał źródłowy do instrukcji użytkownika.

Zasada: każde pytanie o ustawienia trafia tutaj. Wpis bez zdania „co poprawiliśmy w UI" jest
niekompletny — jeżeli pytanie w ogóle padło, to znaczy, że interfejs czegoś nie powiedział.

---

## 1. Jak Walk Assist trzyma stałe obroty — od startu czy dopiero powyżej jakiejś prędkości?

*(pytanie z 2026-09-03, karta FW-130)*

**Odpowiedź.** Regulacja działa od pierwszej chwili, ale **nic nie jest ujmowane, dopóki jest
wolno**. To nie jest regulator włączający się po jakimś progu — to jeden ciągły sufit mocy, który
zależy od obrotów zębatki i po prostu jest pełny, dopóki obroty są niskie.

Przy celu 20 obr/min zębatki:

| Obroty zębatki | Co robi sterownik |
|---|---|
| 0 → 17 obr/min | pełna siła marszu |
| 17 → 23 obr/min | prąd zjeżdża płynnie do zera |
| powyżej 23 obr/min | prąd zero (przycisk nadal trzyma sesję) |

Pas jest **wyśrodkowany na Twojej nastawie**, więc dokładnie przy zadanej wartości dostępna jest
połowa siły — sterowanie oscyluje wokół nastawy. Pas rośnie proporcjonalnie do celu, więc 20 i
60 obr/min zachowują się tak samo.

**To trzyma pas, nie jedną liczbę.** Po płaskim rower ustali się bliżej górnego końca, pod górę
bliżej dolnego. Trzymanie dokładnie jednej liczby niezależnie od obciążenia wymagałoby integratora
— czyli dokładnie tego elementu, który wcześniej dawał przestrzelenie prędkości i odcinanie.

**Co poprawiliśmy w UI:** podpowiedź przy `Walk chainring speed` mówi teraz wprost, że prędkość jest
trzymana przez ujmowanie prądu w okolicy celu, więc realne tempo bywa nieco wyższe na płaskim i
nieco niższe pod górę. Wcześniej pole sugerowało dokładną wartość zadaną. Poprawione też minimum
pola: było 18, a firmware od zawsze miał minimum 20 i po cichu podmieniał 18/19 na 20.

---

## 2. Dlaczego marsz był taki słaby przy ruszaniu i czy suwak „Walk motor current" coś robi?

*(temat z 2026-09-03, karta FW-130)*

**Odpowiedź.** Do FW-129 **nie robił nic**. Bajt jechał z aplikacji do sterownika, sterownik go
zapisywał i odsyłał, ale funkcja, która go czyta, nie miała ani jednego wywołania. Rzeczywista siła
marszu była wpisana na sztywno: 40 jednostek prądu, czyli około 3,8 A — **5,7 % tego, czego napęd
używa przy jeździe**. Stąd „mało siły przy ruszeniu".

Od FW-130 ten parametr steruje sufitem siły marszu jako **procent prądu fazowego silnika**:

- domyślnie **25 %**, co po obcięciu daje 157 jednostek ≈ 15 A — maksimum tego firmware, czyli prawie 4× więcej niż dawne sztywne 40;
- powyżej ok. 23 % wszystko trafia w ten sufit i nic już nie rośnie — parametr ma zapas wyłącznie w dół;
- ⚠ wartość domyślna dotyczy tylko świeżego banku — sterownik z zapisaną wartością **zachowa swoją** aż do zmiany w Canable.

**Co poprawiliśmy w UI:** pole `Walk motor current` wróciło na kartę Walk (było ukryte w CB-019
właśnie dlatego, że nic nie robiło) i ma podpowiedź mówiącą, że to regulator siły, a nie prędkości,
oraz od której wartości przestaje mieć znaczenie. Wróciło też do tabeli podglądu parametrów.

---

## 3. Dlaczego wspomaganie marszu potrafiło się urwać w trakcie pchania?

*(temat z 2026-09-03, karta FW-130)*

**Odpowiedź.** Winny był limit prędkości koła (`Walk assist cut-off`, domyślnie 7,0 km/h), który
działał jak **wyłącznik**: przekroczenie o 0,01 km/h zerowało prąd i kasowało sesję, a powrót
zaczynał się praktycznie od zera i potrzebował ponad sekundy na odbudowanie siły. Odczucie:
pchnięcie – cisza – słaby powrót – pchnięcie.

Włączało się to **na lżejszych biegach**, bo przy tych samych obrotach zębatki koło jedzie tam
szybciej. Przykład dla koła 29″ i zębatki 38T przy 20 obr/min: z tyłu 16T ≈ 6,6 km/h (limit śpi),
14T ≈ 7,5 km/h (limit tnie), 12T ≈ 8,7 km/h (tnie mocno).

Od FW-130 limit jest bezpiecznikiem trzystopniowym: przez ostatnie 1,5 km/h przed limitem moc
**zjeżdża płynnie**, dokładnie na limicie prąd jest zerowany, ale **sesja żyje** — powrót to jedna
rampa 0,55 s. Dopiero 1 km/h powyżej limitu Walk Assist zatrzymuje się naprawdę.

**Co poprawiliśmy w UI:** opis `Cut-off speed` mówił „powyżej tej prędkości Walk Assist wyłącza się
całkowicie". Teraz opisuje wszystkie trzy stopnie, więc widać, że łagodne ujęcie mocy przed limitem
jest zamierzone, a nie awarią. Ten sam opis poprawiony w nagłówku karty Walk.
