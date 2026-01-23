# 🚍 Symulacja „Autobus podmiejski"

Zaawansowana symulacja systemu obsługi autobusów podmiejskich działająca na procesach z użyciem **System V IPC** (pamięć dzielona, semafory, kolejki komunikatów), sygnałów oraz plików.

## 📦 Funkcjonalności

### Podstawowe mechanizmy
- **Flota N autobusów** o pojemności **P pasażerów** i **R miejsc na rowery**
- **Dwa niezależne wejścia** (normalne / z rowerem) — synchronizowane semaforami bramek
- **Inteligentny system odjazdów** co T sekund z możliwością wymuszenia (SIGUSR1)
- **Losowe czasy powrotu** Ti ∈ [3,9] sekund dla każdego kursu
- **Tylko jeden autobus na dworcu** — synchronizacja przez semafor dworca

### Obsługa pasażerów
- **Kasa biletowa** — rejestruje wszystkich pasażerów, wystawia bilety dla zwykłych pasażerów
- **Pasażerowie VIP (~1%)** — mają wcześniej zakupiony bilet, tylko rejestracja
- **Dzieci < 8 lat** — wymagają obecności dorosłego opiekuna
- **Dorośli z dziećmi** — zajmują 2 miejsca, dziecko tworzy osobny proces
- **Pasażerowie z rowerami** — używają dedykowanej bramki

### Kontrola systemu
- **Dyspozytor** — nadzoruje pracę kierowców, może wymusić odjazd lub zablokować dworzec
- **Blokada dworca (SIGUSR2)** — stopniowe zamknięcie z oczekiwaniem na zakończenie procesów
- **Raport szczegółowy** w `report.txt` z timestampami wszystkich zdarzeń
- **Graceful shutdown** po SIGINT — czyszczenie zasobów IPC

## 🖥️ Wymagania

- **System**: Linux (testowane na Ubuntu/Debian/Raspberry Pi OS)
- **Kompilator**: GCC z obsługą C99
- **Pakiety**: `build-essential`, `make`
- Brak zewnętrznych bibliotek — tylko standardowe nagłówki POSIX/System V

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

### Czyszczenie zasobów:

```bash
make clean
```

Usuwa binarki, logi, pliki kluczy IPC i czyści zasoby systemowe.

## ▶️ Uruchomienie

Program główny wymaga **5 parametrów**:

```bash
./main N P R T TOTAL
```

**Parametry:**
- `N` — liczba autobusów w flocie (1-100)
- `P` — maksymalna pojemność pasażerów (1-1000)
- `R` — liczba miejsc na rowery (0-100)
- `T` — czas oczekiwania na dworcu w sekundach (1-3600)
- `TOTAL` — całkowita liczba pasażerów do wygenerowania (1-10000)

### Przykłady uruchomienia:

#### Mała symulacja (test funkcjonalności):
```bash
./main 2 10 5 3 20
```
**Opis:** 2 autobusy, 10 pasażerów max, 5 rowerów max, 3s oczekiwanie, 20 pasażerów ogółem

#### Średnia symulacja (typowe użycie):
```bash
./main 3 20 10 5 50
```
**Opis:** 3 autobusy, 20 pasażerów max, 10 rowerów max, 5s oczekiwanie, 50 pasażerów ogółem

#### Duża symulacja (test stabilności):
```bash
./main 5 50 20 4 100
```
**Opis:** 5 autobusów, 50 pasażerów max, 20 rowerów max, 4s oczekiwanie, 100 pasażerów ogółem

## 📝 System logowania

Wszystkie zdarzenia zapisują się do pliku:

```
report.txt
```

### Format logów:

```
[HH:MM:SS] [MODUŁ] Opis zdarzenia
```

### Rodzaje zdarzeń w logu:

| Moduł | Przykładowe zdarzenia |
|-------|----------------------|
| **MAIN** | Start systemu, zakończenie systemu |
| **GENERATOR** | Utworzenie pasażera, zakończenie generowania |
| **KASA** | Rejestracja pasażera (VIP/DZIECKO), koniec pracy |
| **PASAZER** | Przybycie, wejście do autobusu, odmowy |
| **DZIECKO** | Odmowa (brak opiekuna) |
| **DOROSLY+DZIECKO** | Wejście z dzieckiem |
| **KIEROWCA** | Przyjazd, odjazd (liczba pasażerów/rowerów), powrót |
| **DYSPOZYTOR** | Wymuszenie odjazdu, blokada dworca |

### Podgląd logów na żywo:

```bash
tail -f report.txt
```

### Przykładowy fragment logu:

```
[18:42:15] [MAIN] Start systemu: N=3 P=20 R=10 T=5 TOTAL=50
[18:42:15] [GENERATOR] Start - utworzy 50 pasazerow
[18:42:15] [KASA] Start pracy
[18:42:15] [DYSPOZYTOR] Start pracy
[18:42:15] [KIEROWCA 12345] Start pracy
[18:42:15] [KIEROWCA 12345] Autobus na dworcu
[18:42:16] [PASAZER 12350] Przybycie (VIP=0 wiek=25 rower=1 dziecko=0)
[18:42:16] [KASA] Rejestracja PID=12350 VIP=0 DZIECKO=0
[18:42:16] [PASAZER 12350] Wsiadl
[18:42:18] [PASAZER 12355] Przybycie (VIP=0 wiek=32 rower=0 dziecko=1)
[18:42:18] [KASA] Rejestracja PID=12355 VIP=0 DZIECKO=0
[18:42:18] [KASA] Rejestracja PID=12356 VIP=0 DZIECKO=1
[18:42:18] [DOROSLY+DZIECKO 12355] Wsiadl
[18:42:20] [KIEROWCA 12345] Odjazd: 3 pasazerow, 1 rowerow
[18:42:27] [KIEROWCA 12345] Powrot po 7s
```

> **Uwaga:** Plik `report.txt` jest dopisywany (append). Czyszczony przy każdym `make clean`.

## 🧭 Sterowanie sygnałami

System reaguje na następujące sygnały:

| Sygnał | Źródło | Odbiorca | Działanie |
|--------|--------|----------|-----------|
| **SIGUSR1** | Dyspozytor | Kierowca | Wymuszenie natychmiastowego odjazdu autobusu |
| **SIGUSR2** | Dyspozytor | Kierowca | Blokada dworca i rozpoczęcie shutdown |
| **SIGINT** (Ctrl+C) | Użytkownik | Main → Dyspozytor | Graceful shutdown całego systemu |
| **SIGCHLD** | Kernel | Main, Generator | Zbieranie zombie processes |

### Szczegółowe działanie sygnałów:

#### SIGUSR1 — Wymuszenie odjazdu
```bash
killall -SIGUSR1 dispatcher
```
- Dyspozytor przekazuje sygnał do kierowcy aktualnie na dworcu
- Autobus odjeżdża natychmiast (skrócenie czasu T)
- Logowane jako `[DYSPOZYTOR] Wymuszenie odjazdu`

#### SIGUSR2 — Blokada dworca
```bash
killall -SIGUSR2 dispatcher
```
- Ustawienie flag `station_blocked=1` i `shutdown=1`
- Nowi pasażerowie nie mogą wejść
- Kierowcy kończą pracę po powrocie z kursu
- Logowane jako `[DYSPOZYTOR] Blokada dworca`

#### SIGINT — Shutdown systemu
```bash
# W terminalu z uruchomionym programem:
Ctrl+C
```
- Ustawienie flag `shutdown=1`, `station_blocked=1`
- Przekazanie SIGINT do dyspozytora
- Czekanie na zakończenie wszystkich procesów potomnych
- Czyszczenie zasobów IPC (shm, sem, msg)
- Usunięcie plików kluczy

## 🔐 Mechanizmy synchronizacji

### Semafory (4 semafory w zestawie):

| Indeks | Początkowa wartość | Przeznaczenie |
|--------|-------------------|---------------|
| **0** | 1 | Ochrona pamięci dzielonej (mutex) |
| **1** | 1 | Bramka dla pasażerów bez roweru |
| **2** | 1 | Bramka dla pasażerów z rowerem |
| **3** | 1 | Dostęp do dworca (tylko 1 autobus) |

### Pamięć dzielona (struktura `BusState`):

```c
struct BusState {
    int P;                      // Maksymalna liczba pasażerów
    int R;                      // Maksymalna liczba rowerów
    int T;                      // Czas oczekiwania na dworcu
    int N;                      // Liczba autobusów
    int passengers;             // Aktualna liczba pasażerów w autobusie
    int bikes;                  // Aktualna liczba rowerów w autobusie
    int departing;              // Flaga: autobus odjeżdża
    int station_blocked;        // Flaga: dworzec zablokowany
    int active_passengers;      // Liczba aktywnych pasażerów w systemie
    int total_passengers;       // Całkowita liczba pasażerów do obsłużenia
    int boarded_passengers;     // Liczba pasażerów, którzy weszli do autobusu
    pid_t driver_pid;           // PID aktualnego kierowcy na dworcu
    int shutdown;               // Flaga: system się wyłącza
    int cashier_done;           // Flaga: kasa zakończyła pracę
    int generator_done;         // Flaga: generator zakończył pracę
};
```

### Kolejka komunikatów:

**Typy wiadomości:**
- `MSG_REGISTER (1)` — Rejestracja pasażera w kasie
- `MSG_TICKET_REPLY + PID` — Unikalny typ odpowiedzi dla każdego pasażera

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

