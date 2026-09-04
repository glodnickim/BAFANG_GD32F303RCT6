# EVistDrive — CAN / HMI LIFECYCLE / UPDATE / SHUTDOWN
## Audyt, przyczyna problemów i plan bezpiecznego wdrożenia

**Status dokumentu:** wytyczne wdrożeniowe dla agenta  
**Projekt:** EVistDrive / Bafang G532 / M820 / DP-C245  
**Data:** 2026-09-04  
**Priorytet:** wysoki — poprawa lifecycle CAN/HMI/update/power bez zmiany charakterystyki jazdy

---

# 1. CEL

Celem jest poprawienie zachowania EVistDrive w czterech sytuacjach:

1. normalne uruchomienie roweru,
2. utrata komunikacji z HMI podczas pracy,
3. aktualizacja firmware HMI/displaya,
4. aktualizacja firmware kontrolera.

Aktualnie EVistDrive ma kilka problemów architektonicznych:

- może uznać dowolną ramkę CAN skierowaną do kontrolera za dowód, że HMI żyje,
- może zareagować na broadcast `0x3005` używany podczas aktualizacji HMI jak na żądanie restartu kontrolera,
- po `NVIC_SystemReset()` aplikacja wraca do `main()` i ponownie załącza linie zasilania,
- `power_off_controller()` nie jest terminalnym stanem — wykonuje sekwencję OFF i wraca,
- aplikacja startuje z niezerowym domyślnym poziomem assist,
- brak jawnego rozdzielenia stanów:
  - normalny runtime,
  - HMI lost,
  - HMI update,
  - controller update,
  - terminal shutdown.

Najważniejsze założenie:

> NIE ZMIENIAĆ tego, co obecnie dobrze działa w sterowaniu silnikiem.

Nie ruszać bez osobnej potrzeby:

- torque processing,
- Assist,
- Power,
- ACCEL,
- ramp-up,
- ramp-down,
- fast Iq slew,
- PI,
- FOC,
- Hall/theta,
- Quiet Start,
- Quiet Stop,
- ARMED_ZERO,
- normalnej charakterystyki przyspieszania i hamowania.

To zadanie ma dotyczyć **wyłącznie CAN/HMI/power/update lifecycle**.

---

# 2. KONTEKST UŻYTKOWY

## 2.1 Aktualizacja firmware kontrolera

Fabryczne Bafangi po poprawnej aktualizacji zachowują się praktycznie tak, jakby system kończył aktualizację i pozostawał wyłączony do ponownego świadomego uruchomienia.

EVistDrive zachowuje się inaczej:

```text
update
  ↓
reset
  ↓
aplikacja startuje ponownie
  ↓
zasilanie HMI / power-latch zostaje ponownie załączone
  ↓
rower sam wraca do pracy
```

Docelowo ma być:

```text
update
  ↓
verify / zakończenie
  ↓
OFF
  ↓
użytkownik naciska POWER
  ↓
normalny cold boot na nowym firmware
```

## 2.2 Aktualizacja HMI

Podczas aktualizacji displaya DP-C245:

- display przechodzi do bootloadera,
- normalna komunikacja aplikacyjna HMI chwilowo znika,
- po aktualizacji HMI wykonuje restart,
- w niektórych przypadkach po restarcie nie pojawia się prawidłowa komunikacja CAN,
- display pozostaje zasilany z kontrolera i pokazuje błąd komunikacji.

Podejrzenie użytkownika:

> stock G532 prawdopodobnie nie pozostawia systemu w nieskończoność w takim stanie — jeśli HMI po aktualizacji nie wróci do normalnej komunikacji, kontroler powinien zakończyć sesję i się wyłączyć.

Dokładny stockowy timeout nadal wymaga reverse / logu.

---

# 3. POTWIERDZONE FAKTY — CURRENT EVistDrive

## 3.1 CAN RX rozpoznaje pola ID

W `CAN_Display.c`:

```c
Ext_ID_Rx.command   = (receive_message.rx_efid) & 0xFFFF;
Ext_ID_Rx.operation = (receive_message.rx_efid >> 16) & 0x07;
Ext_ID_Rx.target    = (receive_message.rx_efid >> 19) & 0x1F;
Ext_ID_Rx.source    = (receive_message.rx_efid >> 24) & 0x1F;
```

Format 29-bitowego ID:

```text
command   = bits 0..15
operation = bits 16..18
target    = bits 19..23
source    = bits 24..28
```

Znane węzły:

