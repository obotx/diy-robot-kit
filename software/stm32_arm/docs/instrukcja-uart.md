# STM32 ramienia: instrukcja komend UART

## 1. Wybór profilu programu

**Obecny firmware testowy: pojedynczy ZL na stole** (`ARM_STANDALONE_ZL=1`,
`ARM_SERIAL_SERVICE_MODE=1`, `ARM_BENCH_OUTPUTS=1`). ZR i X nie wykonują ruchów.
Dotyczy pliku `tmp/stm32-standalone-zl-build/obotx_arm_f446re.elf` oraz obecnej
konfiguracji CubeIDE Debug. Nie potwierdza to, jaka wersja jest aktualnie wgrana.
Komenda `INPUTS` powinna zwrócić `standalone_zl=1`.

Istnieją też oddzielne profile:

- Ogólny tekstowy tryb serwisowy: implementuje JOG/MOVE i konfigurację opisaną
  w sekcji 7. Te polecenia są **zablokowane w obecnym profilu ZL**.
- Tryb JSON dla webserwera: inny protokół, nie przyjmuje poniższych komend
  tekstowych. Opis: [połączenie STM32](../../rpi/docs/stm32-link.md). Gotowość ruchu zależy
  też od konfiguracji uruchomieniowej, bazowania i zabezpieczeń.
- Tryb pomiarowy DMA: przyjmuje `SCOPE ...`; opis:
  [high-speed-step.md](high-speed-step.md). Nie służy do jazdy silnikiem.

## 2. Połączenie i jednostki

- Port szeregowy ST-LINK, dotychczas COM3; numer może się zmienić.
- **115200 baud, 8 bitów, bez parzystości, 1 bit stopu, bez kontroli przepływu.**
- Jedna komenda na linię, zakończenie LF (`\n`) lub CRLF (`\r\n`).
- Wielkie litery poleceń; spacja między argumentami. Pisz `EN 1`, nie `EN1`.
- Argumenty liczbowe są całkowite, dziesiętne, bez jednostek i przecinków.
- Pozycja i dystans: **um**; `1000` = 1 mm, `20000` = 20 mm.
- Prędkość: **um/s**; `10000` = 10 mm/s, `200000` = 200 mm/s = 20 cm/s.
- Otwórz port tylko w jednym programie naraz. Zamknij monitor Arduino/PuTTY
  przed uruchomieniem skryptu Python.

## 3. Komendy dostępne teraz

Nawiasy kwadratowe oznaczają argument opcjonalny; nie wpisuj nawiasów.

| Komenda | Działanie |
| --- | --- |
| `STATUS` lub `?` | Odczyt stanu i pozycji. |
| `INPUTS` lub `IO` | Odczyt krańcówek, alarmów i flag profilu. |
| `HB` lub `PING` | Heartbeat, podtrzymanie komunikacji. |
| `EN 1` | Włączenie sterownika ZL, jeżeli warunki na to pozwalają; bez ruchu. |
| `EN 0` | Zatrzymanie i wyłączenie napędu. |
| `STOP` | W profilu pojedynczego ZL: zatrzymanie i wyłączenie napędu. |
| `CLR` lub `CLEAR` | Skasowanie zatrzaśniętego błędu; pozostawia napęd wyłączony. |
| `HOME ZL` | Bazowanie pojedynczego ZL; samo włącza, a po zakończeniu wyłącza napęd. |
| `TRAVEL ZL dystans [predkosc]` | Ruch względny ZL; samo włącza i po zakończeniu wyłącza napęd. |

`CLR` nie naprawia przyczyny błędu: przy aktywnym problemie błąd wróci.
Po błędzie znaczniki bazowania są zerowane; skasowanie błędu nie przywraca bazowania.
Wyłączenie napędu może zwolnić silnik: obecny profil jest do prowadnicy na stole,
nie do podtrzymywania pionowego, obciążonego ramienia.

### TRAVEL: przykłady i ograniczenia

```text
TRAVEL ZL 20000 10000
```

Przejazd o **+20 mm** z zadaną prędkością **10 mm/s**.

```text
TRAVEL ZL -10000 1000
```

Przejazd o **-10 mm** z prędkością **1 mm/s**.

- Dystans różny od zera, maksymalnie +/-20000 um; musi odpowiadać całkowitej
  liczbie impulsów. Całkowite milimetry spełniają ten warunek.
- Prędkość 1..250000 um/s. Pominięcie lub wartość 0 wybiera domyślne 1000 um/s.
- Powyżej 1000 um/s wymagane bazowanie, dodatni dystans i pozycja docelowa
  w pierwszych 80 mm od zera. To ograniczenia testu na stole, nie generatora.
