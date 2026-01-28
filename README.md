# 🚍 Symulacja „Autobus podmiejski"

Zaawansowana symulacja systemu obsługi autobusów podmiejskich wykorzystująca mechanizmy **System V IPC** (pamięć dzielona, semafory, kolejki komunikatów), sygnały POSIX oraz komunikację międzyprocesową. Projekt demonstruje praktyczne zastosowanie synchronizacji procesów w środowisku Linux.

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
- [Przykładowe testy](#-przykładowe-testy)
- [Użyte mechanizmy systemowe](#-użyte-mechanizmy-systemowe)

---

## ⚡ Funkcjonalności

### Podstawowe mechanizmy
- **Flota N autobusów** o pojemności **P pasażerów** i **R miejsc na rowery**
- **Dwa niezależne wejścia** (normalne / z rowerem) synchronizowane semaforami bramek
- **Inteligentny system odjazdów** co **T** sekund z możliwością wymuszenia (SIGUSR1)
- **Losowe czasy powrotu** Ti ∈ **[3,9]** sekund dla każdego kursu
- **Tylko jeden autobus na dworcu** — zapewnione przez semafor dworca

### Obsługa pasażerów
- **Kasa biletowa** — rejestruje wszystkich pasażerów, wystawia bilety dla zwykłych dorosłych
- **Pasażerowie VIP (~1%)** — posiadają wcześniej zakupiony bilet, tylko rejestracja
- **Dzieci < 8 lat** — nie mogą podróżować bez opiekuna (automatyczna odmowa)
- **Dorośli z dziećmi** — zajmują 2 miejsca, dziecko jako osobny proces synchronizowany przez pipe
- **Pasażerowie z rowerami** — używają dedykowanej bramki (semafora 2)
- **Generator pasażerów** — tworzy nowych pasażerów co 1-3 sekundy w nieskończoność

### Kontrola systemu
- **Dyspozytor** — nadzoruje pracę kierowców, może wymusić odjazd lub zablokować dworzec
- **Wymuszenie odjazdu (SIGUSR1)** — przedwczesny odjazd autobusu przed upływem czasu T
- **Blokada dworca (SIGUSR2)** — stopniowe zamknięcie systemu z oczekiwaniem na zakończenie procesów
- **Graceful shutdown (SIGINT)** — kontrolowane zakończenie z czyszczeniem zasobów IPC
- **Raport szczegółowy** w pliku `report.txt` z timestampami wszystkich zdarzeń

---

## 🖥️ Wymagania systemowe

- **System operacyjny**: Linux (testowane na Ubuntu 24.04, Debian, Raspberry Pi OS)
- **Kompilator**: GCC z obsługą standardu C99 lub nowszego
- **Pakiety**: `build-essential`, `make`
- **Uprawnienia**: Możliwość tworzenia zasobów System V IPC
- **Brak zewnętrznych bibliotek** — wykorzystuje wyłącznie standardowe nagłówki POSIX i System V

---

## 📁 Struktura projektu

```
.
├── ipc.h                    # Definicje struktur i stałych IPC
├── main.c                   # Proces główny (inicjalizacja systemu)
├── driver.c                 # Proces kierowcy autobusu
├── cashier.c                # Proces kasy biletowej
├── dispatcher.c             # Proces dyspozytora (zarządzanie sygnałami)
├── passenger.c              # Proces pasażera (logika wejścia)
├── passenger_generator.c    # Generator procesów pasażerów
├── Makefile                 # Automatyzacja kompilacji i czyszczenia
├── README.md                # Dokumentacja projektu
└── report.txt               # Log zdarzeń (tworzony automatycznie)
```

### Krótki opis plików

| Plik | Odpowiedzialność |
|------|------------------|
| **ipc.h** | Definicje struktur `BusState`, `msg`, stałych `MSG_*` oraz ścieżek kluczy IPC |
| **main.c** | Inicjalizacja IPC, tworzenie procesów potomnych, obsługa shutdown, sprzątanie zasobów |
| **driver.c** | Cykl pracy autobusu: przyjazd → oczekiwanie T sekund → odjazd → jazda Ti sekund → powrót |
| **cashier.c** | Odbieranie rejestracji pasażerów, wysyłanie biletów dla dorosłych nie-VIP |
| **dispatcher.c** | Obsługa sygnałów SIGUSR1 (wymuszenie), SIGUSR2 (blokada), przekazywanie do kierowcy |
| **passenger.c** | Losowanie cech, rejestracja w kasie, czekanie na bilet, próby wejścia, fork() dla dzieci |
| **passenger_generator.c** | Nieskończone tworzenie pasażerów co 1-3 sekundy aż do shutdown |

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

Program główny wymaga **4 parametry**:

```bash
./main N P R T
```

### Parametry

| Parametr | Opis | Zakres wartości |
|----------|------|----------------|
| **N** | Liczba autobusów w flocie | 1-100 |
| **P** | Maksymalna pojemność pasażerów | 1-1000 |
| **R** | Liczba miejsc na rowery | 0-100 |
| **T** | Czas oczekiwania na dworcu (sekundy) | 1-3600 |

### Przykłady uruchomienia

#### Mała symulacja (test funkcjonalności)
```bash
./main 2 10 5 3
```
**Opis:** 2 autobusy, max 10 pasażerów, max 5 rowerów, 3s oczekiwanie na dworcu

#### Średnia symulacja (typowe użycie)
```bash
./main 3 20 10 5
```
**Opis:** 3 autobusy, max 20 pasażerów, max 10 rowerów, 5s oczekiwanie na dworcu

#### Duża symulacja (test stabilności)
```bash
./main 5 50 20 4
```
**Opis:** 5 autobusów, max 50 pasażerów, max 20 rowerów, 4s oczekiwanie na dworcu

> **Uwaga:** Program działa w nieskończoność (generator tworzy pasażerów ciągle) aż do otrzymania sygnału shutdown (SIGINT lub SIGUSR2).

---

## 📝 System logowania

Wszystkie zdarzenia zapisują się do pliku:

```
report.txt
```

### Format logów

```
[HH:MM:SS] [MODUŁ] Opis zdarzenia
```

### Rodzaje zdarzeń w logu

| Moduł | Przykładowe zdarzenia |
|-------|----------------------|
| **MAIN** | Start systemu, shutdown initiated, system zakończony |
| **GENERATOR** | Start, koniec pracy |
| **KASA** | Rejestracja (PID, VIP, DZIECKO), koniec pracy |
| **PASAZER** | Przybycie (VIP, wiek, rower, dziecko), wsiadł, dworzec zamknięty |
| **DZIECKO** | Bez opiekuna - odmowa |
| **DOROSLY+DZIECKO** | Wsiadł (VIP, rower) |
| **KIEROWCA** | Start pracy, autobus na dworcu, odjazd (liczba pasażerów/rowerów), powrót, koniec pracy |
| **DYSPOZYTOR** | Start pracy, wymuszenie odjazdu, blokada dworca, koniec pracy |

### Podgląd logów na żywo

```bash
tail -f report.txt
```

### Przykładowy fragment logu

```
[14:32:15] [MAIN] Start systemu: N=3 P=20 R=10 T=5
[14:32:15] [GENERATOR] Start - tworzy pasazerow w nieskonczonosc
[14:32:15] [KASA] Start pracy
[14:32:15] [DYSPOZYTOR] Start pracy
[14:32:15] [KIEROWCA 12345] Start pracy
[14:32:15] [KIEROWCA 12346] Start pracy
[14:32:15] [KIEROWCA 12347] Start pracy
[14:32:15] [KIEROWCA 12345] Autobus na dworcu
[14:32:16] [PASAZER 12350] Przybycie (VIP=0 wiek=25 rower=1 dziecko=0)
[14:32:16] [KASA] Rejestracja PID=12350 VIP=0 DZIECKO=0
[14:32:16] [PASAZER 12350] Wsiadl (VIP=0 rower=1)
[14:32:18] [PASAZER 12355] Przybycie (VIP=0 wiek=32 rower=0 dziecko=1)
[14:32:18] [KASA] Rejestracja PID=12355 VIP=0 DZIECKO=0
[14:32:18] [DOROSLY+DZIECKO 12355] Wsiadl (VIP=0 rower=0)
[14:32:20] [KIEROWCA 12345] Odjazd: 3 pasazerow, 1 rowerow
[14:32:27] [KIEROWCA 12345] Powrot po 7s
[14:32:27] [KIEROWCA 12346] Autobus na dworcu
```

> **Uwaga:** Plik `report.txt` jest dopisywany (append). Czyszczony przy każdym `make clean`.

---

## 🧭 Sterowanie sygnałami

System reaguje na następujące sygnały:

| Sygnał | Źródło | Odbiorca | Działanie |
|--------|--------|----------|-----------|
| **SIGUSR1** | Dyspozytor | Kierowca | Wymuszenie natychmiastowego odjazdu autobusu |
| **SIGUSR2** | Dyspozytor | Kierowca | Blokada dworca i rozpoczęcie shutdown |
| **SIGINT** (Ctrl+C) | Użytkownik | Main → Dyspozytor | Graceful shutdown całego systemu |
| **SIGCHLD** | Kernel | Main, Generator | Zbieranie zombie processes |

### Szczegółowe działanie sygnałów

#### SIGUSR1 — Wymuszenie odjazdu

```bash
killall -SIGUSR1 dispatcher
```

**Działanie:**
1. Dyspozytor otrzymuje sygnał
2. Przekazuje SIGUSR1 do kierowcy aktualnie na dworcu (bus->driver_pid)
3. Kierowca ustawia flagę `force_flag=1`
4. Autobus przerywa czekanie i odjeżdża natychmiast
5. Logowane jako `[DYSPOZYTOR] Wymuszenie odjazdu`

**Zastosowanie:** Przyspieszenie odjazdu autobusu w sytuacji krytycznej

---

#### SIGUSR2 — Blokada dworca

```bash
killall -SIGUSR2 dispatcher
```

**Działanie:**
1. Dyspozytor otrzymuje sygnał
2. Ustawia `bus->station_blocked=1` oraz `bus->shutdown=1`
3. Przekazuje SIGUSR2 do kierowcy
4. Nowi pasażerowie nie mogą już wejść
5. Generator przestaje tworzyć pasażerów
6. Kierowcy kończą pracę po powrocie z kursu
7. Kasa kończy pracę po sprawdzeniu flagi shutdown
8. Logowane jako `[DYSPOZYTOR] Blokada dworca`

**Zastosowanie:** Kontrolowane zamknięcie dworca z zachowaniem bezpieczeństwa pasażerów

---

#### SIGINT — Shutdown systemu

```bash
# W terminalu z uruchomionym programem:
Ctrl+C
```

**Działanie:**
1. Main otrzymuje SIGINT
2. Ustawia `bus->shutdown=1`, `bus->station_blocked=1`
3. Przekazuje SIGINT do dyspozytora
4. Wszystkie procesy sprawdzają flag shutdown przed każdą operacją
5. Kierowcy kończą bieżącą trasę i przestają akceptować nowych pasażerów
6. Generator przestaje tworzyć pasażerów
7. Kasa kończy działanie
8. Main czeka na zakończenie wszystkich procesów potomnych (`wait()`)
9. Czyszczenie zasobów IPC:
   - `shmctl(IPC_RMID)` — usunięcie pamięci dzielonej
   - `semctl(IPC_RMID)` — usunięcie semaforów
   - `msgctl(IPC_RMID)` — usunięcie kolejki komunikatów
10. Usunięcie plików kluczy (`unlink()`)

**Zastosowanie:** Bezpieczne zakończenie całego systemu

---

## 🔐 Mechanizmy synchronizacji

### Semafory (4 semafory w zestawie)

| Indeks | Początkowa wartość | Przeznaczenie |
|--------|-------------------|---------------|
| **0** | 1 | **Mutex** — ochrona pamięci dzielonej (struktura BusState) |
| **1** | 1 | **Bramka bez roweru** — synchronizacja wejścia pasażerów bez roweru |
| **2** | 1 | **Bramka z rowerem** — synchronizacja wejścia pasażerów z rowerem |
| **3** | 1 | **Dworzec** — dostęp do dworca (tylko 1 autobus jednocześnie) |

**Operacje semaforowe:**
- `sem_lock()` — P(sem) — zmniejszenie o 1 (oczekiwanie jeśli 0)
- `sem_unlock()` — V(sem) — zwiększenie o 1 (odblokowanie)
- `gate_lock(gate)` — blokada konkretnej bramki
- `gate_unlock(gate)` — odblokowanie bramki

---

### Pamięć dzielona (struktura `BusState`)

```c
struct BusState {
    int P;                      // Maksymalna liczba pasażerów
    int R;                      // Maksymalna liczba rowerów
    int T;                      // Czas oczekiwania na dworcu
    int N;                      // Liczba autobusów
    int passengers;             // Aktualna liczba pasażerów w autobusie
    int bikes;                  // Aktualna liczba rowerów w autobusie
    int departing;              // Flaga: autobus odjeżdża (0/1)
    int station_blocked;        // Flaga: dworzec zablokowany (0/1)
    int active_passengers;      // Liczba aktywnych pasażerów w systemie
    int boarded_passengers;     // Liczba pasażerów, którzy weszli do autobusu
    pid_t driver_pid;           // PID aktualnego kierowcy na dworcu
    int shutdown;               // Flaga: system się wyłącza (0/1)
};
```

**Dostęp do pamięci dzielonej:**
- Zawsze chroniony przez mutex (semafor 0)
- Operacje atomowe: `sem_lock()` → modyfikacja → `sem_unlock()`
- Przykład: `sem_lock(); bus->passengers++; sem_unlock();`

---

### Kolejka komunikatów

**Typy wiadomości:**
- `MSG_REGISTER (1)` — Rejestracja pasażera w kasie (wysyła pasażer)
- `MSG_TICKET_REPLY + PID` — Unikalny typ odpowiedzi dla każdego pasażera (wysyła kasa)

**Struktura wiadomości:**
```c
struct msg {
    long type;          // Typ wiadomości
    pid_t pid;          // PID pasażera
    int vip;            // Czy VIP (0/1)
    int bike;           // Czy ma rower (0/1)
    int child;          // Czy dziecko (0/1)
    int ticket_ok;      // Czy bilet zatwierdzony (0/1)
};
```

**Schemat komunikacji:**
1. Pasażer → `msgsnd(MSG_REGISTER)` → Kasa
2. Kasa → Sprawdzenie danych → `msgsnd(MSG_TICKET_REPLY + PID)` → Pasażer
3. Pasażer → `msgrcv(MSG_TICKET_REPLY + PID)` → Otrzymanie biletu

---

### Pipe (komunikacja rodzic-dziecko)

Wykorzystywany **tylko** dla synchronizacji dorosłego z dzieckiem:

```c
int pipefd[2];
pipe(pipefd);

pid_t cpid = fork();
if (cpid == 0) {
    // Proces dziecka
    close(pipefd[1]);
    char buf;
    read(pipefd[0], &buf, 1);  // Czeka na sygnał od rodzica
    close(pipefd[0]);
    // Wchodzi przez bramkę
} else {
    // Proces rodzica
    close(pipefd[0]);
    // ... próbuje wsiąść ...
    write(pipefd[1], "X", 1);  // Sygnał dla dziecka
    close(pipefd[1]);
}
```

**Cel:** Zapewnienie, że dziecko wchodzi **równocześnie** z rodzicem (nie przed, nie po)

---

## 🔄 Przepływ procesów

### 1. Inicjalizacja (main.c)

```
main
 ├── Tworzenie plików kluczy (SHM_PATH, SEM_PATH, MSG_PATH)
 ├── Inicjalizacja IPC (shmget, semget, msgget)
 ├── Ustawienie semaforów (0:1, 1:1, 2:1, 3:1)
 ├── fork() → driver (x N razy)
 ├── fork() → cashier
 ├── fork() → dispatcher
 ├── fork() → passenger_generator
 └── wait() — czekanie na zakończenie wszystkich procesów
```

---

### 2. Generator pasażerów (passenger_generator.c)

```
Pętla nieskończona:
    ↓
Losowy sleep(1-3 sekundy)
    ↓
Sprawdzenie shutdown/station_blocked → KONIEC jeśli TAK
    ↓
sem_lock()
bus->active_passengers++
sem_unlock()
    ↓
fork() → passenger
    ↓
Powrót do początku pętli
```

**Zakończenie:** Po otrzymaniu flagi `shutdown=1` lub `station_blocked=1`

---

### 3. Pasażer (passenger.c)

```
Losowanie cech:
    - VIP (1%)
    - rower (50%)
    - wiek (0-79)
    - z_dzieckiem (20% dla dorosłych)
    ↓
Sprawdzenie: dworzec otwarty? → NIE → KONIEC
    ↓
Dziecko < 8 lat? → TAK → "Bez opiekuna - odmowa" → KONIEC
    ↓
Rejestracja w kasie:
    msgsnd(MSG_REGISTER, {pid, vip, bike, child=0})
    ↓
VIP? → TAK → Pomijamy czekanie na bilet
    ↓ NIE
Czekanie na bilet:
    msgrcv(MSG_TICKET_REPLY + PID)
    ↓
Dorosły z dzieckiem? → TAK → fork() + pipe + synchronizacja
    ↓ NIE
Pętla próby wejścia:
    ├── try_board(bike, with_child, vip)
    ├──── gate_lock(1 lub 2)
    ├──── sem_lock()
    ├──── Sprawdzenie: shutdown? departing? miejsca?
    ├──── Wejście: bus->passengers += needed
    ├──── sem_unlock()
    ├──── gate_unlock()
    ├── Sukces? → TAK → Logowanie wsiadł → KONIEC
    ├── Brak miejsca? → sleep(1) → Powtórz
    └── Shutdown? → KONIEC
    ↓
sem_lock()
bus->active_passengers--
sem_unlock()
```

---

### 4. Kierowca (driver.c)

```
Pętla nieskończona:
    ↓
gate_lock(3) — Blokada dworca (tylko 1 autobus)
    ↓
sem_lock()
bus->driver_pid = getpid()
bus->departing = 0
wait_time = bus->T
sem_unlock()
    ↓
Sprawdzenie shutdown/station_blocked → KONIEC jeśli TAK
    ↓
Logowanie: "Autobus na dworcu"
    ↓
Czekanie T sekund (lub force_flag):
    while (!force_flag && waited < wait_time)
        sleep(1)
        waited++
        Sprawdzenie shutdown → KONIEC jeśli TAK
    ↓
force_flag = 0
    ↓
Blokada obu bramek:
    gate_lock(1)
    gate_lock(2)
    ↓
sem_lock()
bus->departing = 1
p = bus->passengers
r = bus->bikes
bus->boarded_passengers += p
sem_unlock()
    ↓
Logowanie: "Odjazd: p pasażerów, r rowerów"
    ↓
Reset liczników:
    sem_lock()
    bus->passengers = 0
    bus->bikes = 0
    sem_unlock()
    ↓
Odblokowanie:
    gate_unlock(1)
    gate_unlock(2)
    gate_unlock(3)
    ↓
Jazda: sleep(Ti) gdzie Ti ∈ [3,9]s
    ↓
Logowanie: "Powrót po Ti s"
    ↓
Sprawdzenie shutdown → KONIEC jeśli TAK
    ↓
Powrót do początku pętli
```

---

### 5. Kasa (cashier.c)

```
Pętla nieskończona:
    ↓
sem_lock()
sd = bus->shutdown
sem_unlock()
    ↓
Jeśli sd → KONIEC
    ↓
msgrcv(MSG_REGISTER, IPC_NOWAIT)
    ↓
Brak wiadomości? → sleep krótko → Powrót do początku
    ↓
Znaleziono wiadomość → Logowanie rejestracji
    ↓
VIP lub dziecko? → TAK → Pomijamy wysyłanie biletu
    ↓ NIE
Wysłanie biletu:
    m.ticket_ok = 1
    m.type = MSG_TICKET_REPLY + m.pid
    msgsnd(msgid, m)
    ↓
Powrót do początku pętli
```

**Zakończenie:** Po otrzymaniu flagi `shutdown=1`

---

### 6. Dyspozytor (dispatcher.c)

```
Rejestracja handlerów sygnałów:
    - SIGINT → handle_int → shutdown=1, station_blocked=1
    - SIGUSR1 → handle_usr1 → kill(driver_pid, SIGUSR1)
    - SIGUSR2 → handle_usr2 → shutdown=1, station_blocked=1, kill(driver_pid, SIGUSR2)
    ↓
Pętla nieskończona:
    pause() — czekanie na sygnał
    ↓
should_exit? → TAK → KONIEC
    ↓
Powrót do początku pętli
```

**Zakończenie:** Po otrzymaniu SIGINT lub SIGUSR2 (ustawia `should_exit=1`)

---

## ✅ Przykładowe testy

### Test 1: Limit pojemności pasażerów i rowerów

**Cel:** Sprawdzenie odmowy wejścia po osiągnięciu limitu

```bash
./main 2 10 3 8
# W drugim terminalu po kilkunastu sekundach:
killall -SIGINT main
```

**Oczekiwany wynik:** Pierwsze 10 osób wchodzi, reszta czeka na następny autobus

**Fragment logu:**
```
[14:42:05] [KIEROWCA 180650] Odjazd: 10 pasazerow, 3 rowerow
[14:42:07] [PASAZER 180677] Wsiadl (VIP=0 rower=1)
[14:42:09] [PASAZER 180678] Wsiadl (VIP=0 rower=0)
```

**Analiza:** Autobus osiągnął limit 10 pasażerów i 3 rowerów, kolejni czekają na następny kurs

---

### Test 2: Wymuszenie odjazdu (SIGUSR1)

**Cel:** Weryfikacja przedwczesnego odjazdu autobusu

```bash
./main 2 15 5 10 &
# Po 3 sekundach:
killall -SIGUSR1 dispatcher
```

**Oczekiwany wynik:** W logach pojawia się `[DYSPOZYTOR] Wymuszenie odjazdu` przed upływem pełnych 10s

**Fragment logu:**
```
[14:36:35] [KIEROWCA 180132] Autobus na dworcu
[14:36:38] [DYSPOZYTOR] Wymuszenie odjazdu
[14:36:38] [KIEROWCA 180132] Odjazd: 4 pasazerow, 3 rowerow
```

**Analiza:** Autobus odjechał po 3 sekundach zamiast czekać 10 sekund

---

### Test 3: Blokada dworca (SIGUSR2)

**Cel:** Sprawdzenie blokady i kontrolowanego zakończenia systemu

```bash
./main 2 10 3 8 &
sleep 10
killall -SIGUSR2 dispatcher
```

**Oczekiwany wynik:** Komunikaty "Blokada dworca", procesy kończą pracę, sprzątanie zasobów

**Fragment logu:**
```
[14:26:50] [DYSPOZYTOR] Blokada dworca
[14:26:51] [KIEROWCA 179213] Koniec pracy
[14:26:51] [KIEROWCA 179214] Koniec pracy
[14:26:52] [KASA] Koniec pracy
[14:26:52] [GENERATOR] Koniec pracy
[14:26:52] [DYSPOZYTOR] Koniec pracy
[14:26:52] [MAIN] System zakonczony
```

**Analiza:** System stopniowo zamyka się po otrzymaniu SIGUSR2

---

### Test 4: Obsługa pasażerów VIP

**Cel:** Weryfikacja rejestracji VIP bez oczekiwania na bilet

```bash
./main 3 20 5 8
```

**Oczekiwany wynik:** W logach wpisy `[KASA] Rejestracja PID ... VIP=1 ...` bez wysyłania biletów

**Fragment logu:**
```
[14:15:42] [PASAZER 185432] Przybycie (VIP=1 wiek=45 rower=0 dziecko=0)
[14:15:42] [KASA] Rejestracja PID=185432 VIP=1 DZIECKO=0
[14:15:42] [PASAZER 185432] Wsiadl (VIP=1 rower=0)
```

**Analiza:** Pasażer VIP nie czekał na bilet z kasy, od razu wszedł

---

### Test 5: Dziecko bez opiekuna

**Cel:** Odmowa wejścia dziecku < 8 lat bez dorosłego

```bash
./main 2 10 3 8
```

**Oczekiwany wynik:** Logi zawierają `[DZIECKO ...] Bez opiekuna - odmowa`

**Fragment logu:**
```
[14:26:47] [PASAZER 179234] Przybycie (VIP=0 wiek=5 rower=0 dziecko=0)
[14:26:47] [DZIECKO 179234] Bez opiekuna - odmowa
```

**Analiza:** Dziecko w wieku 5 lat nie może podróżować samo

---

### Test 6: Dorośli z dziećmi

**Cel:** Sprawdzenie mechanizmu fork() + pipe dla rodzin

```bash
./main 2 15 5 5
```

**Oczekiwany wynik:** 
- Rejestracja dorosłego (osobny PID)
- Dziecko NIE rejestruje się osobno w kasie
- Zajęcie 2 miejsc w autobusie
- Synchronizacja przez pipe

**Fragment logu:**
```
[14:20:15] [PASAZER 187650] Przybycie (VIP=0 wiek=35 rower=0 dziecko=1)
[14:20:15] [KASA] Rejestracja PID=187650 VIP=0 DZIECKO=0
[14:20:16] [DOROSLY+DZIECKO 187650] Wsiadl (VIP=0 rower=0)
```

**Analiza:** Dorosły z dzieckiem zajął 2 miejsca, dziecko nie rejestrowało się osobno

---

### Test 7: Stress test (wiele procesów)

**Cel:** Sprawdzenie synchronizacji przy jednoczesnym dostępie wielu procesów

```bash
./main 5 50 20 4
# Czekamy 60 sekund
killall -SIGINT main
```

**Oczekiwany wynik:** 
- Brak deadlocków
- Wszystkie procesy kończą się poprawnie
- Prawidłowa synchronizacja semaforów
- Logi bez błędów

**Fragment logu:**
```
[14:11:37] [KIEROWCA 177739] Odjazd: 50 pasazerow, 20 rowerow
[14:11:38] [KIEROWCA 177740] Autobus na dworcu
[14:11:42] [KIEROWCA 177741] Odjazd: 12 pasazerow, 5 rowerow
[14:11:43] [MAIN] Shutdown initiated
[14:11:45] [MAIN] System zakonczony
```

**Analiza:** System obsłużył dużą liczbę procesów bez problemów

---

### Test 8: Sprzątanie zasobów IPC

**Cel:** Upewnienie się, że zasoby zostały usunięte po zakończeniu

```bash
./main 2 10 3 8 &
sleep 10
killall -SIGINT main
# Po zakończeniu:
ipcs -m  # pamięć dzielona
ipcs -s  # semafory
ipcs -q  # kolejki komunikatów
```

**Oczekiwany wynik:** Brak pozostałości w systemie

```
------ Message Queues --------
key        msqid      owner      perms      used-bytes   messages    

------ Shared Memory Segments --------
key        shmid      owner      perms      bytes      nattch     status      

------ Semaphore Arrays --------
key        semid      owner      perms      nsems     
```

**Analiza:** Wszystkie zasoby IPC zostały poprawnie usunięte przez `cleanup()`

---

## 🧹 Użyte mechanizmy systemowe

### Procesy
- `fork()` – tworzenie procesów potomnych (generator → pasażerowie, dorosły → dziecko)
- `execl()` – zastępowanie obrazu procesu (uruchamianie driver, cashier, itd.)
- `wait()`, `waitpid()` – oczekiwanie na zakończenie procesów potomnych
- `_exit()` – zakończenie procesu bez sprzątania stdio (w procesach potomnych)
- `getpid()` – uzyskanie PID bieżącego procesu

### Sygnały
- `sigaction()` – rejestracja obsługi sygnałów (zamiast przestarzałego `signal()`)
- `kill()` – wysyłanie sygnałów do innych procesów
- `pause()` – wstrzymanie procesu do otrzymania sygnału
- **SIGUSR1** – sygnał użytkownika 1 (wymuszenie odjazdu)
- **SIGUSR2** – sygnał użytkownika 2 (blokada dworca)
- **SIGINT** – sygnał przerwania (Ctrl+C)
- **SIGCHLD** – sygnał zakończenia procesu potomnego

### IPC (System V)
- `ftok()` – generowanie kluczy IPC na podstawie ścieżki pliku i identyfikatora projektu
- **Pamięć dzielona**: 
  - `shmget()` – tworzenie/dostęp do segmentu pamięci dzielonej
  - `shmat()` – dołączanie segmentu do przestrzeni adresowej procesu
  - `shmdt()` – odłączanie segmentu
  - `shmctl(IPC_RMID)` – usunięcie segmentu
- **Semafory**: 
  - `semget()` – tworzenie/dostęp do zestawu semaforów
  - `semop()` – operacje atomowe na semaforach (P, V)
  - `semctl(SETVAL)` – ustawienie wartości początkowej
  - `semctl(IPC_RMID)` – usunięcie zestawu
- **Kolejki komunikatów**: 
  - `msgget()` – tworzenie/dostęp do kolejki komunikatów
  - `msgsnd()` – wysłanie wiadomości
  - `msgrcv()` – odbiór wiadomości (blokujący lub IPC_NOWAIT)
  - `msgctl(IPC_RMID)` – usunięcie kolejki

### Pipe (komunikacja rodzic-dziecko)
- `pipe()` – tworzenie łącza nienazwanego (unidirectional)
- `read()` – odczyt z pipe
- `write()` – zapis do pipe
- `close()` – zamknięcie końca pipe

### Pliki
- `creat()` – tworzenie pliku
- `open()` – otwieranie pliku z flagami (O_CREAT, O_WRONLY, O_APPEND)
- `write()` – zapis do pliku (logowanie)
- `close()` – zamknięcie deskryptora pliku
- `unlink()` – usuwanie pliku

### Inne
- `time()`, `localtime()`, `strftime()` – znaczniki czasowe w logach
- `srand()`, `rand()` – generowanie losowych wartości (VIP, rower, wiek, dziecko, Ti)
- `sleep()` – opóźnienia (czekanie T, jazda Ti, opóźnienia generatora)


---

## 👤 Autor

**Gabriela Pater**  
Projekt na zajęcia z Systemów Operacyjnych

---

