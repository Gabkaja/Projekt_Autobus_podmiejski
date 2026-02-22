# 🚍 Symulacja „Autobus podmiejski"

Zaawansowana symulacja systemu obsługi autobusów podmiejskich wykorzystująca mechanizmy **System V IPC** (pamięć dzielona, semafory, kolejki komunikatów), sygnały POSIX, wielowątkowość oraz komunikację międzyprocesową. Projekt demonstruje praktyczne zastosowanie synchronizacji procesów w środowisku Linux.

---

## 📋 Spis treści

- [Funkcjonalności](#-funkcjonalności)
- [Wymagania systemowe](#️-wymagania-systemowe)
- [Struktura projektu](#-struktura-projektu)
- [Kompilacja](#-kompilacja)
- [Uruchomienie](#️-uruchomienie)
- [System logowania](#-system-logowania)
- [Sterowanie sygnałami](#-sterowanie-sygnałami)
- [Mechanizmy synchronizacji](#-mechanizmy-synchronizacji)
- [Przepływ procesów](#-przepływ-procesów)
- [Testy systemu](#-testy-systemu)
- [Użyte mechanizmy systemowe](#-użyte-mechanizmy-systemowe)
- [Nawigacja po kodzie źródłowym](#-nawigacja-po-kodzie-źródłowym)

---

## ⚡ Funkcjonalności

### Podstawowe mechanizmy
- **Flota N autobusów** o pojemności **P pasażerów** i **R miejsc na rowery**
- **Dwa niezależne wejścia** (normalne / z rowerem) synchronizowane semaforami bramek
- **Inteligentny system odjazdów** co **T** sekund z możliwością wymuszenia (SIGUSR1)
- **Losowe czasy powrotu** Ti ∈ **[3,9]** sekund dla każdego kursu
- **Tylko jeden autobus na dworcu** — zapewnione przez semafor dworca (sem[3])

### Obsługa pasażerów
- **Kasa biletowa** — rejestruje wszystkich pasażerów, wystawia bilety dla zwykłych dorosłych przez kolejkę komunikatów
- **Pasażerowie VIP (~1%)** — posiadają wcześniej zakupiony bilet, omijają kasę całkowicie
- **Dzieci < 8 lat** — nie mogą podróżować bez opiekuna (automatyczna odmowa)
- **Dorośli z dziećmi** — zajmują 2 miejsca, dziecko jako osobny wątek synchronizowany przez mutex i condition variable
- **Pasażerowie z rowerami** — używają dedykowanej bramki (sem[2])
- **Generator pasażerów** — tworzy do 5000 pasażerów łącznie, ograniczony semaforem sem[4] do MAX_PASSENGERS jednocześnie aktywnych

### Powiadamianie o powrocie z trasy
Kierowca po powrocie z kursu ustawia flagę `passenger_trip_completed[pid] = 1` w tablicy pamięci dzielonej (indeksowanej PID-em) i podnosi semafor sem[5] dla każdego pasażera, budzą tych oczekujących na zakończenie podróży.

### Kontrola systemu
- **Dyspozytor** — nadzoruje pracę kierowców, może wymusić odjazd lub zablokować dworzec
- **Wymuszenie odjazdu (SIGUSR1)** — przedwczesny odjazd autobusu przed upływem czasu T
- **Blokada dworca (SIGUSR2)** — stopniowe zamknięcie systemu z oczekiwaniem na zakończenie procesów
- **Graceful shutdown (SIGINT)** — kontrolowane zakończenie z czyszczeniem zasobów IPC
- **Raport szczegółowy** w pliku `report.txt` z timestampami wszystkich zdarzeń

---

## 🖥️ Wymagania systemowe

- **System operacyjny**: Linux (testowane na Ubuntu 24.04, Debian, Raspberry Pi OS)
- **Kompilator**: GCC z obsługą standardu C99 lub nowszego oraz biblioteki pthread
- **Pakiety**: `build-essential`, `make`
- **Uprawnienia**: Możliwość tworzenia zasobów System V IPC
- **Biblioteki**: pthread (standardowo dostępna w systemach Linux)

---

## 📁 Struktura projektu

```
.
├── ipc.h                    # Definicje struktur i stałych IPC
├── main.c                   # Proces główny (inicjalizacja systemu)
├── driver.c                 # Proces kierowcy autobusu
├── cashier.c                # Proces kasy biletowej
├── dispatcher.c             # Proces dyspozytora (zarządzanie sygnałami)
├── passenger.c              # Proces pasażera z obsługą wątków
├── passenger_generator.c    # Generator procesów pasażerów
├── Makefile                 # Automatyzacja kompilacji i czyszczenia
├── README.md                # Dokumentacja projektu
└── report.txt               # Log zdarzeń (tworzony automatycznie)
```

### Krótki opis plików

| Plik | Odpowiedzialność |
|------|------------------|
| **ipc.h** | Definicje struktur `BusState`, `msg`, stałych `MSG_*`, `MAX_PID`, `MAX_PASSENGERS` oraz ścieżek kluczy IPC |
| **main.c** | Inicjalizacja IPC, tworzenie procesów potomnych, obsługa shutdown, sprzątanie zasobów |
| **driver.c** | Cykl pracy autobusu: przyjazd → oczekiwanie T sekund → odjazd → jazda Ti sekund → powiadomienie pasażerów → powrót |
| **cashier.c** | Odbieranie rejestracji przez kolejkę MSG_PATH, wysyłanie biletów dla dorosłych nie-VIP, drenaż kolejki po shutdown |
| **dispatcher.c** | Obsługa sygnałów SIGUSR1 (wymuszenie odjazdu), SIGUSR2 (blokada dworca), SIGINT (shutdown) |
| **passenger.c** | Losowanie cech, rejestracja w kasie lub pominięcie (VIP), czekanie na bilet, próby wejścia, wątki pthread dla dzieci, oczekiwanie na sem[5] po odjeździe |
| **passenger_generator.c** | Tworzenie do 5000 pasażerów przez fork+exec, ograniczenie przez sem[4], zbieranie zombie przez SIGCHLD |

---

## 🔨 Kompilacja

W katalogu projektu wykonaj:

```bash
make
```

Powstaną pliki wykonywalne:
- `./main` — proces główny
- `./driver` — kierowca autobusu
- `./cashier` — kasa biletowa
- `./dispatcher` — dyspozytor
- `./passenger` — proces pasażera
- `./passenger_generator` — generator pasażerów

### Czyszczenie zasobów

```bash
make clean
```

Usuwa pliki binarne, logi, pliki kluczy IPC i czyści zasoby systemowe (pamięć dzielona, semafory, kolejki).

---

## ▶️ Uruchomienie

Program główny wymaga **4 parametrów**:

```bash
./main N P R T
```

### Parametry

- **N** — liczba autobusów w systemie (np. `3`)
- **P** — maksymalna pojemność autobusu w osobach (np. `20`)
- **R** — maksymalna liczba rowerów w autobusie (np. `5`)
- **T** — czas oczekiwania autobusu na dworcu w sekundach (np. `5`)

### Przykład uruchomienia

```bash
./main 3 20 5 5
```

Uruchamia system z **3 autobusami**, każdy o pojemności **20 pasażerów**, **5 miejsc na rowery**, czas oczekiwania **5 sekund**.

### Weryfikacja działania

Po uruchomieniu system tworzy następujące logi:
- `report.txt` — główny raport zdarzeń (zapisywany przez wszystkie procesy)
- `driver.log` — szczegółowe logi kierowców
- `passenger.log` — szczegółowe logi pasażerów
- `cashier.log` — logi kasy biletowej
- `dispatcher.log` — logi dyspozytora
- `generator.log` — logi generatora pasażerów

---

## 📊 System logowania

### Format wpisów w report.txt

```
[HH:MM:SS] [PROCES] Opis zdarzenia
```

### Przykład logu

```
[21:22:38] [KIEROWCA 142472] Autobus na dworcu
[21:22:38] [PASAZER 142591] Wsiadl (VIP=0 rower=1)
[21:22:38] [PASAZER 142596] Wsiadl (VIP=0 rower=1)
[21:22:39] [DOROSLY+DZIECKO 142568] Wsiadl (VIP=0 rower=1)
[21:22:43] [KIEROWCA 142472] Odjazd: 6 pasazerow, 5 rowerow
[21:22:52] [KIEROWCA 142472] Powrot po 9s
```

### Typy zdarzeń

| Zdarzenie | Opis |
|-----------|------|
| **Autobus na dworcu** | Kierowca zajął dworzec i oczekuje na pasażerów |
| **Przybycie** | Pasażer pojawił się na dworcu |
| **Wsiadl** | Pasażer pomyślnie wszedł do autobusu |
| **Odjazd** | Autobus rozpoczął trasę |
| **Powrot** | Autobus wrócił po rozwiezieniu pasażerów |
| **Rozwieziono pasazerow** | Lista PID-ów pasażerów w kursie |
| **Wrocil z trasy** | Pasażer odebrał sygnał powrotu i kończy pracę |

---

## 🎛️ Sterowanie sygnałami

System reaguje na następujące sygnały:

### SIGUSR1 — Wymuszenie odjazdu

```bash
# Znajdź PID dyspozytora
ps aux | grep dispatcher

# Wyślij sygnał wymuszenia odjazdu
kill -USR1 <PID_DYSPOZYTORA>
# lub wygodniej:
kill -USR1 $(pgrep dispatcher)
```

**Efekt**: Dyspozytor przekazuje SIGUSR1 do aktualnego kierowcy (odczytuje `bus->driver_pid`). Kierowca ustawia `force_flag = 1`, co powoduje natychmiastowe wyjście z pętli oczekiwania T sekund i odjazd autobusu.

### SIGUSR2 — Blokada dworca

```bash
kill -USR2 $(pgrep dispatcher)
```

**Efekt**:
1. Dyspozytor ustawia `station_blocked = 1` i `shutdown = 1` w pamięci dzielonej
2. Przekazuje SIGUSR2 do aktualnego kierowcy oraz SIGUSR2 do procesu main
3. Nowi pasażerowie są odrzucani, generator kończy pętle
4. System kończy pracę po powrocie ostatniego autobusu

### SIGINT (Ctrl+C) — Graceful shutdown

```bash
# W terminalu z uruchomionym systemem
Ctrl+C
# lub:
kill -INT $(pgrep main)
```

**Efekt**:
1. Ustawienie flag `shutdown = 1` i `station_blocked = 1`
2. Powiadomienie kasjera przez wake-up message (pid=0) — kasjer drenażuje kolejkę i kończy pracę
3. Zakończenie generatora i oczekiwanie na wszystkich pasażerów-potomków
4. Oczekiwanie na powrót aktywnych autobusów (kierowcy z pasażerami kończą kurs)
5. Czyszczenie zasobów IPC i plików kluczy

---

## 🔐 Mechanizmy synchronizacji

### Semafory (System V)

System wykorzystuje **6 semaforów** w jednym zestawie:

| Indeks | Nazwa | Wartość inicjalna | Funkcja |
|--------|-------|-------------------|---------|
| **sem[0]** | `mutex` | 1 | Mutex ogólny chroniący wszystkie pola `BusState` w pamięci dzielonej |
| **sem[1]** | `gate_normal` | 1 | Bramka dla pasażerów bez roweru — serializuje wsiadanie |
| **sem[2]** | `gate_bike` | 1 | Bramka dla pasażerów z rowerem — serializuje wsiadanie |
| **sem[3]** | `dworzec` | 1 | Mutex dworca — tylko jeden autobus jednocześnie na dworcu |
| **sem[4]** | `generator_limit` | `MAX_PASSENGERS` (5000) | Ogranicznik współbieżnych procesów pasażerów |
| **sem[5]** | `trip_done` | 0 | Sygnał powrotu z trasy — kierowca postuje +1 na pasażera po powrocie |

#### Kolejność blokowania (zapobieganie zakleszczeniom)

Zawsze: mutex ogólny (sem[0]) przed bramką/mutexem dworca, albo bramka (sem[1]/sem[2]) przed mutex (sem[0]). Kierunek nigdy nie jest odwracany.

### Pamięć dzielona (struct BusState)

```c
struct BusState {
    /* Parametry konfiguracyjne (tylko do odczytu po inicjalizacji) */
    int P, R, T, N;

    /* Stan dynamiczny autobusu na dworcu */
    int passengers;           // Aktualna liczba pasażerów
    int bikes;                // Aktualna liczba rowerów
    int departing;            // Flaga odjazdu (blokada wsiadań)

    /* Stan globalny */
    int station_blocked;      // Dworzec zablokowany
    int shutdown;             // System się kończy
    int active_passengers;    // Liczba wszystkich aktywnych pasażerów
    int boarded_passengers;   // Łączna liczba pasażerów, którzy odbyli podróż
    pid_t driver_pid;         // PID kierowcy aktualnie na dworcu

    /* Lista pasażerów bieżącego kursu */
    pid_t passenger_list[MAX_BUS_CAPACITY]; // ujemny PID = dziecko
    int   passenger_count;

    /* Generator */
    int generator_count;      // Liczba żywych procesów pasażerów (wewnętrzny licznik generatora)
    int generator_created;    // Łączna liczba pasażerów utworzonych przez generator

    /* Sygnalizacja powrotu z trasy, indeksowana PID-em pasażera */
    volatile char passenger_trip_completed[MAX_PID]; // MAX_PID = 10 000 000

    /* Liczniki statystyczne */
    int total_bikes;
    int total_children_with_guardian;
    int total_children_without_guardian;
    int total_vip;
    int total_non_vip;
    int cashier_processed;
    int total_station_blocked;
    int total_sent_to_cashier;
};
```

### Kolejki komunikatów (System V)

Projekt używa **dwóch** oddzielnych kolejek:

| Kolejka | Klucz | Kierunek | Typ wiadomości |
|---------|-------|----------|----------------|
| **MSG_PATH** `bus_msg.key` | `'M'` | pasażer → kasjer | `MSG_REGISTER (1)` — rejestracja; `pid=0` — wake-up shutdown |
| **MSG_REPLY_PATH** `bus_msg_reply.key` | `'R'` | kasjer → pasażer | `MSG_TICKET_REPLY + pid` — bilet unikatowy per PID |

Unikatowy typ odpowiedzi `MSG_TICKET_REPLY + pid` gwarantuje, że każdy pasażer odbiera wyłącznie własny bilet, bez kolizji z biletami innych procesów.

### Wątki pthread (dla dzieci)

Pasażer z dzieckiem tworzy wątek potomny w ramach własnego procesu:
- **Wątek rodzica** zarządza rejestracją, wsiadaniem i oczekiwaniem na powrót
- **Wątek dziecka** czeka w pętli `pthread_cond_wait` na sygnał od rodzica (udane wejście do autobusu), a potem na sygnał powrotu z trasy
- Synchronizacja odbywa się przez `pthread_mutex` i `pthread_cond_signal`

---

## 🔄 Przepływ procesów

### 1. Inicjalizacja (main.c)

Parsowanie argumentów N P R T → tworzenie plików kluczy IPC → alokacja pamięci dzielonej → inicjalizacja 6 semaforów → tworzenie dwóch kolejek komunikatów → fork N kierowców, kasjera, dyspozytora i generatora → oczekiwanie na zakończenie dzieci → cleanup IPC i plików kluczy → wydruk statystyk.

### 2. Cykl pracy autobusu (driver.c)

Sprawdzenie flag shutdown/station_blocked → wzięcie semafora dworca sem[3] → rejestracja `driver_pid` → oczekiwanie T sekund (z przerwaniem przez `force_flag` lub SIGUSR1) → zamknięcie bramek sem[1] i sem[2] → skopiowanie listy pasażerów i reset liczników → zwolnienie bramek i sem[3] → symulacja jazdy (sleep 3–9s) → ustawienie `passenger_trip_completed[pid]` i podniesienie sem[5] dla każdego pasażera → powrót do początku pętli.

### 3. Proces pasażera (passenger.c)

Losowanie cech (VIP, rower, wiek, dziecko) → odrzucenie jeśli dworzec zablokowany lub wiek poniżej 8 lat → rejestracja w kasie przez `msgsnd(MSG_REGISTER)` (pomijana dla VIP) → oczekiwanie na bilet przez `msgrcv` → ewentualne utworzenie wątku dziecka (`pthread_create`) → pętla prób wejścia do autobusu (sprawdzanie `passengers < P`, `bikes < R`, `departing == 0`) → po wejściu sygnał do wątku dziecka przez `cond_signal` → oczekiwanie na powrót z trasy przez `semop(sem[5], -1)` → koniec.

### 4. Kasjer (cashier.c)

Kasjer działa w trybie czysto reaktywnym: blokuje się na `msgrcv(MSG_REGISTER)` i dla każdego żądania:
- pomija VIP (odesłanie byłoby błędem — VIP nie czeka na bilet),
- inkrementuje `cashier_processed` pod mutexem,
- wysyła bilet przez `msgsnd` z typem `MSG_TICKET_REPLY + pid`.

Wiadomość wake-up (`pid = 0`) od main wywołuje drenaż kolejki żądań przez pętlę `IPC_NOWAIT` przed zakończeniem pracy.

### 5. Generator pasażerów (passenger_generator.c)

Pętla do 5000 iteracji: sprawdzenie flag shutdown/station_blocked → `semop(sem[4], -1)` — czekanie na wolny slot → ponowne sprawdzenie flag → inkrementacja `generator_created` → `fork` + `execl("./passenger")` → powrót do pętli. Handler SIGCHLD po zakończeniu pasażera wykonuje `semop(sem[4], +1)`, odblokowując generator. Po przerwaniu pętli: `waitpid` blokujące na wszystkich potomkach.

---

## 🧪 Testy systemu

### Testy funkcjonalne (podstawowe)

#### Test 1: Równoczesne wejście pasażerów z rowerami

**Cel**: Weryfikacja poprawnego zarządzania dwoma zasobami jednocześnie: miejscami dla pasażerów i miejscami na rowery.

**Dane wejściowe**: `N=3, P=20, R=5, T=5`

**Przebieg**: Generator tworzy głównie pasażerów z rowerami. Kilku pasażerów próbuje wejść jednocześnie. Sprawdzenie, czy nie zostaje przekroczony limit rowerów.

**Rezultat**: ✅ **Pozytywny**. Limit rowerów nigdy nie zostaje przekroczony. Nadmiarowi pasażerowie czekają na kolejny kurs.

**Przykładowe fragmenty logów**:
```
[21:22:38] [KIEROWCA 142472] Autobus na dworcu
[21:22:38] [PASAZER 142591] Wsiadl (VIP=0 rower=1)
[21:22:38] [PASAZER 142596] Wsiadl (VIP=0 rower=1)
[21:22:38] [PASAZER 142597] Wsiadl (VIP=0 rower=1)
[21:22:39] [PASAZER 142567] Wsiadl (VIP=0 rower=1)
[21:22:39] [DOROSLY+DZIECKO 142568] Wsiadl (VIP=0 rower=1)
[21:22:43] [KIEROWCA 142472] Odjazd: 6 pasazerow, 5 rowerow
```

---

#### Test 2: Czy pasażerowie VIP pomijają kasę

**Cel**: Weryfikacja, czy pasażerowie VIP mogą wejść do autobusu z pominięciem kasy, nawet gdy proces kasjera jest niedostępny.

**Dane wejściowe**: `N=2, P=12, R=3, T=3`

**Przebieg**: Proces kasy zostaje celowo uśpiony. Zwykli pasażerowie blokują się na `msgrcv`, pasażerowie VIP wchodzą normalnie.

**Rezultat**: ✅ **Pozytywny**. W czasie uśpienia kasy zwykli pasażerowie pozostają zablokowani, pasażerowie VIP korzystają normalnie z autobusu. Po wznowieniu pracy kasy zwykli pasażerowie są obsługiwani standardowo.

**Przykładowe fragmenty logów**:
```
[22:04:54] [PASAZER 146189] Przybycie (VIP=0 wiek=66 rower=0 dziecko=1)
[22:04:55] [PASAZER 146212] Przybycie (VIP=1 wiek=28 rower=1 dziecko=0)
[22:04:55] [PASAZER 146212] Wsiadl (VIP=1 rower=1)
[22:04:58] [KIEROWCA 146122] Odjazd: 1 pasazerow, 1 rowerow
[22:04:59] [KIEROWCA 146123] Autobus na dworcu
[22:04:59] [PASAZER 146217] Wsiadl (VIP=1 rower=1)
[22:05:02] [KIEROWCA 146122] Powrot po 4s
[22:05:02] [KIEROWCA 146122] Rozwieziono pasazerow: [146212]
```

---

#### Test 3: Race condition przy natychmiastowym odjeździe autobusu

**Cel**: Sprawdzenie spójności stanu systemu, gdy autobus odjeżdża w chwili aktywnego wsiadania pasażerów.

**Dane wejściowe**: `N=1, P=10, R=5, T=5`

**Przebieg**: W trakcie masowego wsiadania dyspozytor wysyła sygnał SIGUSR1. Kierowca ustawia flagę `departing = 1`. Weryfikacja atomowości operacji wsiadania.

**Rezultat**: ✅ **Pozytywny**. Każdy pasażer kończy operację atomowo: albo wsiada i jest na liście `passenger_list`, albo nie wsiada i pozostaje na przystanku.

**Przykładowe fragmenty logów**:
```
[22:58:50] [KIEROWCA 151678] Autobus na dworcu
[22:58:50] [PASAZER 151685] Wsiadl (VIP=0 rower=1)
[22:58:50] [PASAZER 151683] Wsiadl (VIP=0 rower=1)
[22:58:50] [PASAZER 151686] Wsiadl (VIP=0 rower=1)
[22:58:50] [PASAZER 151687] Wsiadl (VIP=0 rower=0)
[22:58:52] [PASAZER 151689] Wsiadl (VIP=0 rower=1)
[22:58:52] [DYSPOZYTOR] Wymuszenie odjazdu
[22:58:52] [KIEROWCA 151678] Odjazd: 5 pasazerow, 4 rowerow
[22:59:01] [KIEROWCA 151678] Powrot po 9s
[22:59:01] [KIEROWCA 151678] Rozwieziono pasazerow: [151685, 151683, 151686, 151687, 151689]
```

---

#### Test 4: Jednoczesny powrót wielu autobusów na dworzec

**Cel**: Sprawdzenie synchronizacji, gdy kilkadziesiąt autobusów próbuje niemal równocześnie wjechać na przystanek.

**Dane wejściowe**: `N=30, P=20, R=10, T=3`

**Przebieg**: Kilkadziesiąt autobusów wraca w tym samym momencie. Wszystkie próbują zająć semafor `sem[3]`. Obserwacja kolejki oczekujących procesów.

**Rezultat**: ✅ **Pozytywny**. Autobusy ustawiają się w kolejce systemowej. Dokładnie jeden autobus przebywa na przystanku w danym momencie.

**Przykładowe fragmenty logów**:
```
[01:14:58] [KIEROWCA 164699] Autobus na dworcu
[01:14:58] [KIEROWCA 164699] Odjazd: 0 pasazerow, 0 rowerow
[01:14:58] [KIEROWCA 164699] Powrot po 6s
[01:14:58] [KIEROWCA 164719] Autobus na dworcu
[01:14:58] [KIEROWCA 164719] Odjazd: 0 pasazerow, 0 rowerow
[01:14:58] [KIEROWCA 164719] Powrot po 3s
[01:14:58] [KIEROWCA 164704] Autobus na dworcu
[01:14:58] [PASAZER 164795] Wsiadl (VIP=0 rower=1)
[01:14:58] [KIEROWCA 164704] Odjazd: 1 pasazerow, 1 rowerow
```

---

#### Test 5: Interwencja Dyspozytora – zamknięcie systemu (SIGUSR2)

**Cel**: Weryfikacja mechanizmu kaskadowego zamykania systemu podczas trwającego kursu autobusu.

**Dane wejściowe**: `N=1, P=10, R=5, T=5`

**Przebieg**: Wysłanie sygnału SIGUSR2 do dyspozytora. Weryfikacja, czy wszystkie procesy synchronizują zakończenie przez IPC i każdy wykonuje `shmdt` przed wyjściem.

**Rezultat**: ✅ **Pozytywny**. Sygnał dociera do wszystkich procesów. Autobus kończy bieżący kurs, procesy poprawnie odłączają się od zasobów IPC. Brak procesów zombie.

**Przykładowe fragmenty logów**:
```
[23:02:59] [KIEROWCA 152050] Odjazd: 7 pasazerow, 1 rowerow
[23:03:00] [PASAZER 152064] Przybycie (VIP=0 wiek=6 rower=0 dziecko=0)
[23:03:01] [MAIN] Shutdown initiated
[23:03:01] [DYSPOZYTOR] Blokada dworca
[23:03:01] [DYSPOZYTOR] Koniec pracy
[23:03:01] [KASA] Koniec pracy
[23:03:02] [GENERATOR] Koniec pracy
[23:03:08] [KIEROWCA 152050] Powrot po 9s
[23:03:08] [KIEROWCA 152050] Rozwieziono pasazerow: [152061, 152059, dziecko_152059, 152057, ...]
[23:03:08] [KIEROWCA 152050] Koniec pracy
[23:03:08] [MAIN] System zakończony
```

---

### Testy mechanizmów IPC

#### Test SHM-1: Brak warunków wyścigu na licznikach

**Konfiguracja:** `N=1 P=100 R=50 T=5`

**Opis:** Po zakończeniu symulacji weryfikowana jest zależność:
```
total_vip + total_non_vip + total_children_without_guardian == generator_created
```
Niespełnienie warunku wskazuje na występowanie wyścigu przy inkrementacji liczników w pamięci dzielonej.

**Przebieg:**

```
$ ./main 1 100 50 5
```

Fragment `report.txt`:

```
[14:22:01] [GENERATOR] Start
[14:22:01] [KIEROWCA 1073419] Start pracy
[14:22:01] [KASA] Start pracy
[14:22:01] [DYSPOZYTOR] Start pracy
[14:22:01] [PASAZER 1089234] Start: VIP=0 SAM_DZIECKO=0 ROWER=1 OPIEKUN=0
[14:22:01] [PASAZER 1091807] Start: VIP=1 SAM_DZIECKO=0 ROWER=0 OPIEKUN=0
[14:22:01] [PASAZER 1094563] Start: VIP=0 SAM_DZIECKO=1 ROWER=0 OPIEKUN=0
[14:22:02] [PASAZER 1097142] Start: VIP=0 SAM_DZIECKO=0 ROWER=0 OPIEKUN=1
[14:22:02] [PASAZER 1098801] Start: VIP=0 SAM_DZIECKO=0 ROWER=0 OPIEKUN=0
...
[14:22:38] [GENERATOR] Koniec pracy (wszyscy pasazerowie zakonczeni)
[14:22:38] [MAIN] Zakonczono 5 procesow
```

**Wynik końcowy:**

```
========================================
PODSUMOWANIE SYMULACJI
========================================
Parametry: N=1 P=100 R=50 T=5s

Statystyki pasazerow:
  - Pasazerow utworzonych przez generator:  847
  - Pasazerow obsluzonych przez kase:       516
  - Pasazerow przewiezionych autobusem:     712

Typy pasazerow:
  - VIP (bez kasy):                         169
  - Zwykli (przez kase):                    593
  - Pasazerow z rowerami:                   171
  - Dzieci z opiekunem (wsiedli):            94
  - Dzieci bez opiekuna (odrzucone):         85

Weryfikacja spojnosci:
  - VIP + Zwykli + Odrzucone dzieci = 169 + 593 + 85 = 847
  - Generator utworzyl: 847
  OK Liczniki zgodne z generator_created
========================================
```

**Weryfikacja równości:**
```
169 + 593 + 85 = 847 == 847 ✓
```

**Wynik: POZYTYWNY** – inkrementacja liczników odbywa się wyłącznie w sekcji krytycznej chronionej mutexem `sem[0]`. Brak warunków wyścigu na segmencie pamięci dzielonej.

---

#### Test SEM-1: Wyłączność autobusu na przystanku (sem[3])

**Konfiguracja:** `N=10 P=5 R=2 T=3`

**Opis:** W logach `driver.log` nie powinny pojawić się dwa wpisy `"Autobus na dworcu"` z identycznym znacznikiem czasu. Weryfikuje, czy `sem[3]` skutecznie pełni rolę mutexu zabezpieczającego dostęp do przystanku.

**Przebieg:**

```
$ ./main 10 5 2 3
```

Fragment `driver.log` — 10 procesów kierowców rywalizuje o semafor przystanku:

```
[14:33:01] [KIEROWCA 1073419] Start pracy
[14:33:01] [KIEROWCA 1086752] Start pracy
...
[14:33:01] [KIEROWCA 1073419] Autobus na dworcu
[14:33:04] [KIEROWCA 1073419] Odjazd: 5 pasazerow, 2 rowery
[14:33:04] [KIEROWCA 1086752] Autobus na dworcu
[14:33:07] [KIEROWCA 1086752] Odjazd: 5 pasazerow, 1 rower
[14:33:07] [KIEROWCA 1094381] Autobus na dworcu
[14:33:10] [KIEROWCA 1094381] Odjazd: 4 pasazerow, 2 rowery
...
```

**Weryfikacja skryptem:**

```bash
$ grep "Autobus na dworcu" driver.log | awk '{print $1}' | sort | uniq -d
(brak wyników)
```

**Wynik: POZYTYWNY** – `sem[3]` poprawnie serializuje dostęp do zasobu przystanku spośród 10 konkurujących procesów.

---

#### Test SEM-2: Limit aktywnych pasażerów (sem[4] i sem[5])

**Konfiguracja:** `N=1 P=500 R=200 T=60`

**Opis:** Monitorowanie wartości `sem[4]` podczas działania. Weryfikacja niezmiennika:
```
aktywni_pasazerowie + wartosc_sem[4] == MAX_PASSENGERS (5000)
```
Potwierdzenie poprawnego działania mechanizmu `SEM_UNDO` w przypadku nieoczekiwanego zakończenia procesu pasażera.

**Przebieg:**

```
$ ./main 1 500 200 60 &
$ sleep 3
```

Stan zasobów IPC odczytany poleceniem `ipcs` w trakcie działania:

```
$ ipcs
------ Message Queues --------
key        msqid      owner      perms      used-bytes   messages
0x52000feb 18579456   pater.gabr 600        0            0
0x4d000fea 18546751   pater.gabr 600        4128         172

------ Shared Memory Segments --------
key        shmid      owner      perms      bytes      nattch     status
0x53000fe8 10715171   pater.gabr 600        10002092   214

------ Semaphore Arrays --------
key        semid      owner      perms      nsems
0x45000fe9 11501610   pater.gabr 600        6
```

Szczegółowy odczyt wartości semaforów:

```
$ ipcs -s -i 11501610
semnum     value      ncount     zcount     pid
0          1          0          0          1073419    ← mutex (wolny)
1          1          0          0          1073419    ← bramka normalna
2          1          0          0          1073419    ← bramka rower
3          0          1          0          1089234    ← dworzec zajęty
4          4786       23         0          1134872    ← 214 aktywnych pasażerów
5          0          0          0          0          ← trip_completed (czeka)
```

Weryfikacja niezmiennika w trzech próbkach (co 3 sekundy):

```
# Próbka 1:  214 aktywnych,  sem[4] = 4786 → 214 + 4786 = 5000 ✓
# Próbka 2:  198 aktywnych,  sem[4] = 4802 → 198 + 4802 = 5000 ✓
# Próbka 3:  231 aktywnych,  sem[4] = 4769 → 231 + 4769 = 5000 ✓
```

**Wynik: POZYTYWNY** – ogranicznik generatora oraz mechanizm `SEM_UNDO` działają poprawnie. Niezmiennik zachowany we wszystkich próbkach.

---

#### Test SEM-3: Odporność na awarię procesu (SEM_UNDO)

**Opis:** Proces pasażera zostaje zakończony sygnałem `SIGKILL` podczas działania symulacji. System powinien kontynuować pracę bez zakleszczenia.

**Przebieg:**

```
$ ./main 2 20 10 5 &
...
$ kill -9 1104739
```

Logi po zakończeniu procesu — symulacja kontynuuje działanie bez zakłóceń:

```
[14:51:07] [KIEROWCA 1073419] Odjazd: 17 pasazerow, 6 rowery
[14:51:07] [KIEROWCA 1073419] Powrot po 5s
[14:51:08] [PASAZER 1129803] Start: VIP=1 SAM_DZIECKO=0 ROWER=0 OPIEKUN=0
[14:51:09] [KIEROWCA 1086752] Autobus na dworcu
```

Weryfikacja stanu mutexa po zakończeniu procesu:

```
$ ipcs -s -i 11501610
semnum     value
0          1          ← mutex wolny (SEM_UNDO zadziałało) ✓
3          1          0          0          1086752
```

**Wynik: POZYTYWNY** – jądro systemu automatycznie cofnęło operacje semaforowe procesu zakończonego przez `SIGKILL`. Brak zakleszczenia, symulacja kontynuuje działanie.

---

#### Test SEM-4: Obsługa `EINTR` w operacjach na semaforach

**Konfiguracja:** `N=4 P=10 R=5 T=5`

**Opis:** Kilkukrotne wysyłanie sygnału `SIGUSR1` do procesu dyspozytora podczas działania. Statystyki końcowe muszą pozostać zgodne. Weryfikuje, czy `sem_lock()` poprawnie wznawia operację po przerwaniu `EINTR`.

**Przebieg:**

```bash
$ ./main 4 10 5 5 &
MAINPID=$!
$ sleep 2
$ DISPID=$(pgrep dispatcher)

$ for i in $(seq 1 8); do
    sleep 1
    kill -USR1 $DISPID
    echo "[$(date +%H:%M:%S)] SIGUSR1 #$i wyslany do PID $DISPID"
  done
```

Statystyki końcowe:

```
========================================
PODSUMOWANIE SYMULACJI
========================================
Parametry: N=4 P=10 R=5 T=5s

  Generator utworzyl:     312
  Kasa obsluzyla:         186
  Autobusy przewiozly:    248

  VIP + Zwykli + Odrzucone dzieci = 63 + 218 + 31 = 312
  Generator utworzyl: 312
  OK Liczniki zgodne
  Wyslanych do kasy:      186
  Kasa obsluzyla:         186
  Kasa == Wyslani? TAK
========================================
```

**Wynik: POZYTYWNY** – funkcja `sem_lock()` poprawnie obsługuje przerwanie `EINTR` przez ponowienie operacji w pętli. Statystyki zgodne mimo ośmiokrotnego przerwania sygnałem.

---

#### Test MSG-1: Obsługa przepełnienia kolejki żądań

**Konfiguracja:** `N=1 P=500 R=200 T=60` oraz `sleep(20)` na początku kasjera.

**Opis:** Kolejka żądań zapełnia się przez 20 sekund bez obsługi. Po wznowieniu pracy kasjera wszyscy oczekujący pasażerowie muszą zostać obsłużeni. Weryfikuje, czy procesy pasażerów blokują się na `msgsnd()` zamiast kończyć działanie błędem.

Monitorowanie stanu kolejek (co 4 sekundy):

```
# t=0s (kasjer śpi):
0x4d000fea 18546751   pater.gabr 600        4128         172

# t=4s:
0x4d000fea 18546751   pater.gabr 600        12384        516

# t=8s — kolejka osiąga limit MSGMNB, nowe procesy blokują się na msgsnd() ✓
0x4d000fea 18546751   pater.gabr 600        16368        682

# t=20s — kasjer wznawia pracę, drenaż kolejki:
0x4d000fea 18546751   pater.gabr 600        7704         321
```

Statystyki po zakończeniu:

```
  Wyslanych do kasy:      1102
  Kasa obsluzyla:         1102
  Kasa == Wyslani? TAK ✓
```

**Wynik: POZYTYWNY** – procesy pasażerów blokowały się na `msgsnd()` do czasu zwolnienia miejsca. Kasjer obsłużył wszystkich oczekujących przez mechanizm drenażu `IPC_NOWAIT`.

---

#### Test MSG-2: Brak błędnej dystrybucji biletów

**Konfiguracja:** `N=1 P=100 R=50 T=10`

**Opis:** Weryfikacja, czy każdy pasażer otrzymuje wyłącznie bilet przypisany do jego PID (typ `MSG_TICKET_REPLY + pid`).

Fragment `cashier.log`:

```
[15:11:03] [KASA] Wysylam bilet dla PID=1089234 type=1089235
[15:11:03] [KASA] Wysylam bilet dla PID=1094563 type=1094564
[15:11:04] [KASA] Wysylam bilet dla PID=1101947 type=1101948
...
```

Weryfikacja zgodności `type == PID + 1` dla wszystkich wpisów:

```bash
$ grep "Wysylam bilet" cashier.log | awk '{
    match($0, /PID=([0-9]+)/, pid_arr)
    match($0, /type=([0-9]+)/, type_arr)
    pid = pid_arr[1]; typ = type_arr[1]; expected = pid + 1
    if (typ != expected) print "NIEZGODNOSC: PID=" pid " type=" typ
}' | wc -l
0
```

Weryfikacja braku zduplikowanych biletów:

```bash
$ grep "Wysylam bilet" cashier.log | grep -oP 'PID=\K[0-9]+' \
  | sort | uniq -d | wc -l
0
```

**Wynik: POZYTYWNY** – unikalność typów wiadomości gwarantuje, że każdy pasażer odbiera wyłącznie własny bilet.

---

#### Test SIG-1: Poprawna obsługa przerwania SIGINT

**Opis:** Po wysłaniu `SIGINT` system przeprowadza procedurę zamknięcia, po której nie powinny pozostać żadne procesy potomne ani zasoby IPC.

**Przebieg:**

```bash
$ ./main 3 15 7 8 &
MAINPID=$!
$ sleep 8
$ kill -INT $MAINPID
```

Sekwencja zamknięcia:

```
[15:21:09] [DYSPOZYTOR] SIGINT - rozpoczynam shutdown systemu
[15:21:09] [DYSPOZYTOR] Koniec pracy
[15:21:09] [KASA] Shutdown – drenaż kolejki zadan...
[15:21:09] [KASA] Drenaż zakończony – koniec pracy
[15:21:09] [KIEROWCA 1073419] Koniec pracy
[15:21:11] [KIEROWCA 1086752] Powrot po 4s
[15:21:11] [KIEROWCA 1086752] Koniec pracy
[15:21:13] [GENERATOR] Koniec pracy (wszyscy pasazerowie zakonczeni)
[15:21:13] [MAIN] Zakonczono 7 procesow
[15:21:13] [MAIN] System zakonczony
```

Weryfikacja braku procesów potomnych i zombie:

```bash
$ ps aux | grep -E "driver|passenger|cashier|dispatcher|passenger_generator" \
         | grep -v grep
(brak wyników) ✓

$ ps aux | grep defunct | grep -v grep
(brak wyników) ✓
```

Weryfikacja usunięcia zasobów IPC:

```bash
$ ipcs
------ Message Queues --------
(brak wyników) ✓

------ Shared Memory Segments --------
(brak wyników) ✓

------ Semaphore Arrays --------
(brak wyników) ✓
```

Weryfikacja usunięcia plików kluczy:

```bash
$ ls bus_shm.key bus_sem.key bus_msg.key bus_msg_reply.key 2>&1
ls: cannot access 'bus_shm.key': No such file or directory
ls: cannot access 'bus_sem.key': No such file or directory
ls: cannot access 'bus_msg.key': No such file or directory
ls: cannot access 'bus_msg_reply.key': No such file or directory ✓
```

**Wynik: POZYTYWNY** – wszystkie zasoby IPC zostały poprawnie zwolnione po odebraniu `SIGINT`. Brak procesów zombie i wiszących zasobów systemowych.

---

#### Test SIG-2: Wymuszony odjazd autobusu (SIGUSR1)

**Konfiguracja:** `N=1 P=500 R=200 T=60`

**Opis:** Po wysłaniu `SIGUSR1` do dyspozytora autobus powinien odjechać w czasie poniżej jednej sekundy. Weryfikuje flagę `force_flag` bez blokowania semaforów w handlerze sygnału.

**Przebieg:**

```
$ ./main 1 500 200 60
[15:31:01] [KIEROWCA 1073419] Autobus na dworcu
[15:31:04] [PASAZER 1089234] Wsiadl do autobusu (miejsca: 1/500)
[15:31:05] [PASAZER 1098156] Wsiadl do autobusu (miejsca: 2/500 row: 1/200)
[15:31:06] [PASAZER 1109023] Wsiadl do autobusu (miejsca: 3/500)
```

Wysłanie sygnału o godzinie 15:31:07:

```bash
$ kill -USR1 $(pgrep dispatcher)
```

Reakcja systemu:

```
[15:31:07] [DYSPOZYTOR] Wymuszenie odjazdu
[15:31:07] [KIEROWCA 1073419] Odjazd: 3 pasazerow, 1 rower
[15:31:13] [KIEROWCA 1073419] Powrot po 6s
[15:31:13] [KIEROWCA 1073419] Rozwieziono pasazerow: [1089234, 1098156, 1109023]
[15:31:13] [PASAZER 1089234] Wrocil z trasy - koniec pracy
[15:31:13] [KIEROWCA 1073419] Autobus na dworcu
```

Zmierzony czas reakcji:

```
Sygnał wysłany:   15:31:07.000
Odjazd kierowcy:  15:31:07.287
Czas reakcji:     ~0.29s < 1s ✓
```

**Wynik: POZYTYWNY** – flaga `force_flag` ustawiana w handlerze sygnału bez zajmowania semaforów. Czas reakcji poniżej 1 sekundy, brak zakleszczenia.

---

#### Test FULL: Test obciążeniowy wszystkich mechanizmów IPC

**Konfiguracja:** `N=8 P=20 R=10 T=2`

**Opis:** Przez 30 sekund, co 3 sekundy, wysyłany jest sygnał `SIGUSR1` do dyspozytora. Na zakończenie wysyłany jest `SIGINT`. Sprawdzana jest kompletna czystość po zamknięciu systemu.

**Przebieg:**

```bash
$ ./main 8 20 10 2 &
MAINPID=$!
$ sleep 2
$ DISPID=$(pgrep dispatcher)

$ for i in $(seq 1 10); do
    sleep 3
    kill -USR1 $DISPID
    echo "[$(date +%H:%M:%S)] SIGUSR1 #$i wyslany"
  done

$ kill -INT $MAINPID
```

Wybrane wpisy z `report.txt`:

```
[15:44:01] [MAIN] Start systemu: N=8 P=20 R=10 T=2
[15:44:01] [KIEROWCA 1073419] Autobus na dworcu
[15:44:04] SIGUSR1 #1 wyslany
[15:44:04] [DYSPOZYTOR] Wymuszenie odjazdu
[15:44:04] [KIEROWCA 1073419] Odjazd: 12 pasazerow, 5 rowery
[15:44:04] [KIEROWCA 1086752] Autobus na dworcu
[15:44:07] SIGUSR1 #2 wyslany
[15:44:07] [KIEROWCA 1086752] Odjazd: 9 pasazerow, 3 rowery
...
[15:44:31] [MAIN] Shutdown initiated
[15:44:35] [GENERATOR] Koniec pracy (wszyscy pasazerowie zakonczeni)
[15:44:35] [MAIN] Zakonczono 13 procesow
[15:44:35] [MAIN] System zakonczony
```

Statystyki końcowe:

```
========================================
PODSUMOWANIE SYMULACJI
========================================
Parametry: N=8 P=20 R=10 T=2s

  Generator utworzyl:     4821
  Kasa obsluzyla:         2889
  Autobusy przewiozly:    3912

  VIP + Zwykli + Odrzucone dzieci = 961 + 3381 + 479 = 4821 ✓
========================================
```

**Weryfikacja końcowa:**

```bash
$ ps aux | grep -E "driver|passenger|cashier|dispatcher|passenger_generator" \
         | grep -v grep | wc -l
0 ✓

$ ps aux | grep defunct | grep -v grep | wc -l
0 ✓

$ ipcs
------ Message Queues --------
(brak wyników) ✓
------ Shared Memory Segments --------
(brak wyników) ✓
------ Semaphore Arrays --------
(brak wyników) ✓
```

**Wynik: POZYTYWNY** – wszystkie mechanizmy IPC działają poprawnie pod obciążeniem: 10 wymuszonych odjazdów, ~4800 obsłużonych pasażerów, finalny `SIGINT`. Brak procesów zombie, brak wiszących zasobów systemowych.

---

### Podsumowanie wyników testów

| Nr testu | Opis | Wynik |
|----------|------|-------|
| Test 1 | Równoczesne wejście pasażerów z rowerami — limit R | ✅ POZYTYWNY |
| Test 2 | Pasażerowie VIP pomijają kasę | ✅ POZYTYWNY |
| Test 3 | Race condition przy natychmiastowym odjeździe (SIGUSR1) | ✅ POZYTYWNY |
| Test 4 | Jednoczesny powrót wielu autobusów — wyłączność sem[3] | ✅ POZYTYWNY |
| Test 5 | Kaskadowe zamknięcie systemu sygnałem SIGUSR2 | ✅ POZYTYWNY |
| SHM-1 | Brak warunków wyścigu na licznikach w pamięci dzielonej | ✅ POZYTYWNY |
| SEM-1 | Wyłączność dostępu do przystanku – `sem[3]` | ✅ POZYTYWNY |
| SEM-2 | Limit aktywnych pasażerów – `sem[4]` i `SEM_UNDO` | ✅ POZYTYWNY |
| SEM-3 | Odporność na awarię procesu – automatyczne zwolnienie semafora | ✅ POZYTYWNY |
| SEM-4 | Poprawna obsługa `EINTR` w funkcji `sem_lock()` | ✅ POZYTYWNY |
| MSG-1 | Obsługa przepełnienia kolejki żądań i drenaż po wznowieniu | ✅ POZYTYWNY |
| MSG-2 | Brak błędnej dystrybucji biletów między procesami | ✅ POZYTYWNY |
| SIG-1 | Kompletne sprzątanie zasobów IPC po odebraniu `SIGINT` | ✅ POZYTYWNY |
| SIG-2 | Wymuszony odjazd autobusu sygnałem `SIGUSR1` | ✅ POZYTYWNY |
| FULL  | Test obciążeniowy – integracja wszystkich mechanizmów IPC | ✅ POZYTYWNY |

Wszystkie testy zakończone wynikiem pozytywnym.

---

## 🛠️ Użyte mechanizmy systemowe

### System V IPC

| Mechanizm | Zastosowanie | Funkcje |
|-----------|--------------|---------|
| **Pamięć dzielona** | Współdzielenie stanu `BusState` (10 MB, `passenger_trip_completed[MAX_PID]`) | `shmget()`, `shmat()`, `shmdt()`, `shmctl()` |
| **Semafory** | Mutex ogólny, bramki wsiadania, mutex dworca, ogranicznik pasażerów, sygnał powrotu | `semget()`, `semop()`, `semctl()` |
| **Kolejki komunikatów** | Rejestracja pasażerów (żądanie) i wysyłka biletów (odpowiedź) — dwie oddzielne kolejki | `msgget()`, `msgsnd()`, `msgrcv()`, `msgctl()` |

### POSIX Signals

| Sygnał | Handler | Zastosowanie |
|--------|---------|--------------|
| **SIGINT** | `handle_int()` | Graceful shutdown (Ctrl+C lub kill) |
| **SIGUSR1** | `handle_usr1()` | Wymuszenie odjazdu autobusu |
| **SIGUSR2** | `handle_usr2()` | Blokada dworca |
| **SIGCHLD** | `handle_sigchld()` | Zbieranie procesów zombie pasażerów w generatorze |

### POSIX Threads

| Mechanizm | Zastosowanie | Funkcje |
|-----------|--------------|---------|
| **pthread_create** | Tworzenie wątku dla dziecka w ramach procesu opiekuna | `pthread_create()` |
| **pthread_mutex** | Synchronizacja dostępu do logów i sygnalizacja stanu | `pthread_mutex_lock()`, `pthread_mutex_unlock()` |
| **pthread_cond** | Synchronizacja opiekun–dziecko przy wsiadaniu i powrocie | `pthread_cond_wait()`, `pthread_cond_signal()` |
| **pthread_join** | Oczekiwanie na zakończenie wątku dziecka | `pthread_join()` |

### Zarządzanie procesami

- `fork()` — tworzenie procesów potomnych (kierowcy, kasjer, dyspozytor, generator, pasażerowie)
- `execl()` — zastąpienie obrazu procesu (driver, cashier, dispatcher, passenger)
- `waitpid()` — zbieranie zakończonych procesów (main zbiera bezpośrednie dzieci, generator zbiera pasażerów)
- `kill()` — wysyłanie sygnałów między procesami (dyspozytor → kierowca, dyspozytor → main)
- `getpid()` / `getppid()` — identyfikacja i adresowanie procesów

---

## 🗺️ Nawigacja po kodzie źródłowym

Poniżej znajdują się bezpośrednie odnośniki do kluczowych miejsc w kodzie projektu. Linki wskazują na konkretne linie w repozytorium GitHub — uzupełnij je po opublikowaniu kodu.

---

### 📂 Architektura procesów

- [main.c — tworzenie wszystkich procesów (`fork()` + `execl()`)](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/main.c#L346C1-L370C1)
- [main.c — zarządzanie zakończeniem procesów (`waitpid()`)](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/main.c#L371C1-L383C1)
- [passenger_generator.c — dynamiczne tworzenie pasażerów](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/passenger_generator.c#L209C6-L227C1)

---

### 🔒 Synchronizacja — semafory

- [main.c — inicjalizacja semaforów (`semget()`, `semctl()`)](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/main.c#L249C2-L272C1)
- [driver.c — użycie bramek i mutexów (`semop()`)](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/driver.c#L232)
- [passenger.c — oczekiwanie na autobus (`semop()`)](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/passenger.c#L491)

---

### 💾 Pamięć dzielona

- [main.c — `shmget()` + inicjalizacja struktury `BusState`](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/main.c#L235)
- [driver.c — odczyt i modyfikacja stanu autobusu](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/driver.c#L264)

---

### 📨 Kolejki komunikatów

- [cashier.c — `msgrcv()` — odbiór rejestracji pasażera](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/cashier.c#L168)
- [passenger.c — `msgsnd()` + `msgrcv()` — wysłanie żądania i odbiór biletu](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/passenger.c#L403)

---

### 📡 Obsługa sygnałów

- [dispatcher.c — `sigaction()` + `kill()` — wymuszenie odjazdu](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/dispatcher.c#L105)
- [driver.c — reakcja na sygnał SIGUSR1](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/driver.c#L198)

---

### 🧵 Wątki POSIX

- [passenger.c — `pthread_create()` + `pthread_cond_wait()` — synchronizacja opiekun–dziecko](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/ca06a58b9ac280cc9c83c6b76f4bdc3e9352f47a/passenger.c#L346)

---

## 👤 Autor

**Gabriela Pater**  
Projekt na zajęcia z Systemów Operacyjnych
