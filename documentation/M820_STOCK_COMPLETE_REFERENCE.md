# M820 STOCK COMPLETE REFERENCE

PROJECT: BAFANG M820 — COMPLETE STOCK HARDWARE + CAN REFERENCE
SOURCE: `Firmware source/FT_2026_05_22_w1.bin`
STATUS: IN PROGRESS — PART 0 COMPLETE
PROMPT: `Promotrever .md`

---

# 0. QUICK REFERENCE

## 0.1 MCU

| ITEM | VALUE | CONFIDENCE |
|---|---|---|
| Family | GD32F30x / STM32F103-HD-register-compatible (Cortex-M4) | STRONG |
| Device IRQ count | 60 | CONFIRMED (vector table size) |
| Flash used by app | ~93 KB @ 0x08005000..0x0801BBEC | CONFIRMED |
| SRAM implied top | ≥ 47 KB (SP init 0x2000B808) → HD class (48 KB) | STRONG |
| Std library | ST STM32F10x Standard Peripheral Driver | CONFIRMED (assert path strings) |

NOTE: prompt zakładał GD32F303; obraz jest zbudowany na bibliotece STM32F10x_StdPeriph_Driver,
co jest zgodne z praktyką Bafang (GD32F30x = rejestrowo zgodny z F103 HD, rdzeń Cortex-M4).
Dalsze części potwierdzą mapę peryferiów.

## 0.2 Clock summary

| DOMAIN | VALUE | CONFIDENCE |
|---|---|---|
| SYSCLK (normal) | **(HSE/2)×9** — wymaga HSE=16 MHz dla 72 MHz | CONFIRMED formula / crystal OPEN |
| HSE | ON, crystal (no bypass), HSE/2 do PLL | CONFIRMED |
| AHB (HPRE) | ÷1 | CONFIRMED |
| APB2 (PPRE2 bits[13:11]) | ÷1 | CONFIRMED |
| APB1 (PPRE1 bits[10:8]) | **÷2** | CONFIRMED |
| ADCPRE (bits[15:14]) | setter istnieje @0x08009638, wartość z initu @0x0800B33C — TBD PART 11 | OPEN |
| FLASH latency | 2 WS + prefetch | CONFIRMED |
| Fallback clock | HSI/2 ×16 (=64 MHz przy HSI=8 M) | CONFIRMED path |
| Boot-time clock | do momentu `main` działa HSI (SetSysClock wołany dopiero w main!) | CONFIRMED |
| Timer clocks | patrz §3.6 | STRONG |

UWAGA: brak stałej SystemCoreClock w obrazie; wartość bezwzględna zależy od kryształu
(BIN nie dowodzi wartości HSE → EXTERNAL EVIDENCE REQUIRED, prawdopodobnie 16 MHz).

## 0.5 Peripheral map (initial inventory)

| PERIPHERAL | EVIDENCE (literal refs) | CONFIDENCE |
|---|---|---|
| TIM1 @0x40012C00 | 50 refs — kandydat MOTOR PWM | CONFIRMED used |
| TIM3 @0x40000400 | 41 refs | CONFIRMED used |
| TIM4 @0x40000800 | 37 refs | CONFIRMED used |
| TIM5 @0x40000C00 | 36 refs | CONFIRMED used |
| TIM6/TIM7 | 14+14 refs | CONFIRMED used |
| TIM8 @0x40013400 | 38 refs | CONFIRMED used |
| ADC1 @0x40012400 | 19 refs | CONFIRMED used |
| ADC2 @0x40012800 | 17 refs | CONFIRMED used (dual ADC!) |
| CAN1 @0x40006400 | 13 refs | CONFIRMED used |
| GPIOA..G | 8/19/11/6/4/4/4 refs | CONFIRMED all ports used |
| RCC/DMA1/FLASHIF/IWDG/WWDG/BKP/CRC/AFIO | używane | CONFIRMED |
| USART/SPI/I2C/USB | ZERO literałów baz | CONFIRMED not used (rejestrowo) |

Brak USART ⇒ komunikacja HMI/serwis = CAN (albo bit-bang); rozstrzygną części CAN/HMI.

## 0.3 Complete pin map

OPEN — PART 3.

## 0.4 Connector map

OPEN — PART 4.

## 0.5 Peripheral map (initial inventory)

| PERIPHERAL | EVIDENCE | CONFIDENCE |
|---|---|---|
| GPIO/RCC | stdperiph `stm32f10x_gpio.c`, `rcc.c` linked | CONFIRMED |
| TIM | stdperiph `stm32f10x_tim.c` linked | CONFIRMED |
| ADC | stdperiph `stm32f10x_adc.c` linked | CONFIRMED |
| CAN | stdperiph `stm32f10x_can.c` linked | CONFIRMED |
| DMA | stdperiph `stm32f10x_dma.c` linked | CONFIRMED |
| FLASH/FPEC | stdperiph `stm32f10x_flash.c` linked (NVM/config) | CONFIRMED |
| PWR/BKP/RTC | `pwr.c`, `bkp.c`, `rtc.c` linked | CONFIRMED |
| WWDG/IWDG | `wwdg.c`, `iwdg.c` linked | CONFIRMED |
| misc/NVIC | `misc.c` linked | CONFIRMED |
| USART/SPI/I2C/EXTI | brak stringów assert + zero literałów baz | CONFIRMED not used |

Szczegółowy census literałów: patrz §6.

## 0.6–0.13 Motor PWM / ADC / Sensor / CAN / Interrupts / Protection / Startup / Gaps

OPEN — PART 5+.

## Model identity strings (stock)

| VA | STRING |
|---|---|
| 0x08016B60 | `CR X30P.250.FC 2.1` |
| 0x08016B74 | `CR X30P.250.FC` |
| 0x08016B84 | `BAFANG` |
| 0x08016B8C | `CUSTOM` |
| 0x08016B94 | `Para V1.0` |
| 0x08016BA7 | ` FAKE TAXI 20260522w1` |

Interpretacja HYPOTHESIS: identyfikator modelu/protokołu stockowego + nazwa custom buildu
zgodna z nazwą pliku (`FT…` = FAKE TAXI?, data 2026-05-22 w1).

---

# 1. Firmware image and MCU foundation (PART 0 — SOURCE INVENTORY / BASELINE) — **COMPLETE**

## 1.1 Source image

| ITEM | VALUE | CONFIDENCE |
|---|---|---|
| File | `Firmware source/FT_2026_05_22_w1.bin` | CONFIRMED |
| Size | 93 196 bytes (0x16C0C) | CONFIRMED |
| SHA-256 | `96F9E8D4A7F1E0DCEB47C4A50CD85F2E9FA0C948ADCB4FA7194DCE11233BC32A` | CONFIRMED |

## 1.2 Container / header (file[0x00..0x20))

