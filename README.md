# AEther

Spektralny syntezator dla **M-VAVE FM-1**, działający na zasadzie [PaulStretcha](https://hypermammut.sourceforge.net/paulstretch/):
długie okna, zachowane amplitudy widma, **losowe fazy** i nakładanie ramek. Z każdej nuty robi się gęsta,
nieskończona chmura. Custom firmware na [fm1-base](https://github.com/charlesvestal/fm1-base) /
[felucca-bsp](https://github.com/hugelton/felucca-bsp).

> **Status: v0.5, nietestowane na sprzęcie.** Silnik i cała aplikacja przechodzą testy w symulatorze fm1-base
> (te same źródła co firmware), a CI buduje plik `.fwsc` toolchainem JieLi. Na prawdziwym FM-1 jeszcze nie grało:
> pierwsze uruchomienie to test, nie występ. Instalujesz na własne ryzyko.

![panel](docs/img/manual/panel_numbered.png)

**[Instrukcja obsługi](docs/MANUAL.md)**: każda gałka, strona i preset, ze zrzutami z emulatora.
**English:** a spectral "PaulStretch" synth for the FM-1, see the **[user manual in English](docs/MANUAL.en.md)**.

## Jak to brzmi i jak działa

Pięć rodzajów źródeł widma, jeden silnik:

- **Sample** (16 wbudowanych + USER): sample skanowany bardzo powoli (STRETCH od 1x do ∞), czyli PaulStretch
  wprost. Wbudowane są syntetyzowane przy budowaniu (`app/tools/gen_source.py`), a własne wgrywasz po USB-MIDI.
  Klawisz przesuwa widmo (C4 = oryginalna wysokość), więc jeden sample gra akordami.
- **VA**: oscylator wirtualnie analogowy (saw → square → pulse, wolne LFO z dryfem) grający bez przerwy C4.
  Silnik co ramkę rozmywa jego najnowsze okno i przenosi na wysokość każdego klawisza. Filtr (24 dB/okt, z
  rezonansem) działa na widmie po zsumowaniu głosów, więc ma to samo odcięcie dla każdego klawisza: wysokie nuty
  nie robią się ostrzejsze, a harmoniczne są lekko rozmyte w pasma jak w PADsynth.
- **PAD**: [PADsynth](https://zynaddsubfx.sourceforge.io/doc/PADsynth/PADsynth.htm) tego samego autora: każda
  harmoniczna nuty to gaussowskie pasmo w widmie (szerokość BAND w centach). Nie potrzebuje żadnego materiału
  wejściowego: widmo powstaje od razu w dziedzinie częstotliwości.
- **HARM**: generator harmonicznych rysowany wprost w widmie: liczba harmonicznych (1–64), nachylenie, balans
  parzyste/nieparzyste, uwypuklenie lub wycięcie wokół wybranej harmonicznej, barwa zależna od dynamiki, i
  **cztery obwiednie** (ATTACK, DECAY, SUSTAIN), każda przypisana do jednej harmonicznej albo grupy (ODD, EVEN,
  LOW, MID, HIGH, ALL).
- **LIVE VOX, AIR, BOW, BELL**: generatory grające na żywo (głosy z formantami, wiatr na rezonansach, struny
  Karplusa-Stronga pod „smyczkiem”, pole dzwonów modalnych). Jak w intrach 64k: przepis zamiast danych, kilkaset
  bajtów kodu każdy, bez pamięci na sample, bez pętli, same powoli się zmieniają.

Do tego **matryca modulacji**: 8 slotów „źródło → cel × siła (±100 %)”. Źródła to trzy LFO (7 kształtów, w tym
losowy i dryf, z FADE i SYNC do nuty), dwie obwiednie ADSR, dynamika, kółko modulacji (CC1), nacisk klawiszy i
aftertouch, losowa wartość na nutę i wysokość nuty. Celów jest 27, w tym tempo innego LFO. Jedno źródło może
poruszać kilkoma parametrami, a jeden parametr słuchać kilku źródeł.

| Wbudowane sample | |
|---|---|
| CHOIR, BOWED, FLUTE, BRASS, WHISPR | głosy i instrumenty z formantami: chór na samogłoskach, wiolonczela, flet z oddechem, sekcja dęta, szept |
| PIANO, STRING, KALIMB, GLASS, GAMELN | uderzane i szarpane: fortepian (nieharmoniczność, podwójne struny), Karplus-Strong, kalimba, szkło, gamelan z ombak |
| ORGAN, DRONE, TAPE | organy (rejestry), tanpura z brzękiem jawari, elektryczne pianino z taśmy (wow, flutter, szum) |
| WIND, RAIN, BIRDS | wiatr, deszcz z kroplami, ptaki |

Każdy ma 2,5–4 s, jest syntetyzowany w 44,1 kHz, a trzymany w **11 kHz jako IMA ADPCM + obwiednia SBR**
(~16 KB na 3 s, razem 270 KB flasha). Pasmo powyżej 4,9 kHz silnik odbudowuje przy graniu (SBR, niżej). Jako
WAV-y 44,1 kHz do odsłuchu lub edycji: `python3 app/tools/gen_source.py --out /tmp/s.h --wav-dir starter/` (CI
publikuje je też jako artefakt `starter-samples`).

### SBR: mało w pamięci, pełne pasmo na wyjściu

Sample może leżeć w pamięci w niskiej częstotliwości (11 albo 22 kHz), a silnik odbudowuje brakujące górne
pasmo jak **SBR (Spectral Band Replication)** z HE-AAC. Bierze najwyższą oktawę, którą sample ma, transponuje ją
w górę o oktawy (harmoniczne zostają harmonicznymi) i skaluje do zapisanej obwiedni: 8 pasm co 50 ms, ~170 bajtów
na sekundę. W zwykłym kodeku słabością SBR są fazy i transjenty, ale PaulStretch i tak losuje fazy, więc tutaj
ta metoda nie ma praktycznie wad. Test (`tests/dsp_test.c`): STRING trzymany w 11 kHz odbiega od pełnopasmowego
oryginału w paśmie 5–16 kHz średnio o 2,0 dB z SBR, a bez SBR o 14,4 dB.

## Obsługa

Skrót (pełna wersja w [docs/MANUAL.md](docs/MANUAL.md), po angielsku [docs/MANUAL.en.md](docs/MANUAL.en.md)):
**ALGORITHM** wybiera rodzaj źródła (SAMPLE, LIVE, PAD, HARM, VA), **SELECT** który sample albo generator, **PRESETS** preset, **KNOB 1–4** to cztery parametry bieżącej strony. Strony są pod
przyciskami: **EDIT** (źródło; przy HARM też TONE i HARM ENV A–D), **ENV** (AMP, MOD ENV 1–2), **LFO** (LFO 1–3,
MATRIX), **SEQ** (EVOLVE: ruch w czasie), **FX** (COLOR), **GLO** (SETUP). Przycisk wraca na ostatnio oglądaną stronę
swojej grupy, a kolejne naciśnięcie przechodzi dalej. **HOME** wraca na stronę główną (na niej wycisza), a
przytrzymany pokazuje mapę przycisków. Przytrzymanie **LFO** albo **ENV** i kręcenie gałką przypisuje modulację.
**PLAY** to HOLD, **REC** to FREEZE, **SAVE** zapisuje. MIDI: nuty, CC64 sustain, CC1 i aftertouch jako źródła
matrycy (WHEEL, PRESSURE), CC71 SPREAD, CC74 TILT.

## Moc obliczeniowa i drugi rdzeń

Silnik dzieli pracę każdej ramki na małe kroki rozłożone na kolejne bloki audio, więc nawet przy ośmiu głosach
żaden blok nie dostaje całej ramki naraz. Widma HARM i PAD są rysowane z tablicy zamiast `expf`, a harmoniczne
cichsze niż −120 dB są pomijane. Na hoście skróciło to rysowanie HARM o ~45 %, a PAD o ~30 %.

**Bezpiecznik przeciążenia** (zawsze włączony): gdy blok audio zajmuje ponad 90 % czasu, AEther wyłącza SUB i
SHIMMER, a potem zmniejsza liczbę głosów, nawet do dwóch. Po trzech sekundach spokoju wraca krok po kroku.

**Drugi rdzeń** (SETUP → 2ND CORE, **eksperymentalny**, domyślnie wyłączony): rdzeń 1 maluje widma połowy głosów,
podczas gdy rdzeń 0 czeka w przerwaniu audio. Rdzeń startuje tylko przy włączeniu zasilania. Każde zadanie ma
słowo kontrolne, a spóźniona albo błędna odpowiedź wyłącza rdzeń do wyłączenia zasilania (dźwięk gra dalej na
jednym). Działanie sprawdzone tylko w testach na hoście (wynik identyczny jak na jednym rdzeniu); na urządzeniu
jeszcze nie.

## Emulator w przeglądarce

Cały firmware (panel, ekran, dźwięk) działa też w przeglądarce: ten sam kod, skompilowany do WebAssembly przez
Emscripten i grany w AudioWorklet (emulator z fm1-base). Klawiatura komputera: `Q`–`I` i `A`–`K` to białe
klawisze, cyfry to czarne, `Z`/`X` zmieniają oktawę, `P` to HOLD, `O` to FREEZE. Shift-klik trzyma klawisz
(akordy myszką).

```sh
./build.sh --web                                  # app/build/emu/ (wymaga emcc)
cd app/build/emu && python3 -m http.server 8000   # http://localhost:8000
```

CI buduje go przy każdym pushu (artefakt `aether-emulator`).

## Własny sample

```sh
pip install numpy mido python-rtmidi
python3 tools/aether_load.py moj_dzwiek.wav                 # mono, 22,05 kHz, ADPCM + SBR: do 24 s
python3 tools/aether_load.py moj_dzwiek.wav --rate 11025    # 4,9 kHz w pamięci, reszta z SBR: do 48 s
python3 tools/aether_load.py moj_dzwiek.wav --pcm16         # 16 bit bez szumu ADPCM: do 6,3 s
python3 tools/aether_load.py moj_dzwiek.wav --rate 44100    # pełne pasmo, bez SBR: do 12 s (3,1 s z --pcm16)
python3 tools/aether_load.py dlugi.wav --start 12 --secs 5  # fragment
python3 tools/aether_load.py moj_dzwiek.wav --syx out.syx   # plik .syx dla SysEx Librarian / MIDI-OX
```

Sample trafia do flasha (przetrwa wyłączenie) i od razu staje się źródłem USER. ADPCM ma odstęp sygnału od szumu
ok. 27–36 dB: po rozmyciu fazy szum kwantyzacji zamienia się w cichy „oddech” pod chmurą, co zwykle nie
przeszkadza. Do czystych, cichych materiałów użyj `--pcm16`. Przerwane wgrywanie niczego nie
psuje: nagłówek jest kasowany na początku i zapisywany dopiero po sprawdzeniu CRC.

## Budowanie

```sh
git clone --recursive https://github.com/twaroshinto/AEther && cd AEther
sh tests/run.sh          # testy silnika + cała aplikacja w symulatorze (cc, python3, Pillow, numpy)
./build.sh --host        # sam symulator: app/build/host/aether_host SKRYPT KATALOG
./build.sh               # firmware: app/build/aether.fwsc
```

Firmware wymaga toolchaina JieLi (Linux x86-64) i trzech plików z SDK AC79:

```sh
sh base/tools/get_toolchain.sh ~/.jieli
sh base/bsp/tools/get_sdk.sh ~/.jieli/ac79-sdk
JIELI_TOOLCHAIN=~/.jieli/toolchain AC79_SDK=~/.jieli/ac79-sdk ./build.sh
```

Najprościej: **GitHub Actions** (`.github/workflows/build.yml`) robi to samo i wystawia `aether.fwsc` jako artefakt
każdego pusha; tag `vX.Y` tworzy release.

## Instalacja i powrót

```sh
python3 base/tools/fm1_install.py app/build/aether.fwsc     # przez USB-MIDI (mido + python-rtmidi)
```

Powrót do oficjalnego firmware: updater M-VAVE (albo „Return to official” w instalatorze Felucci). Jeśli coś
pójdzie źle, przytrzymanie **OCT− i OCT+ przez 5 s** wchodzi w tryb aktualizacji, a
`base/bsp/tools/fm1_rescue.sh` (macOS) przywraca oryginał na FM-1, które nie startuje.

## Struktura

| Ścieżka | Co |
|---|---|
| `app/dsp/aether.c`, `aether.h` | silnik: przenośne C99, float, bez alokacji i bez libc; dekoder ADPCM |
| `app/dsp/va.c`, `va.h` | źródło VA: oscylator polyBLEP i MOTION (filtr działa w widmie, w `aether.c`) |
| `app/app.c`, `app/app.h` | aplikacja FM-1: panel, ekran, MIDI, presety, matryca modulacji, bezpiecznik przeciążenia, wgrywanie sampli (SysEx) |
| `app/hw/core1.c`, `core1.S` | drugi rdzeń (eksperymentalny): start, skrzynka zadań, nadzór |
| `app/fm1.toml` | ustawienia builda fm1-base |
| `app/tools/gen_source.py` | generator wbudowanych sampli (przy budowaniu, bez nagrań i licencji) |
| `app/tools/aet_adpcm.py` | koder IMA ADPCM w formacie silnika (generator i `aether_load.py`) |
| `app/tools/aet_sbr.py` | resampling i obwiednia SBR (numpy) |
| `app/tests/*.txt` | scenariusze symulatora: klawisze, strony, HOLD, FREEZE, MIDI, zapis, wgrywanie sampla |
| `tests/dsp_test.c` | testy silnika: wysokość dźwięku, poziomy, cisza po release, brak spóźnionych ramek, budżet pracy |
| `tools/aether_load.py` | wgrywanie WAV-ów po USB-MIDI |
| `base/` | fm1-base (submoduł) |

## Licencja i podziękowania

MPL-2.0, jak fm1-base. Pomysł losowych faz na długich oknach i profil PADsynth pochodzą od Nasci Octaviana Paula
(PaulStretch, PADsynth). **Kod AEther jest napisany od zera i nie zawiera kodu PaulStretcha** (który jest na GPL).
Platforma: fm1-base (Charles Vestal) i felucca-bsp (Leo Kuroshita, Hügelton Instruments). Projekt nieoficjalny,
niezwiązany z M-VAVE ani JieLi.