```text
controller = node 2
HMI        = node 3
BESST /
CANable    = node 5
```

## 3.2 Obecny watchdog komunikacji ma zbyt szeroki warunek

Aktualny kod:

```c
if(Ext_ID_Rx.target == 2){
    comm_lost_ticks = 0;
    comm_seen = 1;
    ...
}
```

Komentarz sugeruje `HMI is alive`, ale warunek sprawdza tylko `target == controller`, a nie `source == HMI`.

Skutek: dowolna ramka skierowana do node 2 może resetować watchdog, np.:

- CANable,
- BESST,
- updater,
- diagnostyka,
- konfigurator,
- inny node.

Czyli HMI może być martwe, ale `comm_lost_ticks` ciągle wraca do zera.

To jest błąd logiczny.

## 3.3 Aktualny watchdog ma dwa progi

W `main.c` pierwszy próg odpowiada ok. 3 s braku HMI i zeruje wspomaganie:

```c
MS.assist_level = 0;

motor_command_t comm_stop_command = {
    .iq_target = 0,
    .id_target = MS.i_d_setpoint,
    .enable = true,
    .emergency_stop = false
};

motor_core_set_command(&comm_stop_command);
```

To jest zgodne z kierunkiem ARMED_ZERO i nie należy tego psuć.

Drugi próg odpowiada ok. 10 s i przy `MS.Speedx100 == 0` wywołuje:

```c
power_off_controller();
```

Na pierwszym wdrożeniu można zachować obecne 3 s / 10 s. Dokładne czasy stock G532 można reverse'ować później.

## 3.4 `power_off_controller()` nie jest terminalnym OFF

Aktualnie:

```c
void power_off_controller(void){
    timer_primary_output_config(TIMER0,DISABLE);
    ui_8_PWM_ON_Flag = 0;

    if(!shutdown_saved){
        soc_state_save();
        shutdown_saved = 1;
    }

    GPIO_BC(GPIOB) = GPIO_PIN_4;
    GPIO_BC(GPIOB) = GPIO_PIN_5;
}
```

Funkcja wykonuje sekwencję OFF i wraca do caller-a. Nie istnieje jawny terminalny stan `SHUTDOWN`.

## 3.5 Po starcie aplikacja ponownie załącza linie zasilania

Startup EVistDrive ustawia m.in. PB4/PB5 ON. Typowa ścieżka:

```text
NVIC_SystemReset()
  ↓
main()
  ↓
gpio_config()
  ↓
PB4 ON
PB5 ON
```

może więc powodować ponowne uruchomienie systemu po aktualizacji.

## 3.6 Aktualne wejście w update kontrolera jest zbyt ogólne

W `CAN_Display.c`:

```c
if(Ext_ID_Rx.command == 0x3005){
    NVIC_SystemReset();
}
```

Warunek znajduje się poza głównym `if(Ext_ID_Rx.target==2)` i nie sprawdza:

- target,
- source,
- operation,
- DLC,
- `data[0]`,
- kontekstu sesji update.

To jest poważny błąd ownership.

---

# 4. KLUCZOWY PROBLEM — UPDATE HMI MOŻE RESETOWAĆ CONTROLLER

Reverse protokołu update DPC245 wskazuje, że updater HMI używa broadcastu:

```text
0x85FF3005
```

Przykładowo:

```text
data 00 -> announcement / wejście w update
data 01 -> completion announcement
```

Znana sekwencja HMI obejmuje m.in.:

```text
0x85FF3005  data 00
0x85194000
0x832A4000
0x85196008
0x832A6008
0x85184001
0x832A4001
...
data chunks
...
0x85FF3005  data 01
```

EVistDrive parsuje z tej ramki `command = 0x3005` i obecny kod może wykonać `NVIC_SystemReset()`, mimo że aktualizowany jest display.

To bardzo dobrze pasuje do objawu:

```text
aktualizuję display
  ↓
controller reaguje na 3005
  ↓
reset controller
  ↓
zaburzona sesja CAN / power lifecycle
  ↓
HMI po update wraca w błędzie komunikacji
```

---

# 5. PRIORYTET P0 — NAPRAWIĆ OWNERSHIP `0x3005`

NIE robić już:

```c
if(command == 0x3005)
    NVIC_SystemReset();
```

`0x3005` należy traktować jako element **update protocol state machine**, a nie jako globalny reset.

Po pojawieniu się `0x3005 START` ustaw:

```text
update_announce_seen = true
update_state = UPDATE_DISCOVERY
```

ale NIE resetuj kontrolera.