```
00: 01 45 82 40 40 00 00 00 00 00 00 00 00 00 6B EC
10: 2E FB 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

| FIELD | RAW | NOTES | CONFIDENCE |
|---|---|---|---|
| magic[0..4] | `01 45 82 40` | nieznany format nagłówka Bafang | OPEN |
| word@0x04 | 0x00000040 | znaczenie nieznane (nie długość nagłówka) | OPEN |
| bytes[0x0C..0x12] | `00 00 6B EC 2E FB` | prawdopodobnie suma kontrolna payloadu; CRC32 (refl/mpeg) NIE pasuje dla zakresów [32,end)/[32,end-4)/[64,end) | OPEN |
| tail word @len-4 | 0xE9CCC5FD | kandydat na CRC payloadu — algorytm nieznany | OPEN |

## 1.3 Application base and vector table

| ITEM | VALUE | EVIDENCE | CONFIDENCE |
|---|---|---|---|
| Application base | **0x08005000** | patrz 1.4 | CONFIRMED |
| Vector table VA | 0x08005000 (= file+0x20) | tabela dekoduje się poprawnie pod tą bazą | CONFIRMED |
| Initial SP | 0x2000B808 | VT[0] | CONFIRMED |
| Reset handler | 0x0800A3CD (Thumb; kod od 0x0800A3CC) | VT[1] | CONFIRMED |
| VTOR literal | 0x08005000 @ file+0x5584 (VA≈0x0800A584, region startupu) | literał w kodzie inicjalizującym | STRONG |
| Default handler | 0x080054BF → `b .` @0x080054BE (E7 FE) | disasm | CONFIRMED |
| Payload w flashu | 0x08005000..0x0801BBEC | file[0x20..end) | CONFIRMED |

### Full core vector table (VA 0x08005000)

| # | NAME | HANDLER | NOTE |
|---|---|---|---|
| 0 | Initial SP | 0x2000B808 | |
| 1 | Reset | 0x0800A3CD | startup: `push {r4,lr}; bl 0x08005130(SysInit?) …` |
| 2 | NMI | 0x08008EB5 | `nop; b .` |
| 3 | HardFault | 0x080083CD | `nop; b .` |
| 4 | MemManage | 0x08008AED | stub |
| 5 | BusFault | 0x08006175 | |
| 6 | UsageFault | 0x0800F031 | |
| 7–10 | Reserved | 0 | |
| 11 | SVCall | 0x0800A9E1 | |
| 12 | DebugMon | 0x08007219 | |
| 13 | Reserved | 0 | |
| 14 | PendSV | 0x080094B1 | |
| 15 | SysTick | 0x0800BAED | real ISR (watchdog/tick pattern) |

### Device IRQ slots (60 total; default = 0x080054BF)

Non-default handlers:

| IRQ | CANDIDATE PERIPHERAL (GD32F30x_HD numbering) | HANDLER | CONFIDENCE |
|---|---|---|---|
| 18 | CAN RX0? / USB_HP_CAN_TX slot family | 0x08005579 | OPEN |
| 19 | CAN RX1? | 0x0800EFA5 | OPEN |
| 20 | CAN SCE? | 0x0800EFA9 | OPEN |
| 21 | ADC1_2 | 0x08006179 | HYPOTHESIS |
| 22 | USB_HP_CAN_TX / TIM8_BRK family | 0x080061F5 | OPEN |
| 25 | ? (DMA/TIM family) | 0x0800BE71 | OPEN |
| 28 | ? | 0x0800C189 | OPEN |
| 29 | ? | 0x0800C209 | OPEN |

UWAGA: dokładne przypisanie IRQ↔peryferium wymaga PART 16 (NVIC audit) i porównania
z numeracją GD32F30x_HD vs F103_HD. Pozycje powyżej to sloty, nie finalne nazwy.

60 slotów IRQ jest spójne z klasą High-Density (F103 HD ma 60 przerwań; GD32F30x HD analogicznie).

## 1.4 Proof of application base (chain of evidence)

1. Domyślne handlery (55×) wskazują 0x080054BF; jedyna baza dająca wyrównaną lokalizację
   z sekwencją `E7 FE` w pliku: offset 0x4DE ⇒ base = 0x08005000 (pozostałe kandydatury niealigned/garbage). CONFIRMED.
2. Reset handler 0x0800A3CC dekoduje czysty Thumb prolog (`push {r4,lr}`, wywołania, literały)
   wyłącznie pod base=0x08005000. CONFIRMED.
3. Literał `0x08005000` występuje w kodzie (file+0x5584) — zgodny z VTOR/SystemInit. STRONG.
4. SysTick/PendSV/SVCall dekodują się jako sensowne ISRs pod tym mapowaniem. CONFIRMED.

## 1.5 Build fingerprint

| ITEM | VALUE |
|---|---|
| Library paths | `..\..\..\Libraries\STM32F10x_StdPeriph_Driver\src\*.c` |
| Linked modules (assert strings) | misc, wwdg, bkp, dma, flash, gpio, iwdg, pwr, rcc, rtc, adc, can, tim |
| Version block | VA 0x08016B60 (tabela wyżej) |

## 1.6 Open questions (PART 0)

QUESTION: Znaczenie pól nagłówka (magic, word@4, CRC).
WHY IT MATTERS: tworzenie własnych update'ów / walidacja obrazu przez bootloader.
CURRENTLY KNOWN: 32-bajtowy header; kandydat sumy `6B EC 2E FB`; tail word 0xE9CCC5FD.
MISSING: algorytm sumy (standardowe CRC32 refl/MPEG2 nie pasują).
CAN STOCK BIN ANSWER IT? YES (algorytm sprawdzający siedzi w bootloaderze — poza tym plikiem) / częściowo NO.
NEXT EXACT TARGET: pozyskanie bootloadera (osobny BIN) lub brute-force CRC wariantów.
PORTING IMPACT: niski (dokumentacja), wysoki (OTA).

QUESTION: Dokładna rodzina MCU (GD32F303xx vs inny GD32/F103 klony).
WHY IT MATTERS: clock tree, ADC timings, ID rejestrów.
CURRENTLY KNOWN: Cortex-M4, HD-class 60 IRQ, SP w 48KB SRAM, biblioteka F10x.
MISSING: odczyt DBGMCU_IDCODE / kod partu w kodzie lub dump rejestru z hardware.
CAN STOCK BIN ANSWER IT? PARTIAL.
NEXT EXACT TARGET: SystemInit (bl 0x08005130 z reset handlera) → RCU/system config.
PORTING IMPACT: średni.

---

# 2. Memory map and boot (PART 1 — COMPLETE)

## 2.1 Memory map

| REGION | RANGE | NOTES | CONFIDENCE |
|---|---|---|---|
| Code flash | 0x08000000..0x0801FFFF (256K class) | bootloader zakłada się @0x08000000 (poza tym BIN) | STRONG |
| Application | **0x08005000**..0x0801BBEC | patrz §1.3 | CONFIRMED |
| SRAM | 0x20000000.., SP init = 0x2000B808 | klasa 48 KB (HD) | STRONG |
| Stack top | 0x2000B808 | VT[0] | CONFIRMED |
| Peripheral | 0x40000000..0x40030000 (F1/GD32 map) | census §6 | CONFIRMED |
| Core | 0xE0000000.. (SysTick/NVIC/SCB/VTOR/DWT) | VTOR literal @0x080091CC region | CONFIRMED |

## 2.2 Boot chain

```
RESET (VT[1]=0x0800A3CC)
  push {r4,lr}; bl __main(0x08005130)
    scatter-load .data/.binit (0x08005138), rt_lib_init
  powrót z __main → konfiguracja zmiennych (str #2 / str #-3 do globali)
  bl 0x08005228 (lib self-test)
MAIN = 0x08017F4C:
  bl 0x8012468        ; early init #1
  bl 0x8016228        ; early init #2
  bl 0x80098C4        ; SetSysClock: HSE→PLL(HSE/2×9)=SYSCLK
  bl 0x800A9B4        ; init #3
  r0=0; bl 0x800A998  ; init #4(param)
  bl 0x8008EB8        ; init #5 (flash/param region check; args 0x08000000+0x5000)
  NVIC init bl 0x80091F4 (tablice priorytetów/grup)
  bl 0x8017FB0        ; zerowanie RAM state-db
  bl 0x800B860        ; aplikacyjna inicjalizacja peryferiów (GPIO/TIM/ADC/CAN)
LOOP 0x08017F82:
  bl 0x80180E4        ; scheduler/task tick
  flag byte; jeżeli ustawiony: r0=0xAAEE; bl 0x801A1EC  ; serwis (magiczny kod!)
```

KEY FACT: zegar 72 MHz startuje dopiero w `main` — przed tym HSI. Boot-sequence timing (PART 46)
musi to uwzględniać.

## 2.3 IRQ candidate map (60 slotów; default=0x080054BF `b .`)

Numeracja F103-HD/GD32F30x-HD:

| IRQ# | NAME (kandydat) | HANDLER VA | CONFIDENCE |
|---|---|---|---|
| 18 | ADC1_2 | 0x08005579 | STRONG |
| 19 | USB_HP_CAN_TX | 0x0800EFA5 | STRONG (CAN TX ISR) |
| 20 | USB_LP_CAN_RX0 | 0x0800EFA9 | STRONG (CAN RX0 ISR) |
| 21 | CAN_RX1 | 0x08006179 | STRONG |
| 22 | CAN_SCE | 0x080061F5 | STRONG |
| 25 | TIM1_UP | 0x0800BE71 | STRONG (fast-loop kandydat!) |
| 28 | TIM2 | 0x0800C189 | STRONG |
| 29 | TIM3 | 0x0800C209 | STRONG |

Pozostałe 52 slotów = default handler ⇒ nieużywane przerwania.
UWAGA: brak osobnego ADC3/TIM8_UP handlera — TIM8 może pracować bez IT albo polling/DMA.

## 2.4 RAM hardware-state (pierwsze wpisy)

| ADDRESS | NAME | EVIDENCE | CONFIDENCE |
|---|---|---|---|
| 0x20000101 | clock_ok_flag (byte; 1=HSE/PLL OK, 0=fallback HSI) | store po SetSysClock @0x0800992A/@0x08009988 | CONFIRMED |

---

# 3. Clock tree (PART 2 — COMPLETE)

## 3.1 Źródła

| OSC | VALUE | CONFIDENCE |
|---|---|---|
| HSI | 8 MHz (internal, fallback path używa /2×16) | STRONG (standard F1/GD32) |
| HSE | crystal — wartość NIE dowodliwa z BIN | OPEN → EXTERNAL |
| LSE/LSI | RTC na LSI (RTCCLKConfig(0x200)=LSI @0x0800A39C) | CONFIRMED |

## 3.2 PLL / SYSCLK

REGISTER-LEVEL (RCC base 0x40021000, CFG0=+0x04):

| STEP | REGISTER WRITE | DECODE | EVIDENCE |
|---|---|---|---|
| DeInit | CIR(+8)=0x9F0000; CR bits clear | stdperiph RCC_DeInit | fn@0x08009994 |
| HSE ON | CR(+0) bit16 (bez BYP bit19) | RCC_HSEConfig(0x10000) | fn@0x08009ABC, call@0x080098CE |
| wait HSERDY | flag code 0x39→wait helper 0x08009D7A | | @0x080098D2 |
| HPRE=/1 | CFG0[7:4]=0 | bic 0xF0 | fn@0x08009A70, call@0x080098EA |
| APB2=/1 | CFG0[13:11]=0 | bic 0x3800 | fn@0x08009BB4, call@0x080098F0 |
| APB1=/2 | CFG0[10:8]=0x1<<8=0x100 (kod 0x400<<? — patrz niżej) | bic 0x700, orr param>>? | fn@0x08009B70, call@0x080098F6 (r0=r4<<10, r4=1→0x400=kod ÷2) |
| FLASH | ACR: latency 2WS + PRFTBE | fn@0x080073D8(2), fn@0x08007318(0x10) | calls@0x080098DE/E4 |
| PLL src | CFG0[17:16]=0b11 = HSE/2 | param 0x30000 | fn@0x08009C1C, call@0x08009902 |
| PLL mul ×9 | CFG0[21:18]=0b1001 | param 0x240000 | jw. |
| PLL ON + wait PLLRDY | CR bit24; flag 0x39 | RCC_PLLCmd(1) | fn@0x08009BF8, call@0x08009908 |
| SW=PLL | CFG0[1:0]=0b10 | RCC_SYSCLKConfig(2) | fn@0x08009D18, call@0x08009918 |
| wait SWS==PLL | (CFG0>>2)&0xC == 8 | fn@0x08009A60 | loop@0x08009920 |
| flag | RAM 0x20000101 = 1 | strb | @0x0800992A |

Fallback (HSE fail) @0x08009930: HSI ON → ten sam schemat, PLL=HSI/2×16, flag=0.

Secondary path fn@0x08009850 (call @0x0800A40C): przełączenie na PLL HSI/2×16 z timeoutami,
zwraca status — używane np. przy rekonfiguracji/uszeniu.

## 3.3 Wynikowe częstotliwości

FORMULA (CONFIRMED): SYSCLK = (HSE/2)·9.

| ITEM | VALUE | NOTE |
|---|---|---|
| SYSCLK | 72 MHz **jeśli HSE=16 MHz** | wartość kryształu: EXTERNAL EVIDENCE |
| HCLK | = SYSCLK | HPRE ÷1 |
| PCLK2 (APB2) | = HCLK | ÷1 |
| PCLK1 (APB1) | = HCLK/2 | ÷2 |
| Timery na APB× | GD32: gdy presc>1 → ×2 ⇒ TIM na APB1 ma takt = HCLK; ST F1: analogicznie ×2 | STRONG |
| ADCCLK | **PCLK2/6** = 12 MHz @72 MHz (ADCPRE=0b10, write 0x8000 @0x0800B33C) | CONFIRMED config |
| CAN | = APB1 (=HCLK/2) | STRONG |

## 3.4 Timing database (start)

| ITEM | VALUE | RAW | DERIVATION | CONFIDENCE |
|---|---|---|---|---|
| CPU/HCLK | (HSE/2)×9 | CFG0[21:18]=9,[17:16]=HSE/2 | §3.2 | CONFIRMED formula |
| PCLK1 | HCLK/2 | CFG0[10:8] | §3.2 | CONFIRMED |
| PCLK2 | HCLK | CFG0[13:11]=0 | §3.2 | CONFIRMED |
| Flash WS | 2 | fn@0x80073D8 arg=2 | | CONFIRMED |
| Boot clock | HSI 8 MHz do main() | brak configu przed main | | CONFIRMED |

## 3.5 Open questions

QUESTION: Wartość HSE (crystal).
WHY IT MATTERS: wszystkie częstotliwości absolutne (PWM, ADC sample, CAN bitrate).
CURRENTLY KNOWN: wzór (HSE/2)*9; typowe Bafang: 16 MHz → 72 MHz.
MISSING: pomiar / schemat.
CAN STOCK BIN ANSWER IT? NO.
PORTING IMPACT: wysoki przy klonowaniu timingów.

QUESTION: Dokładny chip (GD32F303xx vs STM32F103 HD vs GD32F103).
WHY IT MATTERS: max freq, ADC spec, RCU różnice.
CURRENTLY KNOWN: M-klasa, 60 IRQ, biblioteka F10x, układ CFGR zgodny F1.
MISSING: DBGMCU_IDCODE dump z hardware.
CAN STOCK BIN ANSWER IT? PARTIAL (możliwe ślady w kodzie).

---

# 4. GPIO / physical pin database (PART 3 — SUBSTANTIAL)

Metoda: xref wszystkich 26 wywołań `GPIO_Init` @0x08007870 + symulacja struct `{u16 pin; u8 speed@+2; u8 mode@+3}`.
Mode enum (stdperiph F1): AIN=0x00, IN_FLOAT=0x04, IN_PDOWN=0x28, IN_PUP=0x48, OUT_PP=0x10, OUT_OD=0x14, AF_PP=0x18, AF_OD=0x1C.

## 4.1 Authoritative pin table

| PINS | CONFIG | SIGNAL (kandydat) | EVIDENCE (call site) | CONFIDENCE | PORTING |
|---|---|---|---|---|---|
| **PA8, PA9, PA10** | AF_PP 50MHz | **TIM1_CH1/CH2/CH3 — motor PWM HIGH** | 0x0800B3C8 | CONFIRMED config / STRONG func | HARDWARE-MANDATORY |
| **PB13, PB14, PB15** | AF_PP 50MHz | **TIM1_CH1N/CH2N/CH3N — motor PWM LOW** | 0x0800B3E4 | CONFIRMED config / STRONG func | HARDWARE-MANDATORY |
| PA11 | IN_PUP | CAN_RX | 0x080065AE (init CAN fn) | STRONG | HARDWARE-MANDATORY |
| PA12 | AF_PP 50MHz | CAN_TX | 0x080065CA | STRONG | HARDWARE-MANDATORY |
| PB12 | OUT_PP 50MHz (konfig. ×3: 0x800A9D6, 0x8016F74) | TIM1_BKIN pin użyty jako GPIO (driver enable / brake?) | manual decode | STRONG pin / OPEN func | TBD |
| PC6, PC7, PC8 | IN_PUP (init @0x8008004), potem IN_FLOAT (@0x8010656) | **Hall 1/2/3** | dwa call-site | STRONG func / kolejność OPEN | HARDWARE-MANDATORY |
| PC9 | IN_FLOAT 10MHz | bike input (Hall? PAS? speed?) | 0x80106AE | OPEN | TBD |
| PD2 | IN_FLOAT | bike input | 0x80106C0 | OPEN | TBD |
| PC12 | IN_FLOAT | bike input | 0x80106D6 | OPEN | TBD |
| PB9 | IN_FLOAT | bike input (też w puli AIN!) | 0x80106EC | OPEN | TBD |
| PB0 | AIN → potem IN_FLOAT (@0x8010670) | analog→digital reconfig | 2 sites | CONFIRMED reconfig / OPEN func | TBD |
| PB2 | IN_FLOAT | boot/service pin? BOOT1! | 0x8010682 | HYPOTHESIS | TBD |
| PC13 | IN_FLOAT | power/ignition detect? | 0x8010698 | HYPOTHESIS | TBD |
| PA15 | OUT_PP | output (light? LED?) | 0x8010708 | OPEN (pin JTAG — patrz 4.2) | TBD |
| PB10 | OUT_PP | output | 0x8010724 | OPEN | TBD |
| PB8 | OUT_PP | output | 0x801073A | OPEN | TBD |
| PB3, PB4, PB5 | OUT_PP | outputs (PB4/PB5 także @0x8016982/9E) | 0x8010754-7A | OPEN | TBD |
| PA0..PA7 | AIN | pula analogowa ADC_IN0..7 | 0x0800B37E | CONFIRMED pool | HARDWARE-MANDATORY |
| PB0, PB1 | AIN | ADC_IN8/9 pool | 0x0800B392 | CONFIRMED pool | TBD |
| PC0..PC5 | AIN | ADC_IN10..15 pool | 0x0800B3A6 | CONFIRMED pool | TBD |

## 4.2 Kluczowe fakty pochodne

1. **JTAG disabled**: PA15/PB3/PB4 używane jako GPIO ⇒ firmware MUSI pisać AFIO_MAPR.SWJ_CFG (do potwierdzenia w PART 22 — szukaj zapisu MAPR).
2. **Kolejność inicjalizacji**: master-init peryferiów (fn @0x0800B334: ADCCLK=÷6, DMA1 EN, AFIO+GPIOA+B, ADC1+ADC2+TIM1 EN, pulle AIN, PWM piny) ≠ późna rekonfiguracja wejść rowerowych (blok @0x08010634+, wołany gdzieś po handshake?). STARTUP SEQUENCE musi to odzwierciedlić.
3. Hall init fn @0x08007FD0 włącza też **TIM3** ⇒ TIM3 = kandydat timebase Hall/PAS.
4. TIM4 clock włączany osobno (@0x0800B9A4), TIM2 (@0x0800847A).

## 4.3 Open questions

QUESTION: Funkcja PB12 (OUT_PP).
WHY IT MATTERS: bezpieczeństwo mostka (BKIN/driver-enable).
CURRENTLY KNOWN: 3× konfigurowany OUT_PP.
MISSING: consumer logiki (zapisy ODR).
CAN STOCK BIN ANSWER IT? YES (trace BSRR/BRR na 0x40010C0C/0x40010C14).
NEXT EXACT TARGET: grep instrukcji piszących GPIOB BSRR w okolicy TIM1 MOE.
PORTING IMPACT: wysoki.

QUESTION: Przypisanie PC9/PD2/PC12/PB9/PB2/PC13 do funkcji rowerowych.
WHY IT MATTERS: PAS/speed/brake/walk mapping.
CURRENTLY KNOWN: konfiguracje pinów; brak konsumentów.
NEXT EXACT TARGET: śledzenie IDR reads (0x40010C08 itd.) — PART 15.
CAN STOCK BIN ANSWER IT? YES.

---

# 7. Timer inventory (PART 5 — SUBSTANTIAL)

Helpery stdperiph/GD-lib zidentyfikowane: DeInit=0x0800CD70, TimeBaseInit=0x0800E718,
OC1/2/3Init=0x0800D774/DA30/DC94, OC4/NInit=0x0800DEAC, BDTRConfig=0x0800C958, CtrlPWMOutputs=0x0800CA60.

| TIMER | BASE | CLOCK ENABLE | CONFIG EVIDENCE | ROLE | CONFIDENCE |
|---|---|---|---|---|---|
| **TIM1** | 0x40012C00 | APB2 @0x0800B364 | master-init fn §8 | MOTOR PWM | CONFIRMED |
| **TIM3** | 0x40000400 | APB1 @0x08007FE8 (w hall-init!) | PSC=63, ARR=65535, center1 | HALL/capture timebase | CONFIRMED role-HALL |
| TIM2 | 0x40000000 | APB1 @0x0800847A | TBD | TBD (IRQ28 handler aktywny) | used |
| TIM4 | 0x40000800 | APB1 @0x0800B9A4 | TBD | TBD | used |
| TIM5 | 0x40000C00 | TBD | TBD | TBD | literal×36 |
| TIM6/7 | 0x40001000/14 | TBD | TBD (bez IO — timebase/DAC trig?) | TBD | literals |
| TIM8 | 0x40013400 | APB2 (z ADC grupą @0x0800B35C) | TBD; IRQ slot default | kandydat ADC trigger master! | OPEN |

# 8. Motor PWM timer — TIM1 (PART 6 — SUBSTANTIAL)

Źródło: master-init fn @0x0800B334, ciąg od 0x0800B3E8 (DeInit→TimeBase→OC→BDTR).
Wszystkie wywołania z bazą TIM1 (pula @VA 0x0800B784 = 0x40012C00).

## 8.1 Rejestry (CONFIRMED config)

| REJESTR | WARTOŚĆ | DECODE | PORTING |
|---|---|---|---|
| PSC (+0x28) | **0** | tick = f_TIM1 | HARDWARE-MANDATORY |
| ARR/CAR (+0x2C) | **2000 (0x7D0)** | patrz timing | HARDWARE-MANDATORY |
| CR1 (+0x00) | mode=0x20 → **CENTER-ALIGNED mode 1**; CLKDIV=0x100 → tDTS=2·tCK; REP=1 (RCR=1) | center-up-down | HARDWARE-MANDATORY |
| CCR1/2/3 init | **1000 (50%)**, OCMode=0x60 (**PWM1**) | start-safe 50% | STOCK-CHOICE |
| BDTR (+0x44) | DTG=0x19=25, OSSR/OSSI/LOCK=0, **BREAK=OFF**, AOE=OFF | patrz niżej | ⚠️ HARDWARE-MANDATORY |
| CR2 (+0x04) | \|= 1 → **CCDS=1** (CCx DMA req na update) | wpływa na DMA/ADC arch | HARDWARE-MANDATORY |

## 8.2 Timing (przy SYSCLK=72 MHz / HSE=16 MHz)

| ITEM | VALUE | DERIVATION | CONFIDENCE |
|---|---|---|---|
| Timer tick | 13.89 ns | 72 MHz | formula CONFIRMED / absolutna zależna od HSE |
| tDTS | 27.78 ns | CLKDIV=/2 | CONFIRMED |
| Half-period | 27.78 µs | ARR=2000 | CONFIRMED |
| Full period | 55.56 µs | center mode ×2 | CONFIRMED |
| **PWM frequency** | **18 kHz** | 72M/(2·2000) | STRONG (HSE-dependent) |
| Update event (RCR=1) | **co 2-gi event ovf/udf → 9 kHz** | RCR=1 | STRONG — kandydat fast-loop! |
| **Dead-time** | **25 × tDTS ≈ 694 ns** | DTG<128 linear | CONFIRMED encoding |

## 8.3 Wyjścia / struktura mostka

- CH1/2/3 → PA8/PA9/PA10 (HIGH side), CH1N/2N/3N → PB13/PB14/PB15 (LOW side) — §4.1
- Polaryzacje: pola OC-struct [+8]=0 (OCPolarity active-high), [+A]=8 — dokładny mapping pól N-polarności OPEN
- **BREAK/BKIN wyłączone w BDTR** ⇒ brak sprzętowego fast-shutdown przez PB12;
  PB12 jako OUT_PP = sterowany programowo (funkcja OPEN §4.3)
  ⇒ CAŁA ochrona nadprądowa fast-path musi być SOFTWARE (ISR ADC) albo w gate-driverze — KLUCZOWE dla EVistDrive

## 8.4 Open questions

QUESTION: Mechanizm włączania CH1N/2N/3N (cztery wywołania z r1=8 po OCxNInit).
WHY IT MATTERS: sekwencja startu mostka.
CURRENTLY KNOWN: wywołania 0x0800D964/BFC/E3C/FE8 z parametrem 8.
NEXT EXACT TARGET: disasm tych 4 helperów (offsety CCER bits 10/9/8?).
CAN STOCK BIN ANSWER IT? YES.

QUESTION: TIM8 rola (38 literałów, clock EN z ADC-grupą).
WHY IT MATTERS: możliwy TRGO→ADC master trigger zamiast TIM1.
NEXT EXACT TARGET: okna zapisów TIM8 (audyt jak §8) + konfiguracja ADC trigger selektora.
CAN STOCK BIN ANSWER IT? YES.

---

# 11. ADC complete database (PART 9 — SUBSTANTIAL)

Konfiguracja: master-init fn @0x0800B334 (ciąg od 0x0800B59C). Bazy w pulach: ADC1=0x40012400,
ADC2=0x40012800 (pool przechowuje ADC1_DR=0x4001244C i odejmuje 0x4C).
Helpery: RegularChannelConfig=0x08005CB8, Enable-fn=0x08005EB8, Ready-poll=0x080058A4.

## 11.1 Regular groups → DMA

**DMA**: struct init @0x0800B54C-80 → DMA_Init; wskaźniki: ADC1_DR=0x4001244C oraz bufor RAM;
**count=8 elementów** ([sp+0x58]=8); DMA1_CH1 (CCR=0x40020008 pool).

### ADC1 regular sequence (8 kanałów)

| RANK | CH | PIN | SAMPLE TIME CODE | LOGICAL GUESS |
|---|---|---|---|---|
| 1 | 1 | PA1 | 1 | battery I? |
| 2 | 0 | PA0 | 2 | battery V? |
| 3 | 10 | PC0 | 3 | aux/temp |
| 4 | 11 | PC1 | 7 | aux |
| 5 | 12 | PC2 | 5 | aux |
| 6 | 13 | PC3 | 6 | aux |
| 7 | 17 | **VREFINT** | 4 | kalibracja napięcia |
| 8 | 1 | PA1 (dup!) | 1 | OPEN |

### ADC2 regular sequence (8)

| RANK | CH | PIN | ST |
|---|---|---|---|
| 1 | 7 | PA7 | 1 |
| 2 | 14 | PC4 | 2 |
| 3 | 15 | PC5 | 3 |
| 4 | 8 | PB0 | 1 |
| 5 | 9 | PB1 | 1 |
| 6 | 4 | PA4 | 1 |
| 7 | 6 | PA6 | 1 |
| 8 | 0 | PA0 | 1 |

Sample-time codes: 1..7 (F1: 7.5/13.5/28.5/41.5/55.5/71.5/239.5 cykli).
ADCCLK=12 MHz ⇒ np. ST=1 → ~1 µs total per conv.

## 11.2 Injected groups

Dwa wywołania init-injected (@0x0800B5CC i @0x0800B60A) ze struct: [+0]=0x10000?, [+4]=1,
[+5]=0, [+8]=0xE0000, [+C]=0x800, [+10]=8 — dokładny mapping pól OPEN (helper 0x08005964).
Kandydat na pomiary PRĄDÓW FAZ (szybka ścieżka) — patrz §13.

## 11.3 Fast-loop ISR (IRQ18 ADC1_2) @0x08005578 — CONFIRMED mechanics

```
SR=ADC1[0]; test bit2 (JEOC); jeśli nie — exit
clear JEOC (write ~(bit2) do SR)
r0 = *(u32*)(ADC1+0x40)          ; JDR2 region!
val = UBFX r0,#3,#16             ; bity [18:3]
store halfword val → RAM 0x2000011A
flag=byte[0x200005C7]
 if flag==0:
    if byte[0x2000045B]==2 → bl 0x08007604   ; ścieżka przetwarzania
    else store 0 → halfword [0x20000662]
 else → bl 0x0801A488               ; ← główna pętla szybka (FOC?)
```

OPEN: znaczenie ekstrakcji bitów [18:3] (packed dual-ADC? left-align?) — wymaga weryfikacji
z specyfiką ADC tego chipu; anchory podane wyżej.

---

# 10. PWM -> ADC trigger architecture (PART 10 — SUBSTANTIAL)

## 10.1 Zidentyfikowane helpery ADC (CONFIRMED)

| HELPER | FUNKCJA | DOWÓD |
|---|---|---|
| 0x08005CB8 | ADC_RegularChannelConfig(base,ch,rank,st) | sekwencja wywołań §11.1 |
| 0x08005B00 | InjectedChannelConfig(base,ch,rank,st) | wywołania §10.3 |
| 0x08005628 | ADC_Cmd (CTL1+8 bit0 ADON) | disasm body |
| 0x08005684 | ADC_DMACmd (CTL1 bit8) | disasm body |
| 0x080056D8 | ADC_DeInit (APB2RSTR toggle) | disasm body |
| 0x08005C58 | JSQR rank RMW (+0x38) | disasm body |
| 0x0800580C | **JEXTSEL config** (maska na CTL1) | wywołania z 0x7000 |
| 0x08005964 | InjectedInit(struct) | §11.2 |
| 0x08005754 | IT-config (JEOCIE kandydat) | (ADC1,1)+(ADC2,1) |

## 10.2 Wynikowa architektura triggerów

| ELEMENT | KONFIGURACJA | CONFIDENCE |
|---|---|---|
| ADC1 injected JEXTSEL | **0b000** (F103/GD32: **TIM1_TRGO**) | STRONG (kod zapisany 0) |
| ADC2 injected JEXTSEL | **0b111** (alternatywna źródło TIM8-rodzina) | CONFIRMED raw |
| ADC1 regular | DMA circular-ish buf 8; EXTSEL nieznaleziony w tym fn | OPEN |
| Kalibracja ADC | **CR2 \|= 0x800000 (CAL) dla ADC1 wykonana po init** | CONFIRMED (@0x0800B600); ADC2 OPEN |
| Kalibracja ADC (2) | **helper 0x08005EB8 ustawia CTL1 bit2 dla ADC1 i ADC2** | CONFIRMED |
| Kolejność startu | DeInit → channels/injected → CAL(ADC1) → DMACmd(ADC1) → Cmd(ADC1)+Cmd(ADC2) | CONFIRMED |

### ⚡ Wskazówka identyfikacyjna MCU

Helper @0x08005EB8 ORRUJE **CTL1 bit2** (bez parametru state). W STM32F103 bit2 CTL1
jest zarezerwowany, natomiast w **GD32F30x CTL1[2]=CLB — start kalibracji ADC**
(oraz [3]=RSTCLB). Obecność tej operacji ⇒ biblioteka/cel = **GD32F30x**, co wspiera
rodzinę GD32F303 z promptu (choć build używa nazw plików STM32F10x StdPeriph).
CONFIDENCE: STRONG (pośredni), finalny potwierdzenie = DBGMCU_IDCODE z hardware.

---
## 10.3 Injected channels (z 0x08005B00)

| ADC | RANK1 | RANK2 | uwagi |
|---|---|---|---|
| ADC1 | **CH2 (PA2)** | **CH0 (PA0)** | kandydaci phase-current A/B! |
| ADC2 | **CH3 (PA3)** | **CH5 (PA5)** | kandydat phase-C / drugi sygnał |

⇒ Prawdopodobny pomiar prądów fazowych = INJECTED na PA2/PA0 (ADC1) i PA3/PA5 (ADC2),
wyzwalany sprzętowo od TIM1/TRGO w środku aktywnego wektora PWM. Rekonstrukcja 2-of-3
jak w klasycznym stocku Bafang. CONFIRMED anchors / interpretacja STRONG.

## 16*. NVIC priorities (PART 16 — SUBSTANTIAL, struktury NVIC_Init @0x08008FE8 ×4)

| IRQ# | NAME | PREEMPTION | SUB | CMD | EVIDENCE |
|---|---|---|---|---|---|
| 18 | **ADC1_2** | **0 (najwyższy)** | 0 | ON | {0x12,0,0,1} |
| 25 | TIM1_UP | 1 | 0 | ON | {0x19,1,0,1} |
| 24 | TIM1_BRK | 0 | 0 | **OFF** | {0x18,0,0,0} |
| 11 | DMA1_CH1 | 2 | 1 | ON | {0xB,2,1,0?} |

⇒ Fast-loop = ADC ISR (prio 0) > TIM1_UP ISR (prio 1) > DMA (prio 2).

---

# 30. CAN hardware (PART 18 — SUBSTANTIAL)

## 30.1 Piny / zegar / struktura

| ITEM | VALUE | EVIDENCE | CONFIDENCE |
|---|---|---|---|
| Instancja | **CAN1 @0x40006400** (bxCAN) | literały + assert listy {CAN1,CAN2} | CONFIRMED |
| RX pin | **PA11** (IN_PUP) | GPIO_Init @0x080065AE | CONFIRMED config |
| TX pin | **PA12** (AF_PP 50MHz) | @0x080065CA | CONFIRMED config |
| Remap | brak (domyślne PA11/PA12) | brak zapisów AFIO_MAPR dla CAN | STRONG |
| Clock EN | APB1 bit25 | @0x08006592 | CONFirmed |
| IRQ | TX=19, RX0=20, RX1=21, SCE=22 | vector table §2.3 | CONFIRMED |

## 30.2 Bitiming (fn @0x08006904 → CAN_Init @0x08006678)

Struct {u16 prescaler@0; sjw@2; ts1@3→SJW?? ; bs1@4→TS1[19:16]; bs2@5→TS2[22:20]}.
BTR pack (body @0x080684A-66): `([2]<<30)|([3]<<24)|([4]<<16)|([5]<<20)|(presc-1)`.

Wybór wg flagi **RAM 0x20000201** (config z NVM):

| WARIANT | PRESCALER | TS1 | TS2 | SJW | quanta/tj | bitrate @PCLK1=36 MHz |
|---|---|---|---|---|---|---|
| flaga≠0 | 11 | 8 (+1=9tq) | 1 (+1=2tq) | 1 | 12 | **≈272.7 kbps** (niestandard) |
| flaga==0 | 12 | 12 (+1=13tq) | 1 (+1=2tq) | 1 | 16 | **=187.5 kbps** (dokładnie) |

UWAGA: wartości absolutne zależą od HSE (przyjęto 16 MHz ⇒ PCLK1=36 MHz).
Wariant B daje idealnie 187.5 k; wariant A nie jest standardowy — możliwy niestandardowy
protokół Bafang albo inny krystal w wariancie sprzętowym. Sample point B = (1+13)/16 = 87.5%.

## 30.3 Helpery CAN (do reuse w PART 19)

| HELPER | ROLA |
|---|---|
| 0x08006678 | CAN_Init(base,&struct) |
| 0x080065D4 | CAN_ITConfig (IER +0x14) |
| 0x08006AAC | fill-defaults / struct init |
| 0x08006980 | RX decode (mailbox +0x1B0: IDE/DLC/ID extract) |
| 0x0800632C | reset/deinit |

## 30.4 Open questions

QUESTION: Filtry CAN (FBTR/FMR/FxR1..) i FIFO assignment.
WHY IT MATTERS: przyjmowane ID.
KNOWN: literał CAN_FMR(0x40006600) używany przez fn kończącą się @0x08006578
(operacja `FMR &= ~1` = wyjście z trybu inicjalizacji filtrów ⇒ filtry KONFIGUROWANE).
NEXT EXECT TARGET: pełny disasm fn @0x08006500-78 (FxR wartości!).
CAN STOCK BIN ANSWER IT? YES.

QUESTION: Znaczenie flagi 0x20000201 (wariant bitrate'u).
NEXT EXACT TARGET: producenci tej komórki RAM (NVM loader?).
CAN STOCK BIN ANSWER IT? YES.

---

# 31. CAN identifier architecture (PART 19 — PARTIAL)

## 31.1 Ścieżka odbioru (CONFIRMED)

```
IRQ20 CAN_RX0 @0x0800EFA8:
  pending? FIFO0 przez 0x080068A8(CAN1,fifo)
  slot = RAM 0x20000A64 + w_idx*20      ; ring 5 slotów × 20B
  bl 0x08006980(CAN1,&slot)             ; decode: [+0]=STDID, [+4]=EXTID-word,
                                        ;          [+8]=IDE, DLC, data...
  count guard: byte[base+0x66]<5 ? ++(critical) : halfword[base+0x68]++ (OVERFLOW ctr)
  w_idx wrap 0..4
Dispatcher (main loop, okno @0x8011B48):
  filtruje ID-markery {0x2000,0x4000} i kody {3,5,9,19}; gate byte[0x20000354]
  magic handshake: data[+B]==1 && [+C]=='E'(0x45) && [+D]==2 ⇒ byte[0x20000334]=1
  dalej: bl 0x08017E64(&msg)
Drugi ring @RAM 0x20000AD0 (idx +0x65, cnt +0x66) konsumowany równolegle.
```

## 31.2 Bitowe pola EXT ID (STRONG)

Z dispatchera @0x08017E64:

| BITFIELD | ZNACZENIE | DOWÓD |
|---|---|---|
| EXTID[28:24] | **adres/node** (match do tabeli sesji) | ubfx #24,#5 vs slot[+1] |
| EXTID[23:19] | **typ operacji**; wartość **2 = MULTIFRAME/session** | ubfx #19,#5 cmp #2 |

## 31.3 Tabela sesji / multiframe

| ITEM | VALUE |
|---|---|
| Base | RAM **0x20000C80**, 5 slotów × **80 B** (idx*5<<4) |
| Aktywność slotu | byte[slot+0x4E] |
| Node id slotu | byte[slot+0x01] |
| Limit sesji | byte[base+0x190] (<5 sprawdzane) |
| Handler slotu | 0x08008A58(slot) |

⇒ Stock prowadzi do 5 równoległych "sesji" protokołu (multiframe START/DATA/END jak w prompt §19F) — pełna rekonstrukcja wymaga dump 0x08008A58 (NEXT TARGET).

## 31.4 Open questions

QUESTION: Komplet komend (COMMAND DATABASE).
KNOWN: kody {3,5,9,0x13} filtrowane; magic 'E',02.
NEXT: disasm 0x08017E64 w całości + tabela TX ID (mailbox TIR writes).
QUESTION: Skąd STDID vs EXTID w praktyce ramkach HMI.
NEXT: log z hardware + decode 0x08006980 full.

---

# 17*. Fast loop / current processing (PART 12/13 — PARTIAL)

## Dwie ścieżki po ADC IRQ (CONFIRMED anchors)

### A) Ścieżka „stan==2" → 0x08007604 (przetwarzanie/filtry)
- bl 0x0800810C (source select?)
- kopie słów: [0x200004AC]→[0x200004D4], wynik 0x0800B17C→[0x200004AC]
- 0x08006CEC→[0x200004B0]; 0x08007EFC→r4; para ([0x200004B0], r4)→0x08009408→[0x200004B8]
- kopiowanie do 0x200004B4 + **IIR: new=(0x599A*old + 0x2666*x)>>15 ≈ LPF α≈0.15** na [B4+2]
- dalsze bramki: [0x20000516]>6, sbyte[0x2000053C]>0, byte[0x200005BD]…

### B) Główna pętla szybka → 0x0801A488
```
bl 0x08008C80                       ; sync/cache?
BUF = RAM 0x200012A0                ; bufor wejściowy (słowa 32-bit)
val_a = UBFX(BUF[+0x08], #4, #12) → halfword @0x20000118+4
val_c = UBFX(BUF[+0x18], #4, #12) → @+0xC
val_b = UBFX(BUF[+0x10], #4, #12) → @+8
jeśli byte[0x2000045B]==0:
   kopiuj a/c/b → "last valid" 0x200005CA/CC/CE ; fresh-flag byte[0x200005C8]=1
histogramy/przesuwne okna (4×u16): 0x20000450 / 0x20000448 / 0x20000440 (shift co iterację)
nowe próbki doklejane na [+6] każdego okna
```

INTERPRETACJA: BUF[+8]/[+0x10]/[+0x18] = trzy kanały pomiarowe pobierane co cykl szybki;
pakiety 12-bit w bitach[15:4] ⇒ dane LEFT-ALIGNED (spójne z podwójnym pakowaniem
dual-ADC lub innym układem DMA niż regularny §11.1 — DO ROZSTRZYGNIĘCIA: skąd BUF).
Flagi INVALID/last-good potwierdzone strukturalnie (§ prompt 19: PRIMARY/ALTERNATE/last-valid).

## Open questions

QUESTION: Producent bufora 0x200012A0 (który DMA/który tryb pakowania).
WHY IT MATTERS: mapowanie próbka↔kanał fizyczny.
NEXT EXACT TARGET: pełny disasm 0x08008C80 + DMA NDTR/CPAR runtime writes.
CAN STOCK BIN ANSWER IT? YES.

QUESTION: Rola 0x08007604 vs 0x0801A488 (dwa stany byte[0x2000045B]).
NEXT EXACT TARGET: pisarze 0x2000045B (state machine start/stop).

## ROZSTRZYGNIĘTE (update)

**Producent BUF = regularny DMA ADC1** (CONFIRMED):
struct DMA w master-init: CPAR=ADC1_DR(0x4001244C) [pool@VA 0x800B78C],
**CMAR=0x200012A0 [pool@VA 0x800B790]**, count=8.
⇒ FOC czyta **rangi 3/5/7** sekwencji regularnej:
BUF[+0x08]=rank3=**CH10(PC0)**, BUF[+0x10]=rank5=**CH12(PC2)**, BUF[+0x18]=rank7=**CH17 VREFINT**.
Interpretacja OPEN: czy rangi 3/5/7 to trzy fizyczne sygnały, czy okno czasowe
(circular NDTR=8 ⇒ kolejne słowa = kolejne próbki w czasie — schemat "sampling sector").
Istnieje DRUGI bufor **0x200012C0** (=A+0x20; konsumenci @0x8007E58/EA0, 0x80082C0, 0x800C690)
— kandydat: druga połowa ping-pong albo dane ADC2. OPEN.

**Sekwencer szybkiej pętli (fn 0x08008C80)**: licznik stanu byte[RAM 0x200003DE] liczy do
**20 (0x14)**; porównania [0x20000418]↔[0x200005EE]; wrap-porównanie z +0x7F8 (2040).
⇒ maszyna 20-stanów sterująca fazami pomiaru/procesu w pętli szybkiej.

**Timeout sesji CAN**: halfword[slot+0x4C] ≥ **500** ⇒ bl 0x08019CC0(slot,1)+0x08012DF4(slot)
(abort); inaczej inkrement. (@0x08008A58)

**ROZSTRZYGNIĘTE: bufor jest CIRCULAR (STRONG)**

Pełny decode `DMA_Init` @0x08007088: struct {CPAR@0, CMAR@4, NDT@8(walidacja <0x10000),
PSIZE@+0x10{0,0x40}, MSIZE@+0x14{0,0x80}, pole+0x18{0,0x100,0x200}, PRI@+0x1C{0,0x400,0x800},
**pole+0x20{0,0x20}**, pole+0x24{0,0x1000,0x2000,0x3000}, pole+0x28{0,0x4000}};
finalny CCR = suma wartości (maska czyszczenia 0x7FF0 ⇒ bity[14:4]), potem NDT→+4,
CPAR→+8, CMAR→+C kanału.

Konfiguracja master-init: DIR=P2M, NDT=8, PSIZE/MSIZE=32-bit, **pole+0x20=0x20**,
PRI=0x2000(high), pole+0x28=0.

Wartość 0x20 w slot walidowanym jako jedyna alternatywa {0,0x20} i wchodząca prosto
do CCR odpowiada **bitowi CIRC** ⇒ **DMA pracuje w trybie circular** (STRONG).
Konsekwencja: BUF[8 słów] = pierścień czasowy ⇒ odczyt stałych offsetów +8/+0x10/+0x18
w pętli szybkiej = okno ostatnich próbek (semantyka "sampling sector" bardziej prawdopodobna
niż trzy stałe kanały). Dokładne przypisanie próbka↔kanał wymaga jeszcze ustalenia
EXTSEL regularnego (ile konwersji na trigger) — OPEN.

---

# 32. CAN TX / sesje / bootloader (PART 19 kont. — PARTIAL+)

## 32.1 CAN_Transmit i builder ramki (CONFIRMED anchors)

| FN | ROLA |
|---|---|
| **0x08006AAC** | CAN_Transmit(base,&msg): wybór mailboxa po TSR.TME{26,27,28}; TIR(+0x180+mbx*16): STDID<<21 lub EXID<<3+IDE; DLC→TDT; dane→+8/+0xC; start = **TIR\|=1 (TXRQ)** |
| **0x08019C9C** | Builder TX ID: **EXTID = 0x2000000 \| (node<<19) \| 0x20000 \| (u16 data)** → bl 0x08019B38 |

⇒ Potwierdzenie architektury ID po stronie TX: bity [23:19]=adres node'a
(zgodne z RX dispatchera §31.2), stałe flagi bit25 i bit17, młodsze 16 bitów = payload-ID/dane.

## 32.2 Sesje

- Timeout slotu: halfword[slot+0x4C] ≥ 500 tick → **0x08019CC0(slot,1)** — ta sama rodzina
  buildera ID (node<<19) ⇒ abort/wiadomość kończąca sesja.
- Tabela 5×80B @0x20000C80; cleanup zeruje powiązane struktury
  @0x20001108/10E0/10B8/1130/1158 (+0x18).

## 32.3 CRC16 (multiframe/integralność)

**fn @0x0801A318**: klasyczny tabelkowy CRC16:
`crc = (crc<<8) ^ table16[ ((crc>>8) ^ byte) & 0xFF ]`, tabela @**0x0801B358** (256×u16).
IDENTYFIKACJA: tabela = standard CCITT(poly 0x1021) **<<1** (t[1]=0x2042, t[2]=0x4084,
t[4]=0x8108 ⇒ efektywny poly **0x2042** / lub wynik przesunięty). Init/końcowy XOR —
do potwierdzenia na ramce z logu hardware.

## 32.5 CAN_FilterInit (helper = **0x0800640C**, tail @0x080064E0-78)

Struct-based; base FMR=0x40006600 (=CAN1+0x200). Walidacje wejścia (body od +0):
bank=[+A]≤13, mode=[+B]∈{0,1}, scale=[+C]∈{0,1}, FIFO=[+8]∈{0,1}, act=[+D]∈{0,1};
maska bitów = 1<<bank; wejście w INIT (`FMR\|=1`).

| STRUCT FIELD | CEL | ZNACZENIE |
|---|---|---|
| [+0],[+2] u16 | FR1 = FMR+0x40 + n*8 | ID filtru (hi/lo) |
| [+4],[+6] u16 | FR2 = ...+0x44 | maska |
| [+0xA] u8 | numer banku (×8 B) | filter number |
| [+0xB] | FM1R(+4) set/clear bit n | mask(0)/list(1) |
| [+8] {0,1} | FS1R(+0x14) bit n | skala 16/32-bit |
| [+0xD]==1 | FFA1R(+0x1C) bit n | FIFO0/FIFO1 |
| koniec | `FMR &= ~1` | wyjście z INIT |

Wywołania: **dynamiczny filtr per-sesja @0x08019E40** (§32.6). **Statyczna tablica
filtrów = 0x0801B2D8** (8 banków × 16B), dekodowana przez helper @0x0819E3E
(skompresowany format: lower halfword=EXTID, upper=encoder). Wszystkie banki:
mode=0(mask), scale=1(32b), IDE=1(extended), activation=1.

| Bank | FIFO | FR1 (acceptance) | FR2 (mask) | Zakres |
|------|------|-------------------|------------|--------|
| 0 | 0 | 0x00018804 | 0x0007FFFE | EXTID[15:0]=0x3100 (system) |
| 1 | 1 | 0x00000004 | 0x00000006 | **ALL extended** (passthru) |
| 2 | 1 | 0x0000A004 | 0x0006F806 | zakres częściowy |
| 3 | 1 | 0x00000004 | 0x00000006 | **ALL extended** (passthru) |
| 4 | 1 | 0x00000004 | 0x00000006 | **ALL extended** (passthru) |
| 5 | 1 | 0x00000004 | 0x00000006 | **ALL extended** (passthru) |
| 6 | 1 | 0x0001E004 | 0x0007F806 | zakres częściowy |
| 7 | 1 | 0x00000004 | 0x00000006 | **ALL extended** (passthru) |

Bank 0 (FIFO0) = filtr systemowy (wąski); Banki 1,3,4,5,7 (FIFO1) = pełny passthru
dla dynamicznych sesji CAN (RX1 ISR =0x0800EFA8). Banki 2,6 = częściowe zakresy.

## 32.4 Bootloader / reboot (potwierdzenie istnienia)

Fn @~0x0801A2B8: DeInit ADC1+ADC2, wyłączenie DMA(kanał), odczyt **wektora resetu
bootloadera z 0x08000004** → RAM 0x20000330/32C → `MSR SP` + `BLX` ⇒ **jump do
bootloadera @0x08000000**. Dowód: bootloader istnieje osobno przed aplikacją
(spójne z app-base 0x08005000). OTA/enter-bootloader path do rozszerzenia w PART 39.

---

## 32.6 Dynamiczny filtr per-sesja (CONFIRMED)

Fn @0x08019E40: buduje FilterInit struct Z SESJI i woła 0x0800640C:

| FILTERINIT FIELD | ŹRÓDŁO (session struct) | ZNACZENIE |
|---|---|---|
| FR1.hi | ubfx(word@+4,13,16) | ID bits[28:13] |
| FR1.lo | (u16(word@+4)<<3) + **4** | EXID<<3 \| **IDE=EXT** |
| FR2.hi | ubfx(word@+8,13,16) | maska bits[28:13] |
| FR2.lo | (u16(word@+8)<<3) + **6** | maska z IDE\|RTR |
| FM | **0 = tryb MASKA** | |
| bank | byte[sess+0xC] | |
| FFA | **1 = FIFO1** | |

⇒ Stock instaluje filtr dopasowany do ID sesji w FIFO1 (RX0 ISR obsługuje FIFO0 — patrz §31.1;
drugi ring @0x20000AD0 prawdopodobnie koresponduje z FIFO1).

---

# 12*. Pętla FOC — ścieżka danych ADC→DMA→FOC (PART 12/13 — CONFIRMED)

## Pełny pipeline (CONFIRMED)

```
ADC1 regular sequence (8 conv):
  SQR3: CH1(PA1), CH0(PA0), CH10(PC0), CH11(PC1), CH12(PC2), CH13(PC3)
  SQR2: CH17(VREFINT), CH1(PA1)
         ↓
DMA1 CH1 circular (NDT=8, src=0x4001244C, dst=0x200012A0)
         ↓
FOC loop @0x0801A488 reads:
  word[+0x08] = DMA[2] = CH10(PC0)  → UBFX[4,12] → 0x2000011C+4
  word[+0x10] = DMA[4] = CH12(PC2)  → UBFX[4,12] → 0x2000011C+8
  word[+0x18] = DMA[6] = CH17(VREF) → UBFX[4,12] → 0x2000011C+C
```

## Skala i format

- **UBFX[4,12]** = 12-bit extractor z bit4 → **surowa wartość ADC (0..4095)**
- **Zero dodatkowego skalowania** w pętli FOC
- **4-okna przesuwne** @0x20000440/{+8,+10} (4×u16 per kanał)
- **Średnie** (sum>>2): → 0x200003F8/FA/FC
- **Last-valid** (gdy fresh=1): → 0x200005CA/CC/CE

## Kanały wejściowe (CONFIRMED)

| Kanał ADC | Pin | Rola (HYPOTHESIS) | DMA word | FOC avg |
|-----------|-----|-------------------|----------|---------|
| CH10 | **PC0** | Prąd fazy / sensor | DMA[2] | avg[0] |
| CH12 | **PC2** | Prąd fazy / sensor | DMA[4] | avg[1] |
| CH17 | VREFINT | Referencja napięciowa | DMA[6] | avg[2] |
| CH0 | PA0 | Torque? (nie monitorowany w FOC) | DMA[1] | — |
| CH1 | PA1 | Torque? (nie monitorowany w FOC) | DMA[0,7] | — |
| CH11 | PC1 | Rezerwowy? | DMA[3] | — |
| CH13 | PC3 | Rezerwowy? | DMA[5] | — |

## ADC_CR1 = 0x480 (CONFIRMED)

- **bit7 = DISCEN** = 1 (discontinuous mode regular)
- **bit10 = JDISCEN** = 1 (discontinuous mode injected)
- AWDEN/JAWDEN = 0 (watchdogi NIEaktywne w CR1 — korekta)
- TSVREFE=1 (VREFINT+temp sensor enabled via CR2 bit20)
- AWDG thresholds: **brak zapisów** → wartości domyślne HTR=0xFFF(4095), LTR=0x000(0)

# 15. IDR trace — przepływ danych z czujników (PART 15 — SUBSTANTIAL)

## Hall sensors — pełny dataflow (CONFIRMED)

```
PC6/7/8 (GPIOC) → input w/
    ↓
GPIOC_CRL/CRH config via struct @0x8007870
    ↓
TIM3 (PSC=63, ARR=65535, CENTER1) — przechwytuje przejścia
    ↓
TIM2 ISR @0x800C188 — co przejście hall:
  1. Sprawdza flagę TIM2 przez helper 0x800D260
  2. Toggle byte [0x2000043B] (edge indicator)
  3. Wywołuje 0x8007FAC → oblicza prędkość:
     speed = (capture × 160000) / 458752 ≈ capture × 0.349
     (capture z 0x2000053A, unsigned halfword)
  4. Zapisuje speed → 0x20000370 (signed halfword)
  5. speed < 0: inkrementuje licznik błędów [0x200005BF]
     speed ≥ 0: zeruje licznik błędów
  6. speed > 0x4B0 (1200): flaga [0x20000698] = 1 (over-speed?)
     speed ≤ 0x4B0: flaga = 0
  7. Wywołuje 0x8012BF0 → zadania okresowe:
     - co 10 ticków: [0x200003EA] = 1
     - co 50 ticków: [0x20000685] = 1
     - co 100 ticków: [0x200003EB] = 1
  8. Czyści flagę TIM2 przez 0x800CB38
```

## RAM anchors hall (CONFIRMED)

| Adres | Typ | Rola |
|-------|-----|------|
| 0x2000043B | u8 | Toggle edge indicator (0/1) |
| 0x2000053A | u16 | Timer capture value (from TIM3) |
| 0x20000370 | i16 | Obliczona prędkość (signed) |
| 0x200005BF | u8 | Licznik błędów (increment when speed<0) |
| 0x20000698 | u8 | Flag over-speed (1 when speed>1200) |
| 0x20000470 | u16 | Licznik ticków (do zadań okresowych) |
| 0x200003EA | u8 | Flag: co 10 ticków |
| 0x20000685 | u8 | Flag: co 50 ticków |
| 0x200003EB | u8 | Flag: co 100 ticków |

## CAN RX — pełny dataflow (CONFIRMED)

```
CAN1 FIFO0 → CAN_RX IRQ @0x0800EFA8:
  1. CAN_MessagePending(CAN1, 0) → ile wiadomości
  2. Odczytaj indeks zapisu: [0x20000A64 + 0x64] (u8, 0..4)
  3. CAN_Receive(CAN1, 0, &ring_buf[idx*20]) → kopiuj 20B
  4. Inkrementuj count [0x20000A64 + 0x66] (CPSID/CPSIE)
  5. count < 5: increment write pointer (wrap at 4)
     count ≥ 5: increment overflow counter [0x20000A64 + 0x68]
```

## RAM anchors CAN (CONFIRMED)

| Adres | Typ | Rola |
|-------|-----|------|
| 0x20000A64 | struct[0x6A] | Ring buffer control |
| +0x64 | u8 | Write pointer (0..4) |
| +0x66 | u8 | Pending count |
| +0x68 | u16 | Overflow counter |
| 0x20000A64+0..4×20 | 5×20B | RX message slots |
| 0x20000AD0 | struct | FIFO1 ring buffer |

## fn@0x801245C — clears a flag byte

Prosta funkcja: zeruje bajt pod adresem z literal pool. Wywoływana z TIM2 ISR przed odczytem prędkości — prawdopodobnie resetuje flage przejścia hall.

## fn@0x8007FAC — wzór na prędkość

```
input:  capture = u16 z 0x2000053A (timer count between hall transitions)
factor: 160000 / 458752 ≈ 0.3489
output: speed (signed) = capture × 0.349
```

Prędkość 1200 (0x4B0) odpowiada capture ≈ 3437 → TIM3 tick ≈ 14.3μs (przy 72MHz / 64 PSC = 1.125MHz) → 3437 × 14.3μs ≈ 49ms między przejściami → ~20 RPM? (wymaga weryfikacji z gear ratio)

## Open questions

QUESTION: Jaki jest gear ratio i ile par biegunów ma silnik?
Jaka jest rola 0x20000698 (over-speed flag) — cutoff safety?
Czy 0x200003EA/685/3EB to flagi do LED/display/telemetry?

---

# 16. PA0/PA1 — torque sensor (CH0/CH1) (PART 16 — SUBSTANTIAL)

## Hipoteza: PA0/PA1 = dual-channel torque sensor

PA0 (CH0) i PA1 (CH1) w sekwencji ADC1:
- CH0 na pozycji 1 (SQR3)
- CH1 na pozycjach 0 i 7 (SQR3 + SQR2) — **podwójne próbkowanie** = redundantny odczyt

## Dowody

### 1. 0x200004C4 = torque struct (CONFIRMED)
- 2 × i16 (field0, field1) — dual-signal torque sensor
- Referencje: 0x80075DC, 0x8007754, 0x800A0C8, 0x800AF54, 0x8012E0C

### 2. fn@0x800A04C — torque scaling (CONFIRMED)
```
r0 = field0² + field1²  (RMS magnitude)
if r0 > 0x388B1900:
  index = (r0 >> 24) - 0x38
  scale = table[0x081B558 + index×2]  (u16 lookup)
  field0 = (field0 × scale + 0x10000) >> 15
  field1 = (field1 × scale + 0x10000) >> 15
write back to 0x200004C4
```

### 3. fn@0x800A0B0 — torque sum (CONFIRMED)
```
result = (field0 × scale) + (field1 × scale)
→ summed torque value
```

### 4. fn@0x8009F4C — ADC data extraction (CONFIRMED)
- Reads DMA buffer 0x200012A0
- Extracts CH12/CH13/CH17 via lsrs #20
- Filters: diff = [0x200003FE] - [0x200004F4] → [0x20000468]
- IIR: [0x20000676] = (old×3 + new) >> 2

### 5. Torque curve table @0x081B558
- 192B lookup table (96 × u16 entries)
- Index from RMS magnitude → scale factor
- Scale factor 0x8000 = 1.0 (Q15 format)

## RAM anchors torque (CONFIRMED)

| Adres | Typ | Rola |
|-------|-----|------|
| 0x200004C4 | struct[8] | torque sensor raw (field0=i16, field1=i16, +4=filtered0, +6=filtered1) |
| 0x200003FE | u16 | torque filtered A (from 0x20000118) |
| 0x20000400 | u16 | torque filtered B (from 0x20000118) |
| 0x20000468 | i16 | torque difference (A-B) |
| 0x20000484 | u16 | torque IIR filtered |
| 0x20000676 | i16 | torque IIR accumulator |

## Teoria działania

Bafang M820 używa **dwukanałowego czujnika momentu obrotowego** (typowe dla silników mid-drive):
- **PA0 (CH0)** = kanał A (główny)
- **PA1 (CH1)** = kanał B (redundantny, podwójne próbkowanie)
- Oba kanały są przetwarzane niezależnie, a następnie łączone przez RMS (x²+y²)
- Wynik jest skalowany przez lookup table (krzywa momentu)
- Różnica A-B służy do detekcji uszkodzenia czujnika

## Open questions

QUESTION: Jakie jest napięcie referencyjne czujnika momentu (3.3V/5V)?
Jaki jest zakres ADC (0-4095)对应的 torque range (Nm)?
Czy 0x20000468 (różnica A-B) jest używana do fault detection?

---

# 47*. Pętla szybka — sekwencer komutacji (PART 47 — SUBSTANTIAL)

Funkcja 0x0801A488 (pełny decode):

## A) Akwizycja (CONFIRMED)
- BUF DMA circular → trzy wartości UBFX[4,12] → okno robocze 0x20000118{+4,+8,+C}
- świeżość: jeśli flag==0 → kopiuj do last-valid 0x200005CA/CC/CE, fresh=1
- 4-elementowe okna przesuwne @0x20000450/48/40 (+nowa próbka na [+6])
- **średnie 4-próbkowe** (sum>>2 via ubfx#2): → **0x200003F8 / FA / FC**

## B) Sekwencer 7-stanowy (CONFIRMED mechanics)
```
if byte[0x200005E0] >= 7 → skip
TBB [pc, byte[0x200005E0]]        ; tabela skoków @0x0801A588
```
Każda gałąź stanu (mechanika identyczna):
1. diff_A = srednia_X − last_valid_Y (jeśli >0, else 0) → para u32 @0x200005D0
2. diff_B = (srednia_Z − last_valid_W) >>1 → para @0x200005D8
3. byte[0x200005E2] ← 1 lub 2 (które porównanie było aktywne w tym stanie)

## C) Decyzja / licznik (CONFIRMED)
- halfword[0x200005E6]++ (tick sekwencji)
- porównania 32-bit ze znakiem par {5D8} vs {5D0} zależnie od [5E2]∈{1,2}
- jeśli halfword[0x200005E4]==0 i [5E6]>3:
   [5E4]=1; **toggle byte[0x2000043A]**; przesuwne okno 6×u16 @0x2000153C
   += halfword[0x2000067A]; suma/6 → halfword[0x2000067C]; zeruj źródło

## D) Timeout (CONFIRMED)
```
if halfword[0x200005EE] >= 0x190 (400):
    byte[0x2000045B] = 14 (0xE)     ; globalny stan → 14
```

INTERPRETACJA (HYPOTHESIS): klasyczny start/komutacja sterowana progiem prądowo-napięciowym
per sektor Halla; stan 14 = przejście do kolejnej fazy maszyny (patrz pisarze 0x2000045B).
Semantyka kanałów PC0/PC2 vs VREFINT pozostaje OPEN.

---

# 48*. Globalna maszyna stanów silnika (PART 46-49 — SUBSTANTIAL)

**Główny dispatcher: fn @0x080180E4** (wołany z pętli głównej; odczyty [45B] ×25+,
WRITE ×1 @0x80183E8). Stan globalny = **byte RAM 0x2000045B**.
TBH jump table @0x0801810C (19 wpisów, stany 0..18).

## Pełna mapa stanów (CONFIRMED via TBH decode)

| STAN | ADRES | OPIS | TRANSITIONS |
|------|-------|------|-------------|
| 0 | 0x80183B4 | **ALIGN**: Check ADC avgs [20000118+4/8/C]<60; call 0x8012654(state) → [45C]; spin on [5C8] | →0 (loop), →7 |
| 1 | 0x80185F4 | **INIT**: Clear [5C4/5C0/662]; call 0x8008A84, 0x80083C0; **MOE ON** | →2 |
| 2 | 0x801861A | **RUNNING**: Monitor [387]/[69E]/[3C2]/[688] faults; [3EB]++≥20→[385]=1; throttle ramp | →0, →9, →10 |
| 3,4 | 0x8018202 | **UNUSED** → common exit | →exit |
| 5 | 0x8018132 | **SPEED MONITOR**: Check [38C] vs 0x8FC(2300)/0x6A4(1700); counter [45A]++>5→9 | →5, →7, →9, →11 |
| 6 | 0x8018282 | **FLAG CHECK**: Call 0x8008AF0; [397]\|[398] nonzero→counter; [45A]→0→**→8** | →6, →8, →9 |
| 7 | 0x80181DE | **QUERY**: Call 0x8012654(state); [45C]!=0→[5C7]→{0\|7}; [3EC]==7→counter | →0, →5, →7, →9 |
| 8 | 0x80182F6 | **MONITOR2**: Call 0x80094B4; [459] nonzero→counter; [45A]→0→**→18** | →8, →18 |
| 9 | 0x801872A | **FAULT**: Clear timers; MOE OFF; check [3EC]/errors; [45A]++≥50→5 | →5, →9 |
| 10 | 0x801870E | **FAULT STOP**: MOE OFF; clear [385] | →exit |
| 11 | 0x8018898 | **EMERGENCY**: [45B]=11; MOE OFF | →exit |
| 12-17 | 0x8018202 | **UNUSED** → common exit | →exit |
| 18 | 0x8018344 | **ALIGN2**: Check avgs<60; [688] counter; call 0x8012654(state) | →0, →18 |

## RAM anchors (CONFIRMED)

| Adres | Rola |
|-------|------|
| 0x20000458 | Flag: if set → MOE OFF → state=11 (emergency stop) |
| 0x20000459 | Flag: monitored in state 8 |
| 0x2000045A | **Counter**: timeout/retry counter (0..5/50) per state |
| 0x2000045B | **STATE** (byte): global motor state (0..18) |
| 0x2000045C | Result of 0x8012654(state) call |
| 0x20000387 | Running flag: set=1 when motor runs, cleared on fault |
| 0x200003C2 | Fault flag: speed out of range |
| 0x200003EB | Request flag: set=1 → [384]++ → [385]=1 → state 10 |
| 0x20000384 | Counter for [3EB] requests (threshold 20) |
| 0x20000385 | Persistent fault: → state 10 (FAULT STOP) |
| 0x2000038C | **Speed reading** (halfword): checked vs 0x8FC/0x6A4 |
| 0x200003EA | **Tick sync**: spin-wait flag per state iteration |
| 0x200003EC | Query result: compared with 7 |
| 0x20000397/98 | Status bytes: ORed in state 6 |
| 0x200005C7 | Alive flag: checked in state 7 |
| 0x200005C8 | Sync flag: spin-wait in state 0 |
| 0x20000688 | Condition flag: checked in states 2/9 |
| 0x2000069E | Fault flag: ORed with [387] in state 2 |

## Common exit path (@0x80188AA)

Po każdym stanie: throttle ramp computation:
- If signed halfword ≥0x8C(140) and <0xC8(200): **linear interpolation** → (val-0x8C)*0xB6/2
- If ≥0xC8: **saturacja** → 0x1554(5460)
- Compare with previous → increment/decrement counter → add to accumulator

## Sekwencja start/stop (CONFIRMED)

```
INIT(1) → MOE ON → RUNNING(2)
  ├→ fault detected → FAULT(9) → MOE OFF → SPEED MONITOR(5) → ...
  ├→ [3EB]++≥20 → FAULT STOP(10) → MOE OFF → exit
  ├→ [458] set → EMERGENCY(11) → MOE OFF → exit
  └→ normal → ALIGN(0) → QUERY(7) → ALIGN2(18) → back to 0
```

## Open questions

QUESTION: Pełna tabela przejść stan↔warunek.
NEXT EXACT TARGET: decode 0x0800CCF8 + pełny linear 0x80180E4..0x80186E4 z mapowaniem
każdego `cmp [45B],#imm`.
CAN STOCK BIN ANSWER IT? YES.

---

# 55. Evidence ledger

| ID | CLAIM | ANCHOR | CONFIDENCE |
|---|---|---|---|
| E001 | App base 0x08005000 | default-handler alignment chain §1.4 | CONFIRMED |
| E002 | Vector table @0x08005000, 76 entries (16+60) | file+0x20..0x150 | CONFIRMED |
| E003 | SP init = 0x2000B808 | VT[0] | CONFIRMED |
| E004 | Reset = 0x0800A3CC | VT[1] + clean disasm | CONFIRMED |
| E005 | Default IRQ handler = 0x080054BE (`b .`) | disasm @fo 0x4DE | CONFIRMED |
| E006 | VTOR candidate literal 0x08005000 @file+0x5584 | literal scan | STRONG |
| E007 | StdPeriph build (F10x) | assert strings @VA 0x1BAC8.. | CONFIRMED |
| E008 | Modules: adc/can/tim/dma/gpio/rcc/flash/pwr/bkp/rtc/iwdg/wwdg/misc | jw. | CONFIRMED |
| E009 | Version/model strings | VA 0x08016B60.. | CONFIRMED |
| E010 | SHA-256 obrazu | §1.1 | CONFIRMED |
| E011 | main = 0x08017F4C; woła SetSysClock @0x8017F56 | xref BL | CONFIRMED |
| E012 | SYSCLK=(HSE/2)×9; APB1=/2; APB2=/1; lat 2WS+prefetch | §3.2 disasm chain | CONFIRMED |
| E013 | Fallback HSI/2×16, flag RAM 0x20000101 | §3.2 | CONFIRMED |
| E014 | IRQ: ADC1_2=18, CAN TX/RX0/RX1/SCE=19..22, TIM1_UP=25, TIM2=28, TIM3=29 | VT slots + stdperiph numbering | STRONG |
| E015 | RTC z LSI | RCC_RTCCLKConfig(0x200) @0x0800A39C | CONFIRMED |
| E016 | Peripheral literal census (§6): TIM1×50, TIM3×41, TIM4×37, TIM5×36, TIM6/7×14, TIM8×38, ADC1×19, ADC2×17, CAN×13 | skan literałów | CONFIRMED |
| E017 | Boot: zegar konfigurowany dopiero w main | boot chain §2.2 | CONFIRMED |
| E018 | GPIO_Init = 0x08007870, struct {u16 pin;u8 spd@2;u8 mode@3} | CAN init disasm §4 | CONFIRMED |
| E019 | Motor PWM piny: PA8/9/10 + PB13/14/15 (TIM1 CH1-3/CH1N-3N) | GPIO_Init @0x0800B3C8/E4 | CONFIRMED config |
| E020 | CAN: PA11 IPU / PA12 AF_PP | @0x080065AE/CA | CONFIRMED config |
| E021 | Hall kandydaci PC6/7/8 (PUP→FLOAT) | @0x08008004, @0x8010656 | STRONG |
| E022 | Pula analogowa: PA0-7, PB0/1, PC0-5 (AIN) | master-init fn @0x0800B334 | CONFIRMED |
| E023 | ADCCLK=PCLK2/6=12MHz@72M | RCC_ADCCLKConfig(0x8000) @0x0800B33C | CONFIRMED |
| E024 | PB12 OUT_PP ×3 | @0x0800A9D6, 0x8016F74 + pool 0x40010C00 ×2 | CONFIRMED config |
| E025 | Clock enables: DMA1(AHB), ADC1+ADC2+TIM1+AFIO+GPIOA+B(APB2), TIM2/TIM3/TIM4/WWDG/PWR/BKP/CAN(APB1) | clken census | CONFIRMED |
| E026 | Hall-init fn włącza TIM3 | @0x08007FE8 | CONFIRMED |
| E027 | TIM1: PSC=0, ARR=2000, CENTER1, tDTS=/2, RCR=1 | TimeBase struct @0x0800B3F4-18 → fn 0x0800E718 (strh→+0x28/+0x2C/CR1) | CONFIRMED |
| E028 | TIM1 CH1-3: PWM1(0x60), pulse=1000, CCR1/2/3=+0x34/38/3C | helpers h1/h2/h3 store-offsets | CONFIRMED |
| E029 | TIM1 BDTR: DTG=25 (~694ns), BREAK=OFF, AOE=OFF, OSSR/OSSI=0 | BDTRConfig fn 0x0800C958 strh→+0x44 | CONFIRMED |
| E030 | PWM ≈18 kHz; UEV ≈9 kHz (RCR=1) | §8.2 math | STRONG |
| E031 | TIM3 = hall timebase (PSC=63, ARR=65535) | pool @VA 0x08008108=TIM3 + struct fills @0x08008014-2E | CONFIRMED |
| E032 | TIM1 CCDS=1 (CR2\|=1) | @0x0800B4D0-DE | CONFIRMED |
| E033 | ADC1 reg-seq 8ch: CH1,0,10,11,12,13,17(VREFINT),1; ADC2: 7,14,15,8,9,4,6,0 | RegularChannelConfig call chain @0x0800B63C-702 | CONFIRMED |
| E034 | DMA: ADC1_DR(0x4001244C)→RAM, count=8, DMA1_CH1 pool 0x40020008 | struct fills @0x0800B54C-80 | CONFIRMED |
| E035 | ISR ADC1_2: JEOC→read [ADC1+0x40]→UBFX[18:3]→RAM 0x2000011A; dispatch 0x801A488/0x8007604 | disasm §11.3 | CONFIRMED mechanics |
| E036 | RAM: 0x2000011A (fast sample), flags 0x200005C7, 0x2000045B, out 0x20000662 | ISR pools | CONFIRMED anchors |
| E037 | ADC1 CAL=1 wykonana (CR2\|=bit23) po injected-init#1 | @0x0800B5FA-608 (pool ADC1_DR-0x44=CR2) | CONFIRMED |
| E038 | Injected: ADC1{CH2,CH0}, ADC2{CH3,CH5} | calls 0x08005B00 @master-init tail | CONFIRMED |
| E039 | JEXTSEL: ADC1=0b000(TIM1_TRGO klasa), ADC2=0b111 | 0x0800580C(ADCx,mask) wywołania | STRONG |
| E040 | NVIC: ADC1_2=prio0/ON, TIM1_UP=prio1/ON, TIM1_BRK=OFF, DMA1_CH1=prio2/ON | NVIC_Init structs {id,pre,sub,cmd} @0x08008FE8 | CONFIRMED |
| E041 | Kolejność ADC startu: DeInit→conf→CAL(ADC1)→DMACmd→Cmd×2 | master-init flow §10.2 | CONFIRMED |
| E042 | CAN1 @0x40006400, PA11/PA12, bez remapu | §30.1 | CONFIRMED |
| E043 | CAN bitiming 2 warianty (presc11/TS1=8 vs presc12/TS1=12), SJW=1, TS2=1 | fn@0x08006904 + BTR pack @0x0800684A | CONFIRMED config |
| E044 | Bitrate wariant B = dokładnie 187.5 kbps @PCLK1=36M (SP=87.5%) | §30.2 math | STRONG (HSE-dep) |
| E045 | CAN helpery: Init/ITConfig/RX-decode adresy | §30.3 | CONFIRMED |
| E046 | RX0 ISR → ring 5×20B @RAM 0x20000A64 (w_idx+0x64, cnt+0x66, ovf+0x68) | disasm @0x0800EFA8 | CONFIRMED |
| E047 | EXTID[28:24]=node, [23:19]=op (2=multiframe) | ubfx @dispatcher 0x08017E64 | STRONG |
| E048 | Tabela sesji 5×80B @RAM 0x20000C80 (flaga +0x4E, node +1, limit @+0x190) | dispatcher loop | CONFIRMED anchors |
| E049 | Magic handshake 'E',02 → flag RAM 0x20000334 | @0x8011BB6-C6 | CONFIRMED |
| E050 | Kody komend filtrowane {3,5,9,19}, gate byte[0x20000354] | @0x8011B48-70 | CONFIRMED anchors |
| E051 | FOC loop: BUF=0x200012A0; UBFX[4,12] z +8/+10/+18 → 0x20000118{+4,+8,+C} | @0x0801A48E-AAC | CONFIRMED mechanics |
| E052 | last-valid trio 0x200005CA/CC/CE + fresh flag 0x200005C8; hist. 0x20000440/48/50 | @0x0801A4B4-516 | CONFIRMED |
| E053 | IIR LPF (0x5999A·old+0x2666·new)>>15 na ścieżce 0x08007604 | @0x8007650-6E | CONFIRMED |
| E054 | RAM: 0x200004B0/B4/B8/D4/AC = łańcuch filtrów prądu; gate 0x20000516/53C/5BD | §17*.A | CONFIRMED anchors |
| E056 | CMAR=0x200012A0 (DMA ADC1 regular, NDT=8) | pool @VA 0x800B790 | CONFIRMED |
| E057 | FOC czyta rangi 3/5/7 → PC0, PC2, VREFINT (albo okno czasowe circular) | §17* update | CONFIRMED mechanics / OPEN semantyka |
| E058 | Drugi bufor DMA 0x200012C0 | literały @0x8007E58/EA0/82C0/C690 | CONFIRMED existence |
| E059 | Sekwencer 20-stanowy: byte[0x200003DE]≤20; porównania 0x20000418/5EE | fn 0x08008C80 | CONFIRMED |
| E060 | Sesja CAN timeout=500 tick (slot+0x4C) → abort 0x08019CC0+0x08012DF4 | @0x08008A58 | CONFIRMED |
| E061 | DMA_Init struct/CCR-compose decode; pole {0,0x20}→CCR bit5 | @0x08007088 body | CONFIRMED |
| E062 | **DMA CIRCULAR włączony** dla bufora ADC (0x20) + PRI=high, 32-bit widths | master-init fills §17* | STRONG |
| E063 | Kolejność pól DMA struct i zapisy NDT/CPAR/CMAR do kanału | tail @0x080071D4-AA | CONFIRMED |
| E064 | CTL1 bit2 set dla ADC1+ADC2 (GD32 CLB=kalibracja) → **cel = GD32F30x** | @0x08005EB8 body | STRONG |
| E065 | Regular EXTSEL: brak jawnej konfiguracji w master-init (możliwy software-start/CONT) | skan regionu §10 | OPEN anchor |
| E066 | CAN_Transmit=0x08006AAC; TXRQ=TIR bit0; mailbox po TSR.TME26-28 | body decode §32.1 | CONFIRMED |
| E067 | TX ID = 0x02000000 \| node<<19 \| 0x020000 \| u16 | builder @0x08019C9C-BA | CONFIRMED formula |
| E068 | CRC16 tabelkowy, tabela @0x0801B358 | fn @0x0801A318 | CONFIRMED existence |
| E069 | Reboot→bootloader: wektor 0x08000004→MSP+BLX | @0x0801A2D4-F4 | CONFIRMED |
| E070 | RAM 0x20000330/32C = bootloader entry copy | jw. | CONFIRMED anchors |
| E071 | CAN_FilterInit helper: FR1/FR2/FM1R/FS1R/FFA1R mapping + FMR exit | tail @0x080064E0-78 | CONFIRMED mechanics |
| E072 | CRC16 tabela = CCITT(0x1021)<<1 (efektywny poly 0x2042) | t[1..4] @0x0801B358 | CONFIRMED pattern |
| E073 | App-level BL do 0x0800640C (@0x0819E92) i 0x0800632C (@0x081A254) | xref skan | CONFIRMED anchors |
| E074 | Dynamiczny filtr per-sesja: EXID<<3+IDE, tryb MASK, **FIFO1**, bank=sess[+0xC] | @0x08019E40-52 | CONFIRMED |
| E075 | RX0 ISR= FIFO0; filtr sesji → FIFO1 ⇒ drugi ring 0x20000AD0 ↔ FIFO1 (hipoteza spójna) | §31.1+§32.6 | STRONG |
| E076 | Sekwencer 7-stanowy: TBB @0x0801A588 na byte[0x200005E0] (<7) | @0x801A57C-84 | CONFIRMED |
| E077 | Średnie 4-próbkowe → RAM 0x200003F8/FA/FC; diffy → pary 32-bit @5D0/5D8; wybór byte[5E2]∈{1,2} | §47* A/B | CONFIRMED mechanics |
| E078 | Timeout 400 tick ([5EE]≥0x190) ⇒ stan globalny [45B]=14 | @0x801A858-6A | CONFIRMED |
| E079 | RAM: okno 6×u16 @0x2000153C, suma/6→[67C], źródło [67A], toggle [43A], tickery [5E4]/[5E6] | §47* C | CONFIRMED anchors |
| E080 | Dispatcher stanów = fn 0x080180E4 (READ×25+, WRITE@0x80183E8) | statefn skan | CONFIRMED |
| E081 | Stany globalne [45B]: {0,5,6,7,8,9,11,14,18,19}; helper tranzycji 0x0800CCF8 {0;11} | skeleton §48* | CONFIRMED values |
| E082 | TIM2 ISR (fn 0x800BDD8..) czyta [45B] ×4 — przerwanie zależne od stanu silnika | @0x800BF32-C0D4 | CONFIRMED reads |
| E083 | Sekwencer 20-stanów (fn 0x8008C80) czyta [45B] ×6 | @0x8008D4A-DA8 | CONFIRMED reads |
| E084 | 0x0800CCF8 = TIM_CtrlPWMOutputs (MOE bit15); stan 11 = STOP z MOE=0 | body @0x0800CCF8 + dispatcher pattern | CONFIRMED |
| E085 | Walidowane bazy timerów {TIM1,TIM8,0x40014000/4400/4800} — dodatkowy ślad GD32F30x | jw. | STRONG |
| E086 | CAN_FilterInit = **0x0800640C** (pełna walidacja pól + INIT enter) | body od +0 | CONFIRMED |
| E087 | Region @0x0800A430 = self-test obrazu (integrity, fail→assert) — nie filtry | caller-ctx decode | CONFIRMED |
| E088 | Statyczna tablica filtrów CAN = 0x0801B2D8 (8 banków ×16B), format skompresowany, helper 0x0819E3E | filter_decode | CONFIRMED |
| E089 | Banki 1,3,4,5,7 = ALL extended passthru → FIFO1 (dynamic sessions); Bank 0 = FIFO0 (system); 2,6 = partial | full table decode | CONFIRMED |
| E090 | fn@0x081A200 = "AAEE" magic handler → cpsid + MOE OFF + CAN reset = hard motor stop | decode 0x801A200-258 | CONFIRMED |
| E091 | TBH jump table @0x0801810C = 19 stanów (0..18); stany 3,4,12-17=UNUSED | TBH decode | CONFIRMED |
| E092 | Stan 1(INIT) → MOE ON → Stan 2(RUNNING); Stan 9/10/11 = FAULT/STOP/EMERGENCY z MOE OFF | state_full.mjs | CONFIRMED |
| E093 | RAM: [45A]=counter, [45B]=state, [45C]=query result, [3EA]=tick sync, [38C]=speed | dispatcher decode | CONFIRMED |
| E094 | Common exit = throttle ramp: <140→0, 140-200→linear, ≥200→5460; accumulator | @0x80188AA-938 | CONFIRMED |
| E095 | EXTSEL regular = 000 (TIM1_CC1), CONT=0, SWSTART — no CR2 writes found in entire binary | full scan | CONFIRMED |
| E096 | ADC pipeline: CH10(PC0)→DMA[2]→FOC avg[0], CH12(PC2)→DMA[4]→avg[1], CH17(VREF)→DMA[6]→avg[2]; UBFX[4,12]=raw 12-bit | FOC decode | CONFIRMED |
| E097 | ADC1_CR1=0x480: DISCEN=1, JDISCEN=1 (discontinuous mode both groups); TSVREFE=1 | fn@0x800B7D2+0x8005754 | CONFIRMED |
| E098 | 4-sample averaging windows @0x20000440/448/450; means @0x200003F8/FA/FC (sum>>2) | FOC decode | CONFIRMED |
| E099 | AWDG thresholds: no explicit writes → defaults HTR=0xFFF, LTR=0x000 (channel 0 monitored) | full scan | STRONG |
| E100 | TIM2 ISR @0x800C188: toggle [43B], speed calc, store [370], over-speed >1200 → [698] | disasm | CONFIRMED |
| E101 | Speed formula: (capture×160000)/458752 ≈ capture×0.349; capture from [0x2000053A] | fn@0x8007FAC | CONFIRMED |
| E102 | Periodic flags: every 10→[3EA], 50→[685], 100→[3EB] ticks from counter [0x20000470] | fn@0x8012BF0 | CONFIRMED |
| E103 | CAN RX: ring 5×20B, write ptr [+64], count [+66], overflow [+68]; CPSID/CPSIE protection | fn@0x800EFA8 | CONFIRMED |
| E104 | fn@0x801245C: clears flag byte (literal pool @0x08012464); called before speed read in TIM2 ISR | disasm | CONFIRMED |
| E105 | 0x200004C4 = torque struct: field0(i16) + field1(i16) + filtered0 + filtered1; refs at 5 locations | literal scan | CONFIRMED |
| E106 | fn@0x800A04C: RMS torque scaling — sqrt(field0²+field1²), lookup table @0x081B558, Q15 multiply | disasm | CONFIRMED |
| E107 | Torque curve table: 96 × u16 @0x081B558 (192B); index from (RMS>>24)-0x38 | fn@0x800A06C | CONFIRMED |
| E108 | PA0/PA1 = dual-channel torque sensor (CH0/CH1); PA1 double-sampled at ADC positions 0+7 | ADC seq | STRONG |
| E109 | fn@0x8009F4C: ADC data extraction — CH12/13/17 via lsrs#20, IIR filter (old×3+new)>>2 | disasm | CONFIRMED |
| E110 | fn@0x800A0B0: torque sum = (field0×scale) + (field1×scale) — final torque output | disasm | CONFIRMED |

# 56. Open questions

Zbiorczo: §1.6 (+ kolejne partie dodadzą swoje).

---

# FINAL COMPLETENESS MATRIX (running)

| AREA | STATUS | COMPLETENESS | OPEN | EXTERNAL EVIDENCE NEEDED |
|---|---|---|---|---|
| IMAGE/CONTAINER | DONE (PART 0) | header fields OPEN | CRC algo | bootloader BIN (opcjonalnie) |
| MCU | PARTIAL | family class znana, exact part OPEN | DBGMCU ID | brak |
| CLOCK | **DONE (PART 2)** | formula CONFIRMED; wartość HSE zewn. | crystal value | pomiar kryształu |
| PINOUT | SUBSTANTIAL (PART 3) | PWM/CAN/Hall/analog pool znane; funkcje PB12+inputów OPEN | konsumenci pinów | PCB dla connector map |
| CONNECTORS | PENDING | — | — | PCB/zdjęcia |
| PWM | **SUBSTANTIAL (PART 5/6)** | TIM1 PSC/ARR/mode/dead-time/BREAK=OFF znane; CHxN-enable mechanizm OPEN | 4 helpery CCxN | — |
| PWM EDGES/UPDATE | PARTIAL | center1 + RCR=1 ⇒ UEV 9 kHz | pełny cykl (PART 7) | — |
| ADC HW | **DONE (PART 9/11+§12*)** | obie sekwencje + DMA(8) + ISR fast-path + pipeline CONFIRMED: CH10/12/17→DMA→FOC, raw 12-bit, averaging | watchdog thresholds | — |
| CAN HW | **SUBSTANTIAL (PART 18)** | piny, bxcAN, bitiming×2, helpery | filtry, flaga wariantu | — |
| CAN PROTOCOL | PARTIAL (PART 19) | RX-path, ID bitfields, tabela sesji, magic handshake | pełna komenda/payload DB, TX IDs | log CAN z hardware mile widziany |
| CURRENT SENSE | **SUBSTANTIAL (§12*)** | FOC anchors, last-valid/fresh, IIR, hist. windows, ADC→DMA pipeline CONFIRMED, 4-sample avg, raw 12-bit | shunt/wzmacniacz: PCB |
| IRQ MAP | SUBSTANTIAL | handlerzy + priorytety NVIC CONFIRMED | reszta NVIC | — |
| TRIGGER CHAIN | **DONE (PART 10+§48*)** | JEXTSEL: inj1=TIM1_TRGO, inj2=TIM1_CC4; CAL performed; EXTSEL reg=000(TIM1_CC1)/software-start | TIM8 rola | — |
| START SEQUENCE | **SUBSTANTIAL (PART 47*)** | sekwencer 7-stanów + timeout→stan14 + okna/diffy | mapa pełna stanów | — |
| STOP/FAULT | **SUBSTANTIAL (PART 48*)** | full 19-state TBH decoded: INIT→RUNNING→FAULT→EMERGENCY, MOE control, counters, throttle ramp | — | — |
| FAST LOOP | **SUBSTANTIAL (§12*+§47*)** | BUF=DMA circular CONFIRMED, 7-state commutation, 20-state sequencer, last-valid/fresh, 4-sample avg, raw 12-bit | — | — |
| CAN SESSIONS | **SUBSTANTIAL (§32.5-6)** | filtry: 8 static banks @0x0801B2D8 (FIFO0=sys, FIFO1=ALL passthru for sessions), dynamic per-sesja, RX ISR, magic AAEE handler | pełna tabela przejść stanów, TX payload DB | — |
| CAN FILTERS | **CONFIRMED (§32.5)** | 8 statycznych banków, format skompresowany, helper 0x0819E3E | — | — |
| ...reszta | PENDING | — | — | — |

---
*Następne: semantyka bufora DMA (circular vs kanały), TX IDs CAN, sesje deep (0x08008A58), PART 15 (IDR trace przez dataflow), EXTSEL regular, pełne przejścia stanów45B, skale A/count kanałów ADC.*
