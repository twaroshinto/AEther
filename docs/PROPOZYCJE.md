# Co da się upchnąć w FM-1: analiza i propozycje

## Sprzęt (październik 2026, wg społeczności)

| | |
|---|---|
| SoC | JieLi AC7911B (rodzina AC791N / WL82), **dwa rdzenie pi32v2**, **FPU pojedynczej precyzji**, do 320 MHz (SDK) |
| Pamięć programu | 1 MB SPI NOR (XIP); slot aplikacji **581 KB**, obszar danych ~**276 KB** (na sample) |
| RAM | **96 KB** na `.data/.bss` + **336 KB** puli na duże bufory. **Brak zewnętrznego RAM-u** (fm1-base buduje z `CONFIG_NO_SDRAM_ENABLE`) |
| Audio | stereo 44,1 kHz, 24 bit, blok 256 próbek (5,8 ms); USB Audio (nagrywanie wyjścia FM-1, odtwarzanie z komputera) |
| Panel | 27 klawiszy z velocity (pojemnościowe), 7 enkoderów (SELECT, ALGO, PRESET, K1–K4), 14 przycisków, MASTER |
| Ekran | 240 × 240, kolor |
| MIDI | USB-MIDI (in/out), TRS in, Bluetooth MIDI (opcjonalnie, kosztem RAM-u) |
| Wejście audio | **brak** (ani mikrofonu, ani line-in) |

