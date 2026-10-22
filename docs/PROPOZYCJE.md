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

### A. Spektralny resyntezator z losowymi fazami — **zrobione (AEther v0.1)**

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
| RAM | Bufory float dla okna 8192: FFT 64 KB + 2 × widmo 32 KB + odtwarzanie/ogon stereo 64 KB + okno i ćwiartka sinusa 24 KB + pierścień VA 32 KB ≈ **220 KB puli** (z 336; razem z ekranem ~278 KB) |
| Sample w RAM-ie się nie zmieści | Sample czytany bezpośrednio z flasha (XIP), 22,05 kHz, w ADPCM: do 24 s własnego materiału |
| Znany błąd CPU (mnożenia 64-bit obok 16-bitowych odczytów) | Silnik jest w całości na float, bez mnożeń 64-bitowych |

Dopisane w v0.2:

- **VA jako źródło na żywo** (`app/dsp/va.c`): jeden monofoniczny głos VA na C4 pisze do pierścienia 8192 próbek
  (32 KB), a silnik analizuje jego najnowsze okno co ramkę i przesuwa widmo na każdy klawisz, tak jak przy
  samplu. Głos VA na nutę nie miałby sensu: losowe fazy i tak zjadają dudnienia i detune, liczy się tylko
  widmo, a jeden głos kosztuje ~1–2 % CPU zamiast ośmiu.
- **Cztery wbudowane sample** (CHOIR, GLASS, STRING, WIND), generowane przy budowaniu.
- **Kompresja sampli (IMA ADPCM)**, opisana niżej.

Opóźnienie: nuta zaczyna brzmieć w następnej ramce, czyli po ~1–1,5 hopu (rozmiar 8K: ~100–140 ms, 1K: ~15–20 ms).
Dla tej estetyki to cecha, nie wada — ale gałka SIZE pozwala wybrać krótsze, bardziej „ziarniste” okno.

Szacunek obciążenia (z licznika pracy w testach, nie z pomiaru na sprzęcie): najgorszy przypadek (8 głosów × 3
warstwy oktaw, analiza sampla, SPREAD, okno 8K) to ~250 tys. „jednostek” (≈ motylek FFT / bin) na ramkę, czyli
~24 tys. na blok. Przy ~15–30 cyklach na jednostkę daje to **ok. 20–40 % rdzenia** w najgorszym razie, typowo
połowę tego. Stopka ekranu pokazuje prawdziwe CPU % — to pierwsza rzecz do sprawdzenia na urządzeniu.

### Kompresja sampli: dlaczego ADPCM, a nie MP3

| Format | Bity/próbkę | Dekodowanie | Dostęp swobodny | Ocena |
|---|---|---|---|---|
| PCM 16 bit | 16 | nic | tak | czysto, ale 6,3 s własnego materiału |
| **IMA ADPCM, bloki po 256** | **4,1** | ~10 operacji całkowitych na próbkę | **tak**, co blok | **wybrane**: 4× więcej (24 s), praktycznie zero CPU |
| MP3 / AAC / Opus | 1–2 | ramkowe, dziesiątki KB RAM-u na dekoder | słaby (ramki, rezerwuar bitów) | wykonalne (fm1-radio dekoduje MP3 i HE-AAC), ale kosztuje RAM, którego silnik potrzebuje, i komplikuje skakanie po samplu |
| Gotowe spektrogramy (magnitudy w ~512 pasmach logarytmicznych, 8 bit) | — | żadne, bez FFT analizy | tak | najciekawsze na przyszłość: 2 s ≈ 10 KB i o połowę mniej pracy CPU na ramkę, kosztem rozdzielczości widma |

Silnik czyta sample do przodu (z cofnięciem o jedną próbkę do interpolacji), więc dekoder ADPCM trzyma swój stan
i dekoduje tylko nowe próbki. Do początku bloku cofa się najwyżej o 255 próbek. Szum kwantyzacji ADPCM
(27–36 dB SNR) po rozmyciu fazy zmienia się w szeroki, cichy szum, a z PaulStretchem i tak wszystko staje się
szumem o kształtowanym widmie.

### Upscaling: SBR (zrobione w v0.3)

Pytanie było, czy da się trzymać sample w niskiej rozdzielczości i „ulepszać” je przy graniu. Opcje:

| Metoda | Co robi | Na FM-1 |
|---|---|---|
| **SBR** (HE-AAC, mp3PRO) | górne pasmo odtworzone z dolnego: transpozycja + obwiednia zapisana obok (setki bajtów/s) | **wybrane**: silnik i tak pracuje na widmie amplitud, fazy losuje, więc SBR kosztuje jedno przejście po binach na ramkę |
| Ślepe rozszerzenie pasma | górne pasmo z ekstrapolacji nachylenia widma, bez dodatkowych danych | prostsze, gorsze dla dźwięków o nietypowym widmie; dobre jako zapas dla sampli bez obwiedni (na później) |
| Bramka/odszumianie widmowe | odjęcie szacowanego szumu kwantyzacji ADPCM w każdym binie | tanie w naszym układzie; do rozważenia, jeśli szum ADPCM będzie słyszalny |
| Sieci neuronowe (AudioSR, NU-Wave, vocodery) | najlepsza jakość | poza zasięgiem: miliony parametrów, a mamy 336 KB RAM-u i brak akceleratora |

Jak to działa u nas: silnik zna częstotliwość sampla, więc wie, od którego binu (88 % Nyquista sampla) zaczyna
się dziura. Każdy bin powyżej wypełnia wartością z najwyższej oktawy sampla (indeks dzielony przez 2, aż wpadnie w
tę oktawę). Potem dla każdego z 8 pasm liczy energię wstawionego widma i skaluje je do energii z obwiedni,
interpolowanej w czasie wg pozycji skanu. Energia pasma (suma kwadratów amplitud) w skali silnika nie zależy od
rozmiaru FFT, więc obwiednię liczoną w Pythonie na oknie 2048 da się użyć przy każdym rozmiarze okna w silniku.

### B. Rozciąganie dźwięku z komputera na żywo (USB Audio) — następny krok

FM-1 nie ma wejścia audio, ale fm1-base obsługuje **USB Audio OUT** (komputer gra przez FM-1). Dziś ten strumień
jest miksowany po aplikacji i aplikacja go nie widzi. Gdyby `usb.c` oddawał próbki aplikacji, AEther mogłaby
zbierać je w pierścień (wystarczy jedno okno: 8192 × 2 B = 16 KB) i rozciągać na żywo — „PaulStretch jako efekt”.
Wymaga zmiany w fm1-base (warto zaproponować upstream).

### C. Własny syntezator jako źródło — zrobione jako VA

Pierwotny pomysł z nagrywaniem innego silnika do RAM-u zastąpiłem źródłem LIVE: syntezator gra wprost do
pierścienia, z którego silnik bierze najnowsze okno. Nie trzeba nic nagrywać, a każda zmiana gałki od razu
zmienia chmurę. Tą samą drogą da się dołożyć kolejne generatory (FM, wavetable, szum z filtrami grzebieniowymi).

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