Dalsze ramki sesji mają ustalić, kto jest aktualizowany:

```text
3005 START
   ↓
UPDATE_DISCOVERY
   ├── handshake adresowany do node 2
   │       ↓
   │   CONTROLLER_UPDATE
   │
   └── handshake adresowany do node 3
           ↓
       HMI_UPDATE
```

Agent ma najpierw potwierdzić z logów / reverse, która kolejna ramka jednoznacznie rozróżnia controller update od HMI update. Nie zgadywać.

---

# 6. PRIORYTET P1 — ROZDZIELIĆ CAN TRAFFIC OD HMI LIVENESS

Trzeba usunąć semantykę:

```text
target==2
=> HMI alive
```

Wprowadzić co najmniej dwa osobne pojęcia.

## A. `hmi_app_alive`

Oznacza: działa normalna aplikacja HMI i istnieje runtime potrzebny do jazdy.

Najlepszy potwierdzony sygnał:

```text
source    = 3
target    = 2
command   = 0x6300
normalna operacja runtime
poprawny DLC
```

`0x6300` niesie m.in. poziom wspomagania, Walk request, światło i przyciski i występuje okresowo, około co 100 ms.

## B. `hmi_update_alive` / `hmi_transport_alive`

Oznacza: HMI może być w bootloaderze / aktualizacji, ale nie ma normalnej aplikacji runtime.

Ten stan NIE może dawać prawa do jazdy, ale może pozwalać utrzymać zasilanie HMI i CAN do zakończenia update.

Proponowane zmienne:

```c
volatile uint16_t hmi_app_lost_ticks;
volatile uint8_t  hmi_app_seen;
volatile uint8_t  ride_authorized;

volatile uint8_t  update_state;
volatile uint8_t  update_owner;
```

Przykład detekcji runtime HMI:

```c
bool is_hmi_runtime_frame =
    Ext_ID_Rx.source == 3 &&
    Ext_ID_Rx.target == 2 &&
    Ext_ID_Rx.command == 0x6300 &&
    receive_message.rx_dlen == 4;
```

Dopiero wtedy:

```c
hmi_app_lost_ticks = 0;
hmi_app_seen = 1;
```

Nie resetować tego watchdog przez source 5, config writes, diagnostykę i inne ramki tylko dlatego, że `target==2`.

---

# 7. PRIORYTET P2 — START INTERLOCK

Aktualnie startup ustawia `MS.assist_level = 2`.

Po cold boot powinno być:

```text
assist_level = 0
ride_authorized = false
```

Docelowo:

```text
POWER ON
  ↓
BOOT_WAIT_HMI
  ↓
assist = 0
ride_authorized = false
  ↓
czekaj na stabilny 0x6300
  ↓
READY
```

EVistDrive ma już wielopróbkowy debounce 0x6300. Zachować go i wykorzystać do przyznania `ride_authorized = true`.

`ride_authorized == false` ma blokować rider demand / napęd, ale nie powinno bez potrzeby wyłączać ADC, Hall, theta tracking, CAN ani ARMED_ZERO.

---

# 8. PRIORYTET P3 — HMI LOST

Docelowo:

```text
READY
  ↓
brak poprawnego 0x6300
  ↓
timeout
  ↓
HMI_LOST
```

Na pierwszym wdrożeniu zachować obecny próg ok. 3 s:

```text
ride_authorized = false
assist_level = 0
Iq target -> 0
```

Nie robić `emergency stop`, jeśli nie ma fault.

Jeżeli HMI nadal nie wróci, a `Speed == 0`, po ok. 10 s przejść do `SYSTEM_SHUTDOWN`.

Jeśli HMI wróci i znów pojawi się stabilna seria 0x6300:

```text
HMI_LOST
  ↓
stable 6300
  ↓
READY
```

bez restartu roweru.

---

# 9. PRIORYTET P4 — HMI UPDATE MUSI BYĆ OSOBNYM STANEM

Podczas aktualizacji HMI normalny 0x6300 zniknie. To jest oczekiwane.

Nie wolno wtedy po normalnym comm timeout wyłączyć HMI, bo można przerwać flashowanie.

Po jednoznacznym rozpoznaniu, że sesja update należy do HMI:

```text
system_state = HMI_UPDATE
```

W tym stanie:

```text
ride_authorized = false
assist_level = 0
Iq target = 0

HMI power = ON
controller power = ON
CAN = ON
bootloader traffic allowed
```

HMI update nie może uruchomić controller bootloader.