- Przed komendą: `en=0`, `fault=0`, `mode=0`, poprawne wejście POWER_OK,
  nieaktywne krańcówki i alarmy, brak trwającego ruchu.
- Nie wysyłaj `EN 1` przed `TRAVEL` ani `HOME ZL`: wymagają wyłączonego napędu.
- Limit STEP wynosi 160 kHz; dla Z 128 kHz odpowiada 200 mm/s. Częstotliwość
  dotyczy impulsów w blokach DMA; przerwy między blokami obniżają średnią.
- Przyspieszenie nadal działa; krótki przejazd może nie osiągnąć zadanej
  prędkości. Koniec testu TRAVEL jest zatrzymaniem na budżecie impulsów,
  nie pełnym profilem łagodnego hamowania. Zwiększaj prędkość stopniowo.

## 4. Heartbeat i odpowiedzi

Po włączeniu napędu wysyłaj **`HB` co około 150-250 ms**.
Po 1000 ms bez odświeżenia watchdog zatrzymuje napęd i zgłasza błąd 11.
`STATUS` i `INPUTS` **nie odświeżają** watchdoga. Udane EN/JOG/MOVE/HOME oraz
uruchomienie TRAVEL także odświeżają czas, ale nie zastępują regularnego HB.
Nie wykonuj długich ruchów przez ręczne wpisywanie heartbeatów.

Typowa odpowiedź na zaakceptowaną komendę:

```text
OK
STAT en=... fault=... code=... ...
```

`OK` oznacza przyjęcie, **nie zakończenie ruchu** ani fizyczną sprawność napędu.
`INPUTS` odpowiada linią `IO ...`, bez osobnego `OK`.

| Odpowiedź | Znaczenie |
| --- | --- |
| `ERR parse` | Nieprawidłowa składnia, argumenty lub niedostępna postać polecenia. |
| `ERR command` | Polecenie rozpoznane, ale nie spełnia warunków wykonania. |
| `ERR standalone_use_TRAVEL` | Komenda zablokowana w profilu pojedynczego ZL. |
| `ERR standalone_home_precondition` | Niespełnione warunki rozpoczęcia bazowania. |
| `ERR standalone_travel_precondition` | Niespełnione warunki przejazdu lub jego limity. |
| `ERR bench_step_rate` | Zadana prędkość wymaga zbyt wysokiej częstotliwości STEP. |
| `ERR line_too_long` | Przepełnienie linii; napęd zostaje wyłączony. |

Po błędzie sprawdź `STATUS` i `INPUTS`; nie kasuj błędu automatycznie w pętli.

## 5. Jak czytać STATUS i INPUTS

| Pole STAT | Znaczenie |
| --- | --- |
| `en` | Logiczne włączenie napędu: 0/1; nie jest pomiarem napięcia ENA. |
| `fault`, `code` | Flaga błędu i jego kod. |
| `axis` | Oś błędu: 0=ZL, 1=ZR, 2=X. Istotne przy `fault=1`. |
| `mode` | 0=spoczynek, 1=JOG/TRAVEL, 2=MOVE, 3=bazowanie. |
| `zl`, `zr`, `x` | Pozycje w um, obliczone z impulsów, nie odczytane z enkoderów. |
| `h_zl`, `h_zr`, `h_x` | Czy dana oś jest zbazowana. |
| `t_zl`, `t_zr`, `t_x` | Cel MOVE w um; -1 oznacza brak celu MOVE, również podczas TRAVEL. |
| `lim_zl`, `lim_zr`, `lim_x` | Górne limity programowe w um; nie pomiar rozstawu krańcówek. |
| `mv_zl`, `mv_zr`, `mv_x` | Domyślne prędkości MOVE w um/s; nie aktualna prędkość. |
| `f_zl`, `f_zr`, `f_x` | Zadane częstotliwości STEP w **mHz**; 128000000 = 128 kHz. Nie pomiar oscyloskopowy. |
| `home=a,b,c` | Fazy bazowania kolejno ZL, ZR, X. |

Fazy `home`: 0=brak, 1=start, 2=zejście z czujnika, 3=szukanie MIN,
4=odjazd, 5=powolny ponowny najazd, 6=końcowe zejście/odjazd, 7=gotowe.
Po zakończeniu faza może wrócić do 0; potwierdzeniem sukcesu jest `h_zl=1`,
`fault=0`, `mode=0`, `en=0`, `zl=0`.

W `IO` trójki `zl`, `zr`, `x` to kolejno **MIN, MAX, ALARM**, gdzie 1 oznacza
aktywne wejście. `filtered` podaje MIN/MAX po filtracji dla ZL, ZR, X.
`power=1` oznacza stan wejścia POWER_OK; przy zworce nie dowodzi obecności
zasilania sterownika. `bench`, `standalone_zl`, `skew_um` opisują profil testowy.

