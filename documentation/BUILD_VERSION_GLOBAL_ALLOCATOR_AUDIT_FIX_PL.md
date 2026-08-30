# Globalny alokator wersji kanonicznych

## Przyczyna i migracja

Poprzedni alokator w `scripts/build-firmware.ps1` używał
`<repoRoot>/.local/build-number.txt`. Jest to plik ignorowany przez Git i należący do pojedynczego
worktree, więc dwa worktree mogły jednocześnie odczytać stary licznik i wydać ten sam albo
niższy numer. To wyjaśnia testowe, niemonotoniczne `0.0412/0.0413` po kanonicznym `0.0459`.

HWM migracji to `0.0459`: dokument QS-1 rejestruje NORMAL `0.0458` i DIAG `0.0459`; raporty
FW-129B rejestrują tylko `0.0454/0.0455`, a FW-130A `0.0456/0.0457`. `0.0412/0.0413` pozostają
historycznymi artefaktami TEST-ONLY/NON-MONOTONIC i nie obniżają HWM.

## Nowa architektura

`scripts/build-version-allocator.psm1` trzyma stan `M820_BL820.json` w
`<parent-repo>/.ebics-version-state` (lub w `EBICS_VERSION_STATE_ROOT` / `-VersionStateRoot`).
Katalog jest wspólny dla worktree i gałęzi, ale poza katalogiem każdego worktree. Pierwsza
migracja zapisuje HWM 459. Atomowe `New-Item -ItemType Directory` tworzy lock; pod lockiem
odczytywany jest HWM, zapisywany nowy HWM i zwalniany lock. Dlatego rezerwacja jest natychmiast
widoczna w każdym worktree. Timeout locka zatrzymuje AUTO przed buildem.

AUTO (`-BuildMode Auto`) rezerwuje numer przed kompilacją i drukuje VERSION PRECHECK.
Nieudany build nie zwraca numeru. REPRO wymaga `-BuildMode Repro -Version <historyczny>` i nie
zmienia HWM. Developer używa wyłącznie `DEV-NONCANONICAL`. AUTO odrzuca jawny numer.

Test `tests/host/build_version_allocator_host.ps1` używa losowego katalogu TEMP: sprawdza
migrację 0.0459, sekwencję, parę 0.0461/0.0462 z drugiego worktree oraz cztery równoległe
rezerwacje bez duplikatu. Sprawdza też fail-closed dla braku/uszkodzenia JSON, obcej wersji
schematu lub targetu, HWM poniżej 0.0459 i zajętego locka. Nie zużywa prawdziwej wersji ani nie
wykonuje builda firmware.

## C4: stan rzeczywisty i para kanoniczna

Jednorazowa, jawna migracja została wykonana do wspólnego stanu
`C:\Projekty\EBICS\.ebics-version-state\M820_BL820.json` z HWM `0.0459`.
Obok zapisany jest marker `M820_BL820.migration.json`. Ponowne automatyczne utworzenie stanu jest
zabronione: brak albo nieczytelny JSON zatrzymuje AUTO przed rezerwacją. Migracja nie wydała
`0.0460` ani żadnego kolejnego numeru.

Oficjalne wejście do wydania pary to:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-canonical-pair.ps1
```

Skrypt pod jednym lockiem najpierw rezerwuje dokładnie dwa kolejne numery: pierwszy dla NORMAL,
drugi dla DIAG. Dopiero potem buduje oba warianty w trybie `Reserved`. Niepowodzenie NORMAL lub
DIAG nie zwalnia numerów. `-StateRoot` i `-OutputDir` służą do izolacji testów.

Po buildzie `Test-EbicsVersionIdentity` wymaga zgodności numeru w wygenerowanym
`build_version.h`, manifeście i nazwie opublikowanego BIN. Niezgodność jest błędem i pozostawia
rezerwację zużytą; test hosta zawiera celowo błędny manifest i potwierdza odrzucenie.

Zweryfikowano w izolowanym stanie: pojedynczą parę `0.9201` NORMAL / `0.9202` DIAG oraz dwa
równoległe orchestratory: `0.9301/0.9302` i `0.9303/0.9304`, bez duplikatu i bez przeplotu par.
REPRO `0.8123` przeszedł pełny build bez HWM, a Developer wydał wyłącznie
`DEV-NONCANONICAL` i nie utworzył stanu HWM.