Po zakończeniu HMI update:

```text
HMI_UPDATE
  ↓
WAIT_HMI_RECOVERY
```

Czekaj na normalne 0x6300.

Jeśli wróci stabilnie:

```text
READY
```

Jeśli nie wróci w bounded timeout:

```text
SYSTEM_SHUTDOWN
```

Nie pozostawiać na stałe:

```text
display powered
CAN error
controller powered
no normal HMI
```

---

# 10. PRIORYTET P5 — TERMINAL SHUTDOWN

Obecne `power_off_controller()` powinno zostać zastąpione lub opakowane w prawdziwy terminalny shutdown.

Propozycja:

```c
void system_enter_shutdown(shutdown_reason_t reason)
```

Sekwencja:

```text
system_state = SHUTDOWN
ride_authorized = false
normal torque request = 0
Iq -> 0
PWM / MOE -> OFF wg bezpiecznej istniejącej procedury
SOC save once
Display power OFF
DC/DC / power-hold OFF
```

Po wejściu w SHUTDOWN kod NIE może wrócić do:

- ride_control,
- normalnego PWM enable,
- startup processing,
- normalnej obsługi assist,
- ponownego `PB4 ON`,
- ponownego `PB5 ON`.

Jeżeli MCU przez chwilę nadal ma zasilanie:

```c
for(;;){
    keep_power_outputs_off();
    keep_motor_outputs_safe();
    // watchdog handling only if necessary
}
```

Sprawdzić FWDGT, żeby nie powstała sekwencja:

```text
SHUTDOWN
  ↓
watchdog reset
  ↓
main()
  ↓
PB4/PB5 ON
```

---

# 11. PRIORYTET P6 — CONTROLLER UPDATE

Aktualizacja kontrolera wymaga osobnego ownera sesji.

Dopiero gdy:

```text
update_owner == CONTROLLER
```

można wejść w bootloader.

Docelowo:

```text
CONTROLLER_UPDATE
  ↓
bootloader
  ↓
flash
  ↓
verify
  ↓
new app / completion
  ↓
POST_UPDATE_SHUTDOWN
  ↓
OFF
```

Użytkownik ma ponownie nacisnąć POWER.

Jednym z możliwych rozwiązań jest `POST_UPDATE_SHUTDOWN_MAGIC`, zapisany w pamięci przetrzymującej software reset i niekasowanej przez updater.

Przed wyborem miejsca sprawdzić:

- backup registers,
- retained RAM,
- flash config area,
- bootloader-managed marker,
- reset reason registers,
- czy update kasuje dany region.

Nie psuć virtual EEPROM, SOC slots ani config banks.

---

# 12. PROPONOWANA MASZYNA STANÓW

```c
typedef enum {
    SYS_BOOT_WAIT_HMI = 0,
    SYS_READY,
    SYS_HMI_LOST,
    SYS_UPDATE_DISCOVERY,
    SYS_HMI_UPDATE,
    SYS_CONTROLLER_UPDATE,
    SYS_WAIT_HMI_RECOVERY,
    SYS_SHUTDOWN
} system_state_t;
```

Przejścia:

```text
POWER ON
  ↓
BOOT_WAIT_HMI
  ├─ stable 6300 → READY
  └─ brak HMI    → decyzja wg boot timeout
```

```text
READY
  ├─ brak 6300       → HMI_LOST
  ├─ update announce → UPDATE_DISCOVERY
  └─ user power off  → SHUTDOWN
```

```text
HMI_LOST
  ├─ stable 6300           → READY
  └─ timeout + standstill  → SHUTDOWN
```

```text
UPDATE_DISCOVERY
  ├─ owner = HMI        → HMI_UPDATE
  └─ owner = controller → CONTROLLER_UPDATE
```

```text
HMI_UPDATE
  └─ update complete → WAIT_HMI_RECOVERY
```

```text
WAIT_HMI_RECOVERY
  ├─ stable 6300 → READY
  └─ timeout     → SHUTDOWN
```

```text
CONTROLLER_UPDATE
  └─ completion / reboot marker → SHUTDOWN
```

```text
SHUTDOWN
  └─ brak powrotu do runtime
```

---

# 13. RELACJA Z `0x6300`

`0x6300` jest kluczową ramką normalnego runtime HMI.

Znane mapowanie używane w projekcie / reverse obejmuje m.in.:

```text
0x00 = Assist 0
0x0B = Eco / poziom
0x0D = Tour / poziom
0x15 = Sport
0x17 = Sport+
0x03 = Boost
0x06 = Walk
```