## 🔄 Przepływ procesów

### 1. Inicjalizacja (main.c)
```
main → fork() → driver (x N)
     → fork() → cashier
     → fork() → dispatcher
     → fork() → passenger_generator
```

### 2. Generator pasażerów
- Co 1-3 sekundy tworzy nowy proces pasażera
- Inkrementuje `active_passengers`
- Po utworzeniu TOTAL pasażerów ustawia `generator_done=1`

### 3. Pasażer (passenger.c)
```
Losowanie: VIP (1%), rower (50%), wiek, dziecko (20% dla dorosłych)
    ↓
Sprawdzenie: dworzec otwarty?
    ↓
Dziecko < 8 lat? → KONIEC (bez opiekuna)
    ↓
Rejestracja w kasie (msgid)
    ↓
VIP? TAK → omijamy czekanie
    ↓ NIE
Czekanie na bilet (msgrcv)
    ↓
Dorośli z dzieckiem? → fork() dziecka + pipe
    ↓
Pętla próby wejścia (try_board):
    - Blokada odpowiedniej bramki (1 lub 2)
    - Sprawdzenie miejsca (P, R)
    - Wejście lub czekanie
    ↓
Dekrementacja active_passengers
```

### 4. Kierowca (driver.c)
```
Pętla nieskończona:
    ↓
Zablokuj semafor dworca (tylko 1 autobus)
    ↓
Czekaj T sekund (lub SIGUSR1)
    ↓
Sprawdź warunki shutdown
    ↓
Zablokuj obie bramki
    ↓
Ustaw departing=1
    ↓
Zaloguj odjazd (passengers, bikes)
    ↓
Wyzeruj liczniki
    ↓
Odblokuj bramki i dworzec
    ↓
Jazda (sleep Ti ∈ [3,9]s)
    ↓
Powrót
```

### 5. Kasa (cashier.c)
```
Pętla nieskończona:
    ↓
msgrcv(MSG_REGISTER, IPC_NOWAIT)
    ↓
Znaleziono? → Zaloguj rejestrację
    ↓
VIP lub dziecko? → Pomiń wysyłanie biletu
    ↓ NIE
msgsnd(MSG_TICKET_REPLY + PID)
    ↓
Sprawdź warunki zakończenia:
    - shutdown=1
    - generator_done && active_passengers=0
    ↓
Ustaw cashier_done=1
```

### 6. Dyspozytor (dispatcher.c)
```
Rejestracja handlerów:
    - SIGINT → shutdown
    - SIGUSR1 → wymuszenie odjazdu
    - SIGUSR2 → blokada dworca
    ↓
pause() — czekanie na sygnał
    ↓
Przekazanie sygnałów do kierowcy
```

## ✅ Przykładowe testy

### Test 1: Limit pojemności pasażerów i rowerów

**Cel:** Sprawdzenie odmowy wejścia po osiągnięciu limitu

```bash
./main 2 10 3 8 50
```

**Oczekiwany wynik:** Pierwsze 10 osób wchodzi, reszta czeka na następny autobus

**Fragment logu:**
```
[18:42:01] [PASAZER 180677] Wsiadl
[18:42:01] [PASAZER 180678] Wsiadl
[18:42:05] [KIEROWCA 180650] Odjazd: 10 pasazerow, 3 rowerow
```

---

### Test 2: Wymuszenie odjazdu (SIGUSR1)

**Cel:** Weryfikacja przedwczesnego odjazdu autobusu

```bash
./main 2 15 5 10 20 &
# Po 3 sekundach:
killall -SIGUSR1 dispatcher
```

**Oczekiwany wynik:** W logach pojawia się `[DYSPOZYTOR] Wymuszenie odjazdu` przed upływem pełnych 10s

**Fragment logu:**
```
[18:36:35] [KIEROWCA 180132] Autobus na dworcu
[18:36:38] [DYSPOZYTOR] Wymuszenie odjazdu
[18:36:38] [KIEROWCA 180132] Odjazd: 4 pasazerow, 3 rowerow
```

---

### Test 3: Blokada dworca (SIGUSR2)

**Cel:** Sprawdzenie blokady i kontrolowanego zakończenia systemu

```bash
./main 2 10 3 8 30 &
sleep 10
killall -SIGUSR2 dispatcher
```

**Oczekiwany wynik:** Komunikaty "Blokada dworca", kasa i kierowcy kończą pracę, sprzątanie zasobów

**Fragment logu:**
```
[18:26:50] [DYSPOZYTOR] Blokada dworca
[18:26:50] [KIEROWCA 179213] Koniec pracy
[18:26:51] [KASA] Koniec pracy
[18:26:51] [MAIN] System zakonczony
```

---

### Test 4: Obsługa pasażerów VIP

