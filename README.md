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
- [Nawigacja po kodzie źródłowym](#-nawigacja-po-kodzie-źródłowym)

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

# Testy - System zarządzania autobusami

## Test 1: Maksymalne wypełnienie autobusu - race condition

**Cel**: Sprawdzenie czy dokładnie P pasażerów wsiada, nawet gdy więcej próbuje jednocześnie

**Parametry**: `./main 1 5 2 10`
- 1 autobus
- 5 miejsc
- 2 rowery
- 10 sekund czasu oczekiwania

**Scenariusz**: 
- Generator tworzy 20 pasażerów w krótkim czasie (1-3s)
- Wszyscy próbują wsiąść do jednego małego autobusu
- Część będzie musiała czekać na następny kurs

**Przykładowe logi (fragment)**:
```
[14:23:10] [KIEROWCA 1001] Autobus na dworcu
[14:23:11] [PASAZER 2001] Przybycie (VIP=0 wiek=25 rower=0 dziecko=0)
[14:23:11] [PASAZER 2001] Wsiadl (VIP=0 rower=0)
[14:23:12] [PASAZER 2002] Przybycie (VIP=0 wiek=30 rower=1 dziecko=0)
[14:23:12] [PASAZER 2002] Wsiadl (VIP=0 rower=1)
[14:23:13] [PASAZER 2003] Przybycie (VIP=0 wiek=45 rower=0 dziecko=0)
[14:23:13] [PASAZER 2003] Wsiadl (VIP=0 rower=0)
[14:23:13] [PASAZER 2004] Przybycie (VIP=0 wiek=35 rower=1 dziecko=0)
[14:23:13] [PASAZER 2004] Wsiadl (VIP=0 rower=1)
[14:23:14] [PASAZER 2005] Przybycie (VIP=0 wiek=50 rower=0 dziecko=0)
[14:23:14] [PASAZER 2005] Wsiadl (VIP=0 rower=0)
[14:23:14] [PASAZER 2006] Przybycie (VIP=0 wiek=28 rower=0 dziecko=0)
[14:23:14] [PASAZER 2007] Przybycie (VIP=0 wiek=33 rower=1 dziecko=0)
[14:23:24] [KIEROWCA 1001] Odjazd: 5 pasazerow, 2 rowerow
[14:23:24] [KIEROWCA 1001] Autobus na dworcu (wrócił po trasie)
[14:23:25] [PASAZER 2006] Wsiadl (VIP=0 rower=0)
[14:23:25] [PASAZER 2007] Wsiadl (VIP=0 rower=1)
```

**Weryfikacja**: 
- ✅ Pierwszy odjazd: DOKŁADNIE 5 pasażerów i 2 rowery
- ✅ Pasażerowie 2006 i 2007 czekają na następny autobus
- ✅ Brak przekroczenia limitu P=5, R=2

---

## Test 2: Wielokrotny Ctrl+Z podczas aktywnego ruchu

**Cel**: Sprawdzenie odporności na zawieszanie/wznawianie procesu głównego

**Parametry**: `./main 3 10 5 8`

**Scenariusz**:
1. Uruchom system
2. Poczekaj aż kilku pasażerów zacznie wsiadać
3. Naciśnij **Ctrl+Z** (zatrzymanie)
4. Poczekaj 5 sekund
5. Wznów przez **fg**
6. Powtórz 3-4 razy
7. Naciśnij Ctrl+C (shutdown)

**Przykładowe logi**:
```
[15:10:15] [KIEROWCA 3001] Autobus na dworcu
[15:10:16] [PASAZER 4001] Wsiadl (VIP=0 rower=1)
[15:10:17] [PASAZER 4002] Wsiadl (VIP=0 rower=0)
^Z
[1]+  Stopped                 ./main 3 10 5 8

(czekamy 5 sekund, potem fg)

[15:10:23] [PASAZER 4003] Wsiadl (VIP=0 rower=0)
[15:10:24] [KIEROWCA 3001] Odjazd: 3 pasazerow, 1 rowerow
[15:10:25] [KIEROWCA 3002] Autobus na dworcu
[15:10:26] [PASAZER 4004] Wsiadl (VIP=0 rower=1)
^Z
[1]+  Stopped                 ./main 3 10 5 8

(znowu fg)

[15:10:35] [KIEROWCA 3001] Powrot po 4s
[15:10:36] [PASAZER 4005] Wsiadl (VIP=0 rower=0)
```

**Weryfikacja**:
- ✅ System kontynuuje działanie po każdym fg
- ✅ Brak "Odjazd: 0 pasazerow" (co by oznaczało zdublowane autobusy)
- ✅ Kolejność logów jest spójna
- ✅ Brak deadlocków na semaforach

---

## Test 3: Rodzice z dziećmi - ekstremalna synchronizacja

**Cel**: Test synchronizacji procesów rodzic-dziecko

**Parametry**: `./main 2 6 3 15`

**Scenariusz**: 
- Modyfikujemy generator aby tworzył TYLKO pasażerów z dziećmi (100% zamiast 20%)
- Każdy rodzic+dziecko zajmuje 2 miejsca
- Autobus pomieści tylko 3 pary

**Przykładowe logi**:
```
[16:20:10] [KIEROWCA 5001] Autobus na dworcu
[16:20:11] [PASAZER 6001] Przybycie (VIP=0 wiek=35 rower=0 dziecko=1)
[16:20:12] [DOROSLY+DZIECKO 6001] Wsiadl (VIP=0 rower=0)
[16:20:13] [PASAZER 6002] Przybycie (VIP=0 wiek=28 rower=1 dziecko=1)
[16:20:14] [DOROSLY+DZIECKO 6002] Wsiadl (VIP=0 rower=1)
[16:20:15] [PASAZER 6003] Przybycie (VIP=0 wiek=42 rower=0 dziecko=1)
[16:20:16] [DOROSLY+DZIECKO 6003] Wsiadl (VIP=0 rower=0)
[16:20:17] [PASAZER 6004] Przybycie (VIP=0 wiek=31 rower=0 dziecko=1)
[16:20:25] [KIEROWCA 5001] Odjazd: 6 pasazerow, 1 rowerow
[16:20:26] [KIEROWCA 5002] Autobus na dworcu
[16:20:27] [DOROSLY+DZIECKO 6004] Wsiadl (VIP=0 rower=0)
```

**Weryfikacja**:
- ✅ Odjazd: 6 pasażerów (3 pary × 2 miejsca)
- ✅ Każde "DOROSLY+DZIECKO" zajmuje dokładnie 2 miejsca
- ✅ Czwarta para czeka na następny autobus
- ✅ Brak wpisów "[DZIECKO XXX] Bez opiekuna" (dzieci są z rodzicami)

---

## Test 4: SIGUSR1 - wymuszenie odjazdu w krytycznym momencie

**Cel**: Test wymuszenia odjazdu gdy pasażerowie wsiadają

**Parametry**: `./main 2 10 5 20`

**Scenariusz**:
1. Uruchom system
2. Poczekaj aż 3-4 pasażerów wsiądzie
3. Wyślij `kill -SIGUSR1 <PID_DYSPOZYTORA>` (natychmiastowy odjazd)
4. Sprawdź czy autobus odjeżdża z tymi pasażerami

**Przykładowe logi**:
```
[17:30:10] [KIEROWCA 7001] Autobus na dworcu
[17:30:12] [PASAZER 8001] Wsiadl (VIP=0 rower=0)
[17:30:13] [PASAZER 8002] Wsiadl (VIP=0 rower=1)
[17:30:14] [PASAZER 8003] Wsiadl (VIP=0 rower=0)
[17:30:15] [DYSPOZYTOR] Wymuszenie odjazdu
[17:30:15] [KIEROWCA 7001] Odjazd: 3 pasazerow, 1 rowerow
[17:30:15] [KIEROWCA 7002] Autobus na dworcu
[17:30:16] [PASAZER 8004] Przybycie (VIP=0 wiek=45 rower=0 dziecko=0)
[17:30:17] [PASAZER 8004] Wsiadl (VIP=0 rower=0)
```

**Weryfikacja**:
- ✅ Autobus odjeżdża NATYCHMIAST po SIGUSR1 (nie czeka T=20s)
- ✅ Zabiera pasażerów którzy już wsiedli (3 osoby, 1 rower)
- ✅ Następny autobus natychmiast wjeżdża
- ✅ Nowi pasażerowie wsiadają do nowego autobusu

---

## Test 5: SIGUSR2 - blokada dworca gdy autobusy są w trasie

**Cel**: Test graceful shutdown gdy część autobusów jest w trasie

**Parametry**: `./main 3 8 4 10`

**Scenariusz**:
1. Uruchom system
2. Poczekaj aż 2 autobusy będą w trasie (po logu "Odjazd")
3. Wyślij `kill -SIGUSR2 <PID_DYSPOZYTORA>`
4. Sprawdź czy wszystkie autobusy bezpiecznie kończą trasy

**Przykładowe logi**:
```
[18:15:10] [KIEROWCA 9001] Odjazd: 8 pasazerow, 3 rowerow
[18:15:12] [KIEROWCA 9002] Odjazd: 7 pasazerow, 4 rowerow
[18:15:13] [KIEROWCA 9003] Autobus na dworcu
[18:15:14] [DYSPOZYTOR] Blokada dworca
[18:15:14] [DYSPOZYTOR] Koniec pracy
[18:15:14] [KIEROWCA 9003] Koniec pracy
[18:15:14] [KASA] Koniec pracy
[18:15:14] [GENERATOR] Koniec pracy
[18:15:14] [PASAZER 10001] Dworzec zamkniety przed rejestracją
[18:15:16] [KIEROWCA 9001] Powrot po 6s
[18:15:16] [KIEROWCA 9001] Koniec pracy
[18:15:18] [KIEROWCA 9002] Powrot po 6s
[18:15:18] [KIEROWCA 9002] Koniec pracy
[18:15:19] [MAIN] System zakonczony
```

**Weryfikacja**:
- ✅ Autobusy w trasie (9001, 9002) kończą swoje trasy
- ✅ Autobus na dworcu (9003) kończy natychmiast
- ✅ Wszystkie procesy kończą się w odpowiedniej kolejności
- ✅ Nowi pasażerowie dostają "Dworzec zamkniety"
- ✅ Brak zombie processów

---

## Test 6: Minimalna pojemność - bardzo mały autobus

**Cel**: Test skrajnego przypadku P=1, R=1

**Parametry**: `./main 1 1 1 2`
- 1 autobus
- 1 miejsce (!)
- 1 rower (!)
- 2 sekundy czasu

**Scenariusz**:
- Generator tworzy
- Wszyscy muszą jechać po kolei

**Przykładowe logi**:
```
[10:41:20] [KASA] Rejestracja PID=2719399 VIP=0 DZIECKO=0
[10:41:21] [KIEROWCA 2719392] Powrot po 3s
[10:41:21] [KIEROWCA 2719392] Autobus na dworcu
[10:41:21] [PASAZER 2719397] Wsiadl (VIP=0 rower=0)
[10:41:21] [PASAZER 2719400] Przybycie (VIP=0 wiek=23 rower=0 dziecko=1)
[10:41:22] [KASA] Rejestracja PID=2719400 VIP=0 DZIECKO=0
[10:41:23] [KIEROWCA 2719392] Odjazd: 1 pasazerow, 0 rowerow
[10:41:23] [PASAZER 2719402] Przybycie (VIP=0 wiek=23 rower=0 dziecko=1)
[10:41:24] [KASA] Rejestracja PID=2719402 VIP=0 DZIECKO=0
[10:41:24] [PASAZER 2719405] Przybycie (VIP=0 wiek=23 rower=0 dziecko=1)
[10:41:25] [KASA] Rejestracja PID=2719405 VIP=0 DZIECKO=0
[10:41:26] [PASAZER 2719409] Przybycie (VIP=0 wiek=58 rower=0 dziecko=0)
[10:41:27] [KASA] Rejestracja PID=2719409 VIP=0 DZIECKO=0
[10:41:29] [PASAZER 2719410] Przybycie (VIP=0 wiek=24 rower=0 dziecko=0)
[10:41:30] [KASA] Rejestracja PID=2719410 VIP=0 DZIECKO=0
[10:41:31] [PASAZER 2719467] Przybycie (VIP=0 wiek=73 rower=0 dziecko=0)
[10:41:31] [KASA] Rejestracja PID=2719467 VIP=0 DZIECKO=0
[10:41:32] [KIEROWCA 2719392] Powrot po 9s
[10:41:32] [KIEROWCA 2719392] Autobus na dworcu
[10:41:32] [PASAZER 2719472] Przybycie (VIP=0 wiek=44 rower=0 dziecko=0)
[10:41:32] [PASAZER 2719399] Wsiadl (VIP=0 rower=0)
```

**Weryfikacja**:
- ✅ Każdy odjazd: DOKŁADNIE 1 pasażer, 0 lub 1 rowerów
- ✅ Pasażerowie wsiadają po kolei
- ✅ System nie deadlockuje mimo ekstremalnych ograniczeń

---

## Test 7: Maksymalna konkurencja - 10 autobusów, duży ruch

**Cel**: Stress test z wieloma autobusami i pasażerami

**Parametry**: `./main 10 15 8 3`
- 10 autobusów
- 15 miejsc
- 8 rowerów
- 3 sekundy czasu (bardzo szybki obrót)

**Scenariusz**:
- Generator tworzy pasażerów co 1-3s
- Wiele autobusów jednocześnie na dworcu
- Szybka rotacja - autobusy wracają bardzo szybko

**Przykładowe logi (fragment)**:
```
[20:10:00] [KIEROWCA 13001] Autobus na dworcu
[20:10:01] [PASAZER 14001] Wsiadl (VIP=0 rower=1)
[20:10:02] [PASAZER 14002] Wsiadl (VIP=0 rower=0)
[20:10:03] [KIEROWCA 13001] Odjazd: 2 pasazerow, 1 rowerow
[20:10:03] [KIEROWCA 13002] Autobus na dworcu
[20:10:04] [PASAZER 14003] Wsiadl (VIP=0 rower=1)
[20:10:05] [PASAZER 14004] Wsiadl (VIP=0 rower=0)
[20:10:06] [KIEROWCA 13002] Odjazd: 2 pasazerow, 1 rowerow
[20:10:06] [KIEROWCA 13003] Autobus na dworcu
[20:10:07] [KIEROWCA 13001] Powrot po 4s
[20:10:07] [PASAZER 14005] Wsiadl (VIP=0 rower=0)
[20:10:09] [KIEROWCA 13003] Odjazd: 1 pasazerow, 0 rowerow
[20:10:09] [KIEROWCA 13004] Autobus na dworcu
[20:10:10] [KIEROWCA 13002] Powrot po 4s
```

**Weryfikacja**:
- ✅ W KAŻDEJ chwili MAX 1 autobus na dworcu (semafor gate[3])
- ✅ Brak nakładających się "Autobus na dworcu" bez "Odjazd"
- ✅ Wszystkie powroty są logiczne (czas 3-9s)
- ✅ Brak deadlocków mimo 10 autobusów

---

## Test 8: Samymi VIPami

**Cel**: Test czy VIPowie nie potrzebują biletów

**Parametry**: `./main 2 10 5 10`

**Scenariusz**:
- Modyfikujemy kod aby 100% pasażerów to VIPowie (zamiast 1%)
- VIPowie nie rejestrują się w kasie przed wejściem do autobusu


**Przykładowe logi**:
```
[21:00:10] [KIEROWCA 15001] Autobus na dworcu
[21:00:11] [PASAZER 16001] Przybycie (VIP=1 wiek=45 rower=0 dziecko=0)
[21:00:11] [PASAZER 16001] Wsiadl (VIP=1 rower=0)
[21:00:12] [PASAZER 16002] Przybycie (VIP=1 wiek=52 rower=1 dziecko=0)
[21:00:12] [PASAZER 16002] Wsiadl (VIP=1 rower=1)
[21:00:13] [PASAZER 16003] Przybycie (VIP=1 wiek=38 rower=0 dziecko=0)
[21:00:13] [PASAZER 16003] Wsiadl (VIP=1 rower=0)
```

**Weryfikacja**:
- ✅ BRAK logów "KASA Rejestracja" dla VIPów przed wejściem do autobusu
- ✅ VIPowie wsiadają NATYCHMIAST bez czekania na bilet

---

## Test 9: Dzieci bez opiekuna - masowe odrzucenie

**Cel**: Test odrzucania dzieci poniżej 8 lat

**Parametry**: `./main 2 10 5 10`

**Scenariusz**:
- Modyfikujemy generator aby tworzył tylko dzieci 0-7 lat
- Wszystkie powinny zostać odrzucone

**Przykładowe logi**:
```
[22:00:10] [KIEROWCA 17001] Autobus na dworcu
[22:00:11] [PASAZER 18001] Przybycie (VIP=0 wiek=3 rower=0 dziecko=0)
[22:00:11] [DZIECKO 18001] Bez opiekuna - odmowa
[22:00:12] [PASAZER 18002] Przybycie (VIP=0 wiek=5 rower=1 dziecko=0)
[22:00:12] [DZIECKO 18002] Bez opiekuna - odmowa
[22:00:13] [PASAZER 18003] Przybycie (VIP=0 wiek=7 rower=0 dziecko=0)
[22:00:13] [DZIECKO 18003] Bez opiekuna - odmowa
[22:00:23] [KIEROWCA 17001] Odjazd: 0 pasazerow, 0 rowerow
```

**Weryfikacja**:
- ✅ Wszystkie dzieci <8 lat są odrzucane
- ✅ Autobus odjeżdża pusty (bo nikt nie może wsiąść)
- ✅ Brak crashy, system działa stabilnie

---

## Test 10: Długotrwałe działanie - test stabilności

**Cel**: Sprawdzenie czy system nie ma wycieków pamięci/zasobów

**Parametry**: `./main 5 20 10 5`

**Scenariusz**:
- Uruchom system
- Pozwól działać 30 minut
- Monitoruj użycie pamięci (ps aux, top)
- Zakończ przez Ctrl+C

**Weryfikacja**:
- ✅ Użycie pamięci stabilne (brak wycieków)
- ✅ Liczba procesów stabilna (brak zombie)
- ✅ System działa sprawnie przez cały czas
- ✅ Graceful shutdown działa poprawnie po długim czasie działania
- ✅ Wszystkie zasoby IPC są poprawnie czyszczone (`ipcs` po zakończeniu)

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
## 🔗 Nawigacja po kodzie źródłowym

### 📚 Definicje i struktury danych

| Element | Opis | Link do kodu |
|---------|------|--------------|
| **`struct BusState`** | Struktura w pamięci dzielonej - stan autobusu, liczniki pasażerów/rowerów, flagi shutdown | [ipc.h#L37-L60](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/ipc.h#L37-L60) |
| **`struct msg`** | Struktura wiadomości w kolejce - rejestracja i bilety | [ipc.h#L69-L82](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/ipc.h#L69-L82) |
| **Typy wiadomości** | `MSG_REGISTER`, `MSG_TICKET_REPLY` - stałe dla komunikacji | [ipc.h#L25-L26](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/ipc.h#L25-L26) |
| **Ścieżki kluczy IPC** | `SHM_PATH`, `SEM_PATH`, `MSG_PATH` - pliki dla `ftok()` | [ipc.h#L19-L21](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/ipc.h#L19-L21) |

---

### 🎯 Inicjalizacja systemu (main.c)

| Funkcjonalność | Opis | Link do kodu |
|----------------|------|--------------|
| **Parsowanie argumentów** | Odczyt N, P, R, T z `argv[]` | [main.c#L152-L155](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/main.c#L152-L155) |
| **Tworzenie kluczy IPC** | `creat()` + `ftok()` dla SHM, SEM, MSG | [main.c#L174-L182](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/main.c#L174-L182) |
| **Inicjalizacja pamięci dzielonej** | `shmget()` + `shmat()` + inicjalizacja `BusState` | [main.c#L192-L205](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/main.c#L192-L205) |
| **Inicjalizacja semaforów** | `semget()` + `semctl(SETVAL)` dla 4 semaforów | [main.c#L213-L224](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/main.c#L213-L224) |
| **Inicjalizacja kolejki** | `msgget()` dla komunikacji kasa-pasażer | [main.c#L228-L233](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/main.c#L228-L233) |
| **Tworzenie procesów** | `fork()` + `execl()` dla driver (×N), cashier, dispatcher, generator | [main.c#L277-L323](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/main.c#L277-L323) |
| **Główna pętla wait** | `wait()` - zbieranie zombie procesów | [main.c#L328](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/main.c#L328) |
| **Cleanup zasobów** | `shmctl()`, `semctl()`, `msgctl()` - usuwanie IPC | [main.c#L84-L98](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/main.c#L84-L98) |

---

### 🚌 Proces kierowcy (driver.c)

| Funkcjonalność | Opis | Link do kodu |
|----------------|------|--------------|
| **Rejestracja handlerów sygnałów** | `sigaction()` dla SIGUSR1, SIGUSR2 | [driver.c#L165-L187](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L165-L187) |
| **Inicjalizacja IPC** | `ftok()`, `shmget()`, `shmat()`, `semget()` | [driver.c#L139-L161](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L139-L161) |
| **Blokada dworca** | `gate_lock(3)` - tylko 1 autobus na dworcu | [driver.c#L90-L93](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L90-L93) |
| **Ustawienie PID kierowcy** | `bus->driver_pid = getpid()` w sekcji krytycznej | [driver.c#L209](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L209) |
| **Czekanie T sekund** | Pętla `sleep(1)` z obsługą `force_flag` | [driver.c#L229-L241](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L229-L241) |
| **Blokada bramek** | `gate_lock(1)` + `gate_lock(2)` przed odjazdem | [driver.c#L258-L259](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L258-L259) |
| **Ustawienie flagi odjazdu** | `bus->departing = 1` + odczyt liczników | [driver.c#L263-L266](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L263-L266) |
| **Reset liczników** | `bus->passengers = 0`, `bus->bikes = 0` | [driver.c#L278-L279](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L278-L279) |
| **Odblokowanie bramek** | `gate_unlock(1, 2, 3)` - zwolnienie zasobów | [driver.c#L283-L285](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L283-L285) |
| **Jazda (sleep Ti)** | `sleep(3 + rand() % 7)` - losowy czas trasy [3-9]s | [driver.c#L289-L290](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L289-L290) |
| **Handler SIGUSR1** | Ustawienie `force_flag = 1` - wymuszenie odjazdu | [driver.c#L110-L113](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L110-L113) |

---

### 💰 Proces kasy (cashier.c)

| Funkcjonalność | Opis | Link do kodu |
|----------------|------|--------------|
| **Inicjalizacja IPC** | `ftok()`, `shmget()`, `shmat()`, `msgget()` | [cashier.c#L79-L104](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/cashier.c#L79-L104) |
| **Główna pętla** | Sprawdzanie `shutdown` + `msgrcv()` z IPC_NOWAIT | [cashier.c#115-L174](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/cashier.c#L115-L174) |
| **Odbieranie rejestracji** | `msgrcv(MSG_REGISTER, IPC_NOWAIT)` | [cashier.c#L130](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/cashier.c#L130) |
| **Logowanie rejestracji** | Wpis do `report.txt` z PID, VIP, DZIECKO | [cashier.c#L156-L159](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/cashier.c#L156-L159) |
| **Wysyłanie biletu** | `msgsnd(MSG_TICKET_REPLY + PID)` dla nie-VIP dorosłych | [cashier.c#L169-L171](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/cashier.c#L169-L171) |
| **Cleanup** | `shmdt()`| [cashier.c#L181](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/cashier.c#L181) |

---

### 🎮 Proces dyspozytora (dispatcher.c)

| Funkcjonalność | Opis | Link do kodu |
|----------------|------|--------------|
| **Inicjalizacja IPC** | `ftok()`, `shmget()`, `shmat()`, `semget()` | [dispatcher.c#L116-L135](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/dispatcher.c#L116-L135) |
| **Handler SIGUSR1** | Wymuszenie odjazdu - `kill(driver_pid, SIGUSR1)` | [dispatcher.c#L76-L86](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/dispatcher.c#L76-L86) |
| **Handler SIGUSR2** | Blokada dworca - ustawienie flag shutdown | [dispatcher.c#L97-L112](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/dispatcher.c#L97-L112) |
| **Handler SIGINT** | Graceful shutdown - ustawienie flag | [dispatcher.c#L63-L66](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/dispatcher.c#L63-L66) |
| **Rejestracja handlerów** | `sigaction()` dla wszystkich sygnałów | [dispatcher.c#L144-L166](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/dispatcher.c#L144-L166) |
| **Główna pętla** | `pause()` - czekanie na sygnały | [dispatcher.c#L172](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/dispatcher.c#L172) |

---

### 👤 Proces pasażera (passenger.c)

| Funkcjonalność | Opis | Link do kodu |
|----------------|------|--------------|
| **Losowanie cech** | VIP (1%), rower (50%), wiek (0-79), dziecko (20%) | [passenger.c#L195-L200](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L195-L200) |
| **Sprawdzenie dworca** | `station_blocked` → "Dworzec zamknięty" | [passenger.c#L213-L216](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L213-L216) |
| **Odmowa dla samotnego dziecka** | Wiek <8 → "Bez opiekuna - odmowa" | [passenger.c#L231-L240](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L231-L240) |
| **Wysłanie rejestracji** | `msgsnd(MSG_REGISTER)` do kasy | [passenger.c#L269-L271](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L269-L271) |
| **Czekanie na bilet** | `msgrcv(MSG_TICKET_REPLY + PID)` dla nie-VIP | [passenger.c#L281](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L281) |
| **Fork dla dziecka** | `pipe()` + `fork()` dla synchronizacji | [passenger.c#L326-L344](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L326-L344) |
| **Proces dziecka** | `read()` z pipe - czekanie na sygnał rodzica | [passenger.c#L352](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L352) |
| **Funkcja try_board()** | Atomowa próba wejścia - sprawdzenie miejsc | [passenger.c#L123-L165](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L123-L165) |
| **Blokada bramki** | `gate_lock(1 lub 2)` w zależności od roweru | [passenger.c#L125-L128](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L125-L128) |
| **Sprawdzenie warunków** | `shutdown`, `departing`, wolne miejsca | [passenger.c#L141-L156](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L141-L156) |
| **Wejście do autobusu** | `bus->passengers += needed`, `bus->bikes++` | [passenger.c#L159-L160](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L159-L160) |
| **Pętla prób wejścia** | Wywołania `try_board()` ze `sleep(1)` | [passenger.c#L374,L406](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L374-L406) |
| **Sygnał dla dziecka** | `write()` do pipe po udanym wejściu | [passenger.c#L389](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L389) |
| **Dekrementacja licznika** | `bus->active_passengers--` przed wyjściem | [passenger.c#L437](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L437) |

---

### 🔄 Generator pasażerów (passenger_generator.c)

| Funkcjonalność | Opis | Link do kodu |
|----------------|------|--------------|
| **Inicjalizacja IPC** | `ftok()`, `shmget()`, `shmat()`, `semget()` | [passenger_generator.c#L106-L128](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger_generator.c#L106-L128) |
| **Główna pętla** | Nieskończona pętla `for(;;)` | [passenger_generator.c#L154-L199](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger_generator.c#L154-L199) |
| **Losowe opóźnienie** | `sleep(1 + rand() % 3)` - co 1-3 sekundy | [passenger_generator.c#L157-L158](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger_generator.c#L157-L158) |
| **Sprawdzenie shutdown** | `shutdown` lub `station_blocked` → koniec | [passenger_generator.c#L162-L165](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger_generator.c#L162-L165) |
| **Inkrementacja licznika** | `bus->active_passengers++` przed fork | [passenger_generator.c#L175](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger_generator.c#L175) |
| **Fork pasażera** | `fork()` + `execl("./passenger")` | [passenger_generator.c#L179-L195](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger_generator.c#L179-L195) |



---

### 🔍 Kluczowe sekcje krytyczne

| Sekcja krytyczna | Chroniony zasób | Gdzie | Link |
|------------------|-----------------|-------|------|
| **Próba wejścia pasażera** | `bus->passengers`, `bus->bikes` | passenger.c | [L159-L160](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L159-L160) |
| **Odjazd autobusu** | `bus->departing`, liczniki | driver.c | [L263](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L263) |
| **Przyjazd na dworzec** | `bus->driver_pid`, `bus->departing` | driver.c | [L210](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/driver.c#L210) |
| **Tworzenie pasażera** | `bus->active_passengers` | passenger_generator.c | [L174-L176](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger_generator.c#L174-L176) |
| **Koniec pasażera** | `bus->active_passengers` | passenger.c | [L450](https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/main/passenger.c#L450) |


---

## 👤 Autor

**Gabriela Pater**  
Projekt na zajęcia z Systemów Operacyjnych

---