Aktualny EVistDrive ma już logikę wielopróbkowej akceptacji `level_code`. Nie przebudowywać mapowania assist w ramach tego zadania, jeśli audyt nie wykaże osobnego błędu.

---

# 14. WALK ASSIST

Obecna logika Walk ma debounce dla:

```text
0x6300[1] == 0x06
```

Nie ruszać go bez potrzeby.

Nowe wymaganie:

```text
Walk może działać tylko gdy ride_authorized == true
```

i system nie jest w:

- HMI_LOST,
- HMI_UPDATE,
- CONTROLLER_UPDATE,
- SHUTDOWN.

---

# 15. AUTO-OFF `0x6303`

`0x6303` niesie ustawienie Auto-Off.

Aktualny kod zapisuje:

```c
auto_off_minutes = receive_message.rx_data[0];
```

Nie traktować 0x6303 jako podstawowego proof-of-life dla normalnej jazdy. Podstawą `hmi_app_alive` powinien być 0x6300.

---

# 16. CZEGO NIE UŻYWAĆ JAKO PEWNY HMI HEARTBEAT

Nie używać automatycznie `0x82FF1200` jako HMI heartbeat.

W nowszym zweryfikowanym decode jest to ramka z kontrolera / broadcast status-event, a nie prosty sygnał `HMI -> controller alive`.

---

# 17. STOCK G532 — CO JEST PEWNE, A CO NIE

## Pewne / silnie potwierdzone

- HMI normalnie utrzymuje runtime state przez 0x6300,
- 0x6300 jest stanem utrzymywanym, a nie pojedynczym one-shot,
- nowy stan jest stabilizowany / debounce'owany,
- update HMI korzysta z broadcastu 0x3005,
- HMI update ma własny handshake i własny owner,
- EVistDrive obecnie zbyt szeroko interpretuje 0x3005.

## Nadal niepewne

Nie wpisywać jako factory fact, dopóki nie zostanie potwierdzone:

- dokładny stock G532 timeout braku HMI,
- czy stock po HMI update wyłącza system po dokładnie X sekundach,
- dokładna logika resident bootloadera kontrolera po zakończeniu flash,
- dokładny stockowy marker update-complete,
- dokładne GPIO / latch ownership w stock G532,
- czy stock używa reset reason / backup register / RAM marker / bootloader flag do post-update OFF.

---

# 18. DALSZY TARGETED REVERSE G532

Jeśli potrzebne będzie dokładniejsze odwzorowanie stock, wykonać wąski reverse bez ponownego rozbierania FOC.

Szukamy:

## 18.1 HMI age / timeout

Znaleźć odbiór 0x6300, potem timestamp / age / counter storage i wszystkie xrefy.

Odpowiedzieć:

```text
po ilu ms bez HMI:
- zabraniany jest assist?
- Iq przechodzi do 0?
- następuje power-off?
```

## 18.2 Consumer 0x6303

Ustalić, czy 0x6303 tylko przechowuje Auto-Off, czy bierze udział w shutdown lifecycle.

## 18.3 Power latch

Zidentyfikować stockowe GPIO odpowiadające za:

```text
display power
DC/DC hold
self-power latch
```

oraz callerów:

- power button,
- auto-off,
- HMI lost,
- update complete,
- fault,
- bootloader.

## 18.4 Update completion

W stock binary / bootloader szukać:

- 0x3005,
- update state,
- jump-to-bootloader,
- post-flash reset,
- retained marker,
- backup register,
- reset reason,
- power-latch release.

---

# 19. BRAKUJĄCE DANE — BOOTLOADER

Jeżeli posiadany G532 BIN zawiera tylko aplikację, pełne zachowanie stock update może wymagać dumpu resident bootloadera.

Raportować wyniki jako:

```text
CONFIRMED
STRONG INFERENCE
UNKNOWN / NEEDS BOOTLOADER DUMP
```

---

# 20. TESTY BEZ OSCYLOSKOPU

Użytkownik testuje głównie przez CAN logging i zachowanie roweru.

## Test A — normalny boot

Sprawdzić:

- moment pierwszego 6300,
- liczbę próbek do READY,
- czy przed READY assist = 0,
- czy użytkowo nie powstaje zbędne opóźnienie.

PASS:

```text
brak możliwości napędu przed prawidłowym HMI
normalna jazda po READY
```

## Test B — odpięcie HMI / CAN podczas jazdy

Sprawdzić:

- czas do Iq -> 0,
- brak gwałtownego hard stop,
- czas do shutdown przy postoju.

## Test C — CANable przy martwym HMI

Przy braku HMI wysyłać source 5 do node 2.

PASS:

```text
source 5 nie resetuje hmi_app_lost_ticks
```

## Test D — udany update HMI

Logować od przed 0x3005 data 00 do po restarcie displaya.

PASS:

```text
controller nie resetuje się na HMI 3005
display ma zasilanie przez flash
ride_authorized=false
po normalnym 6300 system wraca do READY
```

## Test E — HMI nie wraca po update

Symulować:

```text
update complete
HMI reset
brak normalnego 6300
```

PASS:

```text
bounded recovery timeout
potem terminal shutdown
```

## Test F — update kontrolera

PASS:

```text
HMI update nie uruchamia controller update
controller update nadal działa
po zakończeniu system OFF
```

## Test G — zwykłe POWER OFF

Sprawdzić:

- długie POWER nadal działa,
- SOC zapisuje się raz,
- brak watchdog reboot,
- display gaśnie,
- system nie wraca.

## Test H — Auto-Off

Sprawdzić 0x6303, timeout, terminal shutdown i brak samoczynnego restartu.

---

# 21. LOGOWANIE DIAGNOSTYCZNE

Na czas wdrażania dodać lekki log zmian stanów.

Minimum:

```text
SYS_STATE
RIDE_AUTH
HMI_APP_SEEN
HMI_APP_AGE
UPDATE_STATE
UPDATE_OWNER
SHUTDOWN_REASON
LAST_CAN_SOURCE
LAST_CAN_TARGET
LAST_CAN_COMMAND
RESET_REASON
```

Nie floodować fast loop.

Przykłady:

```text
STATE BOOT_WAIT_HMI -> READY
reason: stable_6300
```

```text
STATE READY -> HMI_LOST
age_ms: 3000
```

```text
UPDATE_DISCOVERY -> HMI_UPDATE
owner: node3
```

```text
SHUTDOWN reason=HMI_RECOVERY_TIMEOUT
```

---

# 22. SUGEROWANE ENUMY

```c
typedef enum {
    UPDATE_NONE = 0,
    UPDATE_DISCOVERY,
    UPDATE_HMI,
    UPDATE_CONTROLLER,
    UPDATE_WAIT_HMI_RECOVERY
} update_state_t;

typedef enum {
    UPDATE_OWNER_NONE = 0,
    UPDATE_OWNER_HMI = 3,
    UPDATE_OWNER_CONTROLLER = 2
} update_owner_t;

typedef enum {
    SHUTDOWN_USER_POWER = 0,
    SHUTDOWN_AUTO_OFF,
    SHUTDOWN_HMI_LOST,
    SHUTDOWN_HMI_UPDATE_RECOVERY_FAIL,
    SHUTDOWN_AFTER_CONTROLLER_UPDATE,
    SHUTDOWN_FAULT
} shutdown_reason_t;
```

---

# 23. BEZPIECZNE MIEJSCE NA `ride_authorized`

Nie dodawać drugiej równoległej rampy.

Najlepiej gate'ować wysoko w chain:

```text
rider demand
  ↓
assist request
  ↓
ride_authorized ?
   ├─ YES -> normal pipeline
   └─ NO  -> requested torque = 0
```

A dalej zachować istniejące:

```text
limits
shaping
fast Iq slew
motor_core_set_command
FOC
```

---

# 24. NIE DODAWAĆ DRUGIEGO OWNERA Iq

Nie tworzyć równoległych ownerów:

```text
normal pipeline writes Iq
+
HMI watchdog writes Iq
+
update code writes Iq
+
shutdown writes Iq
```

Preferowany kierunek:

```text
system_state / ride_authorized
  ↓
normalny jeden pipeline
  ↓
final Iq owner
```

Wyjątek: terminalny shutdown może bezpośrednio wymusić safe-off.

---

# 25. INTERAKCJA Z ARMED_ZERO

Normalne `assist=0` nie oznacza system shutdown.

Przy zdrowym HMI i w normalnym READY:

```text
Iq target = 0
FOC / ADC / Hall / theta mogą pozostać alive
MOE może pozostać zgodnie z ARMED_ZERO lifecycle
```

Nie cofać tej architektury.

---

# 26. INTERAKCJA Z QUIET STOP

Po HMI_LOST zejście do zero torque powinno używać tej samej bezpiecznej drogi co normalny brak demand.

Nie robić:

```text
HMI timeout -> hard PWM OFF natychmiast
```

chyba że wystąpi prawdziwy fault.

---

# 27. INTERAKCJA Z UPDATE

W czasie update:

```text
ride_authorized = false
```

ale to nie oznacza automatycznie `power_off_controller()`.

Rozdzielić:

```text
NO RIDE
```

od:

```text
SYSTEM OFF
```

HMI_UPDATE:

```text
NO RIDE
POWER ON
CAN ON
HMI ON
```

SHUTDOWN:

```text
NO RIDE
PWM OFF
HMI OFF
POWER HOLD OFF
terminal
```

---

# 28. CO ZMIENI SIĘ DLA UŻYTKOWNIKA

## Normalna jazda

Praktycznie nic.

Nie powinny zmienić się:

- reakcja na nacisk,
- start,
- płynność,
- acceleration,
- deceleration,
- Quiet Start,
- Quiet Stop,
- Assist,
- Power.

## Uruchomienie

Po POWER system poczeka na prawidłowe HMI przed przyznaniem prawa do jazdy.

## Utrata HMI

Po poprawce tylko prawidłowy runtime HMI będzie podtrzymywał ride permission.

## Aktualizacja HMI

Po poprawce controller nie będzie resetował się na sam broadcast 3005; napęd będzie zablokowany, ale HMI zachowa zasilanie do końca flashowania.

## HMI nie wróci po update

Po bounded recovery system przejdzie do OFF.

## Aktualizacja kontrolera

Po update controller ma zakończyć w OFF. Użytkownik uruchamia rower ponownie przyciskiem POWER.

---

# 29. PRIORYTETY WDROŻENIA

## P0 — krytyczne
Naprawić `0x3005` ownership.

## P1
Naprawić HMI liveness: nie `target==2`, tylko prawidłowy source3 runtime 6300.

## P2
Boot interlock: `assist=0`, `ride_authorized=false` do stabilnego HMI.

## P3
Wprowadzić `HMI_UPDATE`.

## P4
Zrobić terminalny `SYSTEM_SHUTDOWN`.

## P5
Zrobić poprawny post-controller-update OFF.

## P6
Dodać logi i przeprowadzić pełny test matrix.

---

# 30. PLAN PRACY DLA AGENTA

## Etap 1 — audit only

Najpierw NIE zmieniaj kodu.

Zrób listę:

1. wszystkie write do:
   - `comm_lost_ticks`
   - `comm_seen`
   - `assist_level`
   - `walk_can_request`
   - `power_off_controller`
   - PB4
   - PB5

2. wszystkie consumer/caller:
   - `0x3005`
   - `0x6300`
   - `0x6303`

3. wszystkie:
   - `NVIC_SystemReset`
   - bootloader jump
   - reset reason reads
   - watchdog reset/reload logic

4. wszystkie miejsca mogące ponownie:
   - enable PWM,
   - enable power latch,
   - enable HMI power.

Raport bez modyfikacji.

## Etap 2 — P0 patch

Napraw tylko `0x3005`.

Warunki sukcesu:

```text
HMI update nie resetuje controller
controller update nadal możliwy
```

## Etap 3 — liveness split

Wprowadź `hmi_app_alive` oparte na source3 runtime.

Test:

```text
source5 nie resetuje HMI watchdog
```

## Etap 4 — ride authorization

Dodaj `ride_authorized` i boot wait.

Nie twórz drugiego Iq ownera.

## Etap 5 — HMI update state

Rozpoznaj update owner. Zablokuj jazdę podczas HMI update, ale zachowaj zasilanie.

## Etap 6 — terminal shutdown

Przebuduj `power_off_controller()` / dodaj `system_enter_shutdown`.

## Etap 7 — controller post-update OFF

Dopiero gdy poprzednie kroki są stabilne.

---

# 31. ACCEPTANCE CRITERIA

Zmiana jest zaakceptowana dopiero gdy:

- AC-1: normalna jazda ma ten sam feeling,
- AC-2: HMI update nie resetuje controller na samym broadcast 0x3005,
- AC-3: CANable / BESST nie może udawać `HMI alive`,
- AC-4: bez prawidłowego HMI nie można rozpocząć wspomagania,
- AC-5: po utracie HMI napęd schodzi bezpiecznie do zero,
- AC-6: po powrocie HMI możliwy jest recovery bez restartu,
- AC-7: podczas HMI update display pozostaje zasilany aż do zakończenia procedury,
- AC-8: jeśli HMI po update nie wraca, system nie pozostaje na stałe w powered-error,
- AC-9: terminal shutdown nie może powrócić do normalnego main loop,
- AC-10: controller firmware update kończy się OFF,
- AC-11: SOC zapisuje się dokładnie raz,
- AC-12: nie powstaje nowy boczny owner finalnego Iq.