### Kody błędów

| Kod | Znaczenie |
| --- | --- |
| 0 | Brak błędu. |
| 1 | Napęd niewłączony. |
| 2 | Alarm sterownika silnika. |
| 3 | Ograniczenie krańcowe. |
| 4 | Limit programowy pozycji. |
| 5 | Zbyt duża różnica liczonych pozycji ZL/ZR. |
| 6 | Brak potwierdzenia POWER_OK. |
| 7 | Błędne polecenie. |
| 8 | Przekroczony czas bazowania. |
| 9 | Nieprawidłowy przebieg bazowania/czujnik/budżet drogi. |
| 10 | Generator STEP, DMA lub niepewna liczba impulsów. |
| 11 | Timeout komunikacji. |

## 6. Zalecana obsługa przez skrypt PC

Skrypt obsługuje heartbeat i zakończenie testu. Potwierdzenie
`--standalone-actuator` oznacza pojedynczą, zabezpieczoną prowadnicę na stole,
wolną drogę i dostępne odcięcie zasilania.

```powershell
Set-Location -LiteralPath '<repository-root>'
.\software\rpi\.venv\Scripts\python.exe .\software\stm32_arm\tools\bench_serial.py --port COM3 home-zl --standalone-actuator
```

Przed bazowaniem sprawdź oba czujniki. ZL ma stać po nieaktywnej stronie MIN,
w odległości nie większej niż 50 mm, z MIN w kierunku ujemnym. Nie zaczynaj
z aktywnym czujnikiem. Zero zostaje ustalone po końcowym odjeździe od MIN,
nie dokładnie na krawędzi jego załączenia. Homing nie mierzy całego skoku do MAX.

Przejazd +20 mm z zadaną prędkością 10 mm/s po poprawnym bazowaniu:

```powershell
.\software\rpi\.venv\Scripts\python.exe .\software\stm32_arm\tools\bench_serial.py --port COM3 travel --mm 20 --speed-mm-s 10 --standalone-actuator
```

Tutaj argumenty skryptu są w **mm i mm/s**, inaczej niż surowe komendy UART.
Po usunięciu przyczyny błędu można jawnie wykonać:

```powershell
.\software\rpi\.venv\Scripts\python.exe .\software\stm32_arm\tools\bench_serial.py --port COM3 clear
```

## 7. Pozostałe zaimplementowane komendy: inny profil serwisowy

**Poniższe komendy nie wykonają ruchu/konfiguracji w obecnym profilu ZL.**
Wymagają firmware bez `ARM_STANDALONE_ZL` i poprawnie przygotowanych wyjść.
Nie zmieniaj profilu tylko po to, żeby ominąć warunki testu.

`A` oznacza ZL, ZR, X lub Z; Z steruje oboma napędami pionowymi.

| Składnia | Znaczenie |
| --- | --- |
| `JOG A v` | Ruch z zadaną prędkością v w um/s; znak wyznacza kierunek, 0 zadaje zatrzymanie przez rampę. |
| `MOVE A p [v]` | Ruch do **bezwzględnej** pozycji p w um; v dodatnie w um/s. Wymaga bazowania. |
| `HOME Z` | Wspólne bazowanie ZL i ZR. |
| `HOME X` | Bazowanie X. |
| `SETLIM A max` | Ustawienie granic programowych 0..max w um. |
| `SETLIM A min max` | Ustawienie obu granic programowych w um. Alias: LIMIT. |
| `SETVEL A v` | Domyślna prędkość MOVE w um/s. Alias: MOVEVEL. |
| `SETPOS A p` | Przypisanie pozycji bez ruchu i oznaczenie osi jako zbazowanej; nie jest fizycznym bazowaniem. |

Ogólny profil nie obsługuje osobnego `HOME ZL`/`HOME ZR`; `HOME ZL` jest
specjalnością profilu testowego. JOG/MOVE wymagają wcześniejszego `EN 1`.
`STOP` w ogólnym profilu zeruje ruch, ale nie wyłącza ENA; użyj `EN 0`, aby
wyłączyć napęd. Heartbeat obowiązuje nadal po STOP, gdy `en=1`.

Pominięte v lub v=0 w MOVE wybiera SETVEL. ZL/ZR mają obecnie limit 250000 um/s,
X 500000 um/s; obowiązuje też limit STEP. SETLIM/SETVEL/SETPOS wymagają spoczynku.
Zmiany konfiguracji są w RAM: **nie ma komend SAVE/LOAD ani zapisu do Flash**.
Nie ma również tekstowej komendy do zmiany przyspieszenia lub prędkości homingu.