Źródła: [felucca-bsp](https://github.com/hugelton/felucca-bsp) (CPU.md, EXPERIMENTAL.md),
[fm1-base](https://github.com/charlesvestal/fm1-base) (README: budżety pamięci),
[aroum/fm1-custom-fw](https://github.com/aroum/fm1-custom-fw) (REVERSE_ENGINEERING.md).

## Jak działa PaulStretch i co z tego wynika dla FM-1

PaulStretch (Nasca Octavian Paul) bierze długie okno dźwięku (typowo 0,25–2 s), liczy FFT, **zachowuje tylko
amplitudy, a fazy zastępuje losowymi**, robi odwrotne FFT i nakłada ramki co pół okna. Pozycja odczytu w źródle
przesuwa się bardzo wolno — stąd „rozciągnięcie” × 10…× 10 000 bez artefaktów granularnych. Ten sam autor zrobił
PADsynth: widmo budowane wprost z harmonicznych rozmytych gaussowsko, też z losowymi fazami.

Wymagania obliczeniowe są skromne (jedno FFT na pół okna), ale **pamięciowe duże**: okno 8192 próbek to 32 KB na
bufor float, a potrzeba kilku buforów. Przy 336 KB puli (bez zewnętrznego RAM-u) to jest główne ograniczenie.

## Warianty

### A. Spektralny resyntezator z losowymi fazami — **zrobione (SMUGA v0.1)**

Jeden silnik, dwa źródła widma:

- **SAMPLE** — PaulStretch wprost: sample z flasha (wbudowany „chór” albo własny, wgrywany po USB-MIDI) skanowany
  w tempie `hop / STRETCH`. Klawisz przesuwa widmo (C4 = oryginał), więc ten sam sample gra polifonicznie.
- **PAD** — PADsynth: każda nuta to zestaw harmonicznych z gaussowskim pasmem (BAND w centach), z regulacją
  jasności, nieharmoniczności (struna sztywna) i wycięcia parzystych.

Sztuczki, dzięki którym to się mieści:

| Problem | Rozwiązanie |
|---|---|
| Polifonia = N razy FFT? | Nie: wszystkie głosy sumowane w **jedno widmo amplitud**, jedno odwrotne FFT na ramkę niezależnie od liczby nut |
| Stereo = 2 × FFT? | Nie: lewy i prawy kanał to dwa rzeczywiste sygnały spakowane w **jedno zespolone FFT** (Re = L, Im = R), każdy z własnymi losowymi fazami — naturalna, zdekorelowana szerokość |
| FFT 8192 w jednym przerwaniu audio = skok obciążenia | Ramka to maszyna stanów z małymi krokami; każdy blok 256 próbek wykonuje swoją część pracy (z zapasem ×1,5). Górne ograniczenie pracy ramki liczone z parametrów, więc nagła zmiana (FREEZE puszczony, nowe nuty, dłuższe okno) nie spóźnia ramki |
| RAM | Bufory float dla okna 8192: FFT 64 KB + 2 × widmo 32 KB + odtwarzanie/ogon stereo 64 KB + okno i ćwiartka sinusa 24 KB ≈ **188 KB puli** |
| Sample w RAM-ie się nie zmieści | Sample czytany bezpośrednio z flasha (XIP), 22,05 kHz: do 6,3 s własnego materiału |
| Znany błąd CPU (mnożenia 64-bit obok 16-bitowych odczytów) | Silnik jest w całości na float, bez mnożeń 64-bitowych |

Opóźnienie: nuta zaczyna brzmieć w następnej ramce, czyli po ~1–1,5 hopu (rozmiar 8K: ~100–140 ms, 1K: ~15–20 ms).
Dla tej estetyki to cecha, nie wada — ale gałka SIZE pozwala wybrać krótsze, bardziej „ziarniste” okno.

Szacunek obciążenia (z licznika pracy w testach, nie z pomiaru na sprzęcie): najgorszy przypadek (8 głosów × 3
warstwy oktaw, analiza sampla, SPREAD, okno 8K) to ~250 tys. „jednostek” (≈ motylek FFT / bin) na ramkę, czyli
~24 tys. na blok. Przy ~15–30 cyklach na jednostkę daje to **ok. 20–40 % rdzenia** w najgorszym razie, typowo
połowę tego. Stopka ekranu pokazuje prawdziwe CPU % — to pierwsza rzecz do sprawdzenia na urządzeniu.

### B. Rozciąganie dźwięku z komputera na żywo (USB Audio) — następny krok

FM-1 nie ma wejścia audio, ale fm1-base obsługuje **USB Audio OUT** (komputer gra przez FM-1). Dziś ten strumień
jest miksowany po aplikacji i aplikacja go nie widzi. Gdyby `usb.c` oddawał próbki aplikacji, SMUGA mogłaby
zbierać je w pierścień (wystarczy jedno okno: 8192 × 2 B = 16 KB) i rozciągać na żywo — „PaulStretch jako efekt”.
Wymaga zmiany w fm1-base (warto zaproponować upstream).

### C. Resampling własnego dźwięku FM-1

Nagranie wyjścia innego silnika (np. prostego FM) do RAM-u i rozciągnięcie go. W RAM-ie po silniku zostaje ~90 KB
→ ok. 2 s przy 22 kHz/16 bit. Dłużej tylko przez flash (wolny zapis, zużycie). Ciekawe, ale mniej opłacalne niż B.

### D. Drugi rdzeń

felucca-bsp ma (beta) start drugiego rdzenia. FFT można by liczyć tam, zostawiając pierwszy rdzeń na UI i audio,
a zaoszczędzony czas oddać na większe okno albo pół-częstotliwościowy silnik (E). Na razie niesprawdzone na
urządzeniach — dopiero po pomiarach CPU z wariantu A.

### E. Silnik w 22,05 kHz

Liczenie ramek w połowie częstotliwości i podwójne próbkowanie na wyjściu: to samo 32 KB okna daje 372 ms
zamiast 186 ms (gładsze, bliżej oryginalnego PaulStretcha), kosztem pasma do 11 kHz. Dla padów to rozsądny wybór;
dobry kandydat na opcję w kolejnej wersji.

## Ryzyka

- **Nic z tego nie było jeszcze uruchomione na prawdziwym FM-1.** Silnik i cała aplikacja przechodzą testy w
  symulatorze fm1-base, ale budżet CPU, czas zapisu flasha i zachowanie USB trzeba sprawdzić na sprzęcie.
- Instalacja custom firmware jest na własne ryzyko; powrót: oficjalny updater M-VAVE albo tryb aktualizacji
  (oba przyciski OCT trzymane 5 s).