---

# 32. REGRESJE, KTÓRYCH NIE WOLNO WPROWADZIĆ

Nie zaakceptować patcha, jeżeli:

- zmieni ramp-up/down,
- zmieni reakcję torque,
- pogorszy Quiet Stop,
- przywróci klik przy starcie/stop,
- wyłączy ARMED_ZERO w normalnym assist=0,
- doda drugi Iq slew,
- doda bezpośrednie write finalnego Iq w wielu modułach,
- HMI update traci zasilanie przed zakończeniem,
- controller resetuje się na broadcast HMI update,
- watchdog powoduje boot-loop po shutdown.

---

# 33. MINIMALNY PIERWSZY PATCH

Jeżeli potrzebny jest bardzo mały i bezpieczny krok 1:

1. usunąć bezwarunkowy:
   ```c
   if(command == 0x3005) NVIC_SystemReset();
   ```

2. dodać log:
   ```text
   RX 3005 source/target/op/dlc/data0
   ```

3. NIE wchodzić jeszcze w bootloader automatycznie,

4. zrobić dwa logi:
   - update HMI,
   - update controller,

5. na podstawie różnicy potwierdzić jednoznaczny owner session,

6. dopiero potem przywrócić controller-update entry na poprawnym warunku.

---

# 34. KOŃCOWA ZASADA ARCHITEKTONICZNA

Docelowo EVistDrive powinien rozróżniać:

```text
CAN BUS ALIVE
HMI TRANSPORT ALIVE
HMI APPLICATION ALIVE
RIDE AUTHORIZED
SYSTEM POWERED
```

To NIE są synonimy.

### HMI update

```text
CAN BUS ALIVE        = YES
HMI TRANSPORT ALIVE  = YES
HMI APP ALIVE        = NO
RIDE AUTHORIZED      = NO
SYSTEM POWERED       = YES
```

### Normalna jazda

```text
CAN BUS ALIVE        = YES
HMI TRANSPORT ALIVE  = YES
HMI APP ALIVE        = YES
RIDE AUTHORIZED      = YES
SYSTEM POWERED       = YES
```

### HMI lost przed shutdown

```text
CAN BUS ALIVE        = MAYBE
HMI TRANSPORT ALIVE  = NO
HMI APP ALIVE        = NO
RIDE AUTHORIZED      = NO
SYSTEM POWERED       = YES
```

### Terminal OFF

```text
RIDE AUTHORIZED      = NO
MOTOR OUTPUT         = SAFE OFF
HMI POWER            = OFF
POWER HOLD           = OFF
SYSTEM STATE          = SHUTDOWN
```

---

# 35. PODSUMOWANIE DLA AGENTA

Najbardziej prawdopodobny obecny root cause problemu podczas aktualizacji HMI nie leży w FOC ani w motor control.

Leży w CAN/update lifecycle:

```text
HMI updater wysyła broadcast 0x3005
        ↓
EVistDrive sprawdza tylko command==0x3005
        ↓
controller robi NVIC_SystemReset()
        ↓
startup ponownie załącza power/display
        ↓
sesja update / recovery jest zaburzona
```

Drugi root cause:

```text
target==2
        ↓
comm_lost_ticks=0
        ↓
dowolny node może udawać HMI
```

Trzeci root cause:

```text
power_off_controller()
        ↓
GPIO OFF
        ↓
return
```

czyli brak terminalnego shutdown lifecycle.

Poprawić te trzy elementy w pierwszej kolejności.

Nie ruszać charakterystyki jazdy.

---

# 36. OCZEKIWANY EFEKT KOŃCOWY

Po wdrożeniu użytkownik powinien dostać:

- normalną jazdę bez zmiany feelingu,
- brak samoczynnego resetowania kontrolera podczas update HMI,
- bezpieczne odcięcie napędu po prawdziwej utracie HMI,
- automatyczny recovery po chwilowym zaniku HMI,
- poprawne utrzymanie zasilania HMI w trakcie flashowania,
- wyłączenie systemu, jeśli HMI po update nie wróci,
- controller update kończący się OFF,
- brak samoczynnego restartu roweru po firmware update,
- jednoznaczny i audytowalny lifecycle z jasnym ownership stanów.