**Cel:** Weryfikacja rejestracji VIP bez oczekiwania na bilet

```bash
./main 3 20 5 8 200
```

**Oczekiwany wynik:** W logach wpisy `[KASA] Rejestracja PID ... VIP=1 ...` bez wysyłania biletów

**Fragment logu:**
```
[19:15:42] [KASA] Rejestracja PID=185432 VIP=1 DZIECKO=0
[19:15:42] [PASAZER 185432] Wsiadl
```

---

### Test 5: Dziecko bez opiekuna

**Cel:** Odmowa wejścia dziecku < 8 lat bez dorosłego

```bash
./main 2 10 3 8 50
```

**Oczekiwany wynik:** Logi zawierają `[DZIECKO ...] Bez opiekuna - odmowa`

**Fragment logu:**
```
[18:26:47] [DZIECKO 179234] Bez opiekuna - odmowa
```

---

### Test 6: Dorośli z dziećmi

**Cel:** Sprawdzenie mechanizmu fork() + pipe dla rodzin

```bash
./main 2 15 5 5 30
```

**Oczekiwany wynik:** 
- Rejestracja dorosłego i dziecka (osobne PID)
- Zajęcie 2 miejsc w autobusie
- Synchronizacja przez pipe

**Fragment logu:**
```
[19:20:15] [PASAZER 187650] Przybycie (VIP=0 wiek=35 rower=0 dziecko=1)
[19:20:15] [KASA] Rejestracja PID=187650 VIP=0 DZIECKO=0
[19:20:15] [KASA] Rejestracja PID=187651 VIP=0 DZIECKO=1
[19:20:16] [DOROSLY+DZIECKO 187650] Wsiadl
```

---

### Test 7: Stress test (wiele procesów)

**Cel:** Sprawdzenie synchronizacji przy jednoczesnym dostępie wielu procesów

```bash
./main 5 50 20 4 100
```

**Oczekiwany wynik:** 
- Brak deadlocków
- Wszystkie procesy kończą się poprawnie
- Prawidłowa synchronizacja semaforów

**Fragment logu:**
```
[18:11:37] [KIEROWCA 177739] Odjazd: 50 pasazerow, 20 rowerow
[18:11:38] [KIEROWCA 177740] Autobus na dworcu
[18:11:42] [KIEROWCA 177741] Odjazd: 0 pasazerow, 0 rowerow
[18:11:43] [DYSPOZYTOR] Blokada dworca
[18:11:43] [MAIN] System zakonczony
```

---

### Test 8: Sprzątanie zasobów IPC

**Cel:** Upewnienie się, że zasoby zostały usunięte po zakończeniu

```bash
./main 2 10 3 8 25
# Po zakończeniu:
ipcs -m  # pamięć dzielona
ipcs -s  # semafory
ipcs -q  # kolejki komunikatów
```

**Oczekiwany wynik:** Brak pozostałości w systemie

```txt
------ Message Queues --------
key        msqid      owner      perms      used-bytes   messages    

------ Shared Memory Segments --------
key        shmid      owner      perms      bytes      nattch     status      

------ Semaphore Arrays --------
key        semid      owner      perms      nsems     
```

---

## 🧹 Użyte mechanizmy systemowe

### Procesy:
- `fork()` – tworzenie procesów potomnych
- `execl()` – zastępowanie obrazu procesu
- `wait()`, `waitpid()` – oczekiwanie na zakończenie procesów potomnych
- `_exit()` – zakończenie procesu bez sprzątania stdio

### Sygnały:
- `sigaction()` – rejestracja obsługi sygnałów
- `kill()` – wysyłanie sygnałów do innych procesów
- `SIGUSR1`, `SIGUSR2`, `SIGINT`, `SIGCHLD` – sygnały sterujące

### IPC (System V):
- `ftok()` – generowanie kluczy IPC
- **Pamięć dzielona**: `shmget()`, `shmat()`, `shmdt()`, `shmctl()`
- **Semafory**: `semget()`, `semop()`, `semctl()`
- **Kolejki komunikatów**: `msgget()`, `msgsnd()`, `msgrcv()`, `msgctl()`

### Pipe (komunikacja rodzic-dziecko):
- `pipe()` – tworzenie łącza nienazwanego
- `read()`, `write()` – komunikacja między dorosłym a dzieckiem

### Pliki:
- `creat()`, `open()`, `close()` – operacje na plikach
- `write()` – zapis do logów
- `unlink()` – usuwanie plików

---

## 👤 Autor

Gabriela Pater  
Projekt na zajęcia z Systemów Operacyjnych

---

**Uwaga:** Program wymaga uprawnień do tworzenia zasobów IPC. W przypadku problemów sprawdź uprawnienia użytkownika i dostępność zasobów systemowych.