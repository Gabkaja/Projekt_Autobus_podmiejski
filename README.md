# 🚍 Symulacja „Autobus podmiejski"

Symulacja systemu obsługi autobusów podmiejskich działająca na procesach z użyciem **System V IPC** (pamięć dzielona, semafory, kolejki komunikatów), sygnałów oraz plików.

## 📦 Funkcjonalności

- **Flota N autobusów** o pojemności **P pasażerów** i **R miejsc na rowery**
- **Dwa wejścia do autobusu** (normalne / rowery) — przez każdą bramkę może przejść jedna osoba na raz
- **Odjazdy co T sekund** + możliwość wymuszenia odjazdu (dyspozytor → SIGUSR1)
- **Losowe czasy powrotu** Ti (3-9s) dla każdego kursu
- **Kasa** rejestruje wszystkich pasażerów; zwykli pasażerowie dostają odpowiedź „bilet OK"
- **VIP (~1%)**: mają wcześniej bilet, omijają płatność, ale są rejestrowani
- **Dzieci < 8 lat**: tylko z dorosłym opiekunem; dziecko zajmuje osobne miejsce
- **Blokada dworca (SIGUSR2)**: pasażerowie nie wchodzą, system kończy pracę
- **Raport z przebiegu** w pliku `report.txt` (append)
- **Kontrolowane zakończenie** po Ctrl+C (SIGINT) — sprzątanie IPC i zamknięcie procesów

## 🖥️ Wymagania

- **System**: Linux (testowane na Ubuntu/Debian)
- **Pakiety**: `gcc`, `make`
- Brak dodatkowych bibliotek (używane standardowe nagłówki systemowe POSIX/System V)



## 📁 Struktura projektu

```
.
├── ipc.h              # Definicje struktur i stałych IPC
├── main.c             # Proces główny (uruchamia całą symulację)
├── driver.c           # Proces kierowcy autobusu
├── cashier.c          # Proces kasy biletowej
├── dispatcher.c       # Proces dyspozytora (sygnały)
├── passenger.c        # Proces pasażera
├── Makefile           # Automatyzacja kompilacji
└── report.txt         # Log zdarzeń (tworzony automatycznie)
```

## 🔨 Kompilacja

W katalogu projektu:

```bash
make
```

Powstaną pliki wykonywalne:
- `./main`
- `./driver`
- `./cashier`
- `./dispatcher`
- `./passenger`

### Czyszczenie:

```bash
make clean
```

Usuwa binarki, logi i zasoby IPC.

## ▶️ Uruchomienie

Program główny wymaga 5 parametrów:

```bash
./main N P R T TOTAL
```

**Gdzie:**
- `N` – liczba autobusów (1-100)
- `P` – pojemność pasażerów (1-1000)
- `R` – liczba miejsc na rowery (0-100)
- `T` – czas oczekiwania na dworcu w sekundach (1-3600)
- `TOTAL` – całkowita liczba pasażerów w symulacji (1-10000)

### Przykład:

```bash
./main 2 10 5 3 20
```

**Opis:** 2 autobusy, 10 pasażerów max, 5 rowerów max, 3s oczekiwanie, 20 pasażerów total

### Alternatywnie (domyślne parametry):

```bash
make run
```

## 📝 Logi

Wszystkie zdarzenia zapisują się do:

```
report.txt
```

**Zdarzenia w logu:**
- Rejestracje w kasie
- Wejścia do autobusu
- Odjazdy i powroty
- Blokady dworca
- Zakończenia pracy procesów

### Podgląd na żywo:

```bash
tail -f report.txt
```

> **Uwaga:** Plik `report.txt` jest dopisywany (append). Czyszczony przy `make clean`.

## 🧭 Sterowanie i sygnały

| Sygnał | Źródło | Opis |
|--------|--------|------|
| **SIGUSR1** | Dyspozytor → Kierowca | Wymuszenie odjazdu autobusu (po ~5s) |
| **SIGUSR2** | Dyspozytor → Kierowca | Blokada dworca (po ~10s) |
| **SIGINT** (Ctrl+C) | Użytkownik → Main | Grzeczne zakończenie symulacji |

### Działanie sygnałów:

- **SIGUSR1**: Autobus odjeżdża natychmiast, nawet jeśli nie upłynął pełny czas T
- **SIGUSR2**: Dworzec zostaje zablokowany, nowi pasażerowie nie wchodzą, kierowcy kończą pracę
- **SIGINT**: Ustawienie `shutdown=1`, sprzątanie IPC (shm, sem, msg), zakończenie wszystkich procesów

## 🔎 Przykładowy przebieg (skrócony)

```
[22:32:08] [KIEROWCA 66932] Autobus na dworcu
[22:32:08] [KASA] Rejestracja PID 66939 VIP 0 DZIECKO 0
[22:32:08] [PASAZER 66939] Wejscie
[22:32:08] [DZIECKO 66938] Bez opiekuna
[22:32:08] [DOROSLY+DZIECKO 66948] Wejscie
[22:32:08] [PASAZER 67030] Brak miejsca
[22:32:12] [KIEROWCA 66932] Odjazd: 50 pasazerow, 20 rowerow
[22:32:12] [KIEROWCA 66932] Powrot po 7s
[22:32:13] [DYSPOZYTOR] Wymuszenie odjazdu
[22:32:18] [DYSPOZYTOR] Blokada dworca
[22:32:18] [KIEROWCA 66935] Koniec pracy
[22:32:18] [KASA] Koniec pracy
```

## 🧹 Sprzątanie zasobów

Po zakończeniu lub przed ponownym uruchomieniem:

```bash
make clean
```

**Usuwa:**
- Binarki (`main`, `driver`, `cashier`, `dispatcher`, `passenger`)
- Pliki tymczasowe (`report.txt`, `bus_*.key`)
- Zasoby IPC (pamięć dzielona, semafory, kolejki komunikatów)

### Ręczne sprawdzenie zasobów IPC:

```bash
ipcs -m  # pamięć dzielona
ipcs -s  # semafory
ipcs -q  # kolejki komunikatów
```

### Ręczne usunięcie (jeśli make clean nie zadziałał):

```bash
ipcrm -a  # usuwa wszystkie zasoby IPC użytkownika
```

## ⚙️ Użyte mechanizmy systemowe

### Procesy:
- `fork()` – tworzenie procesów potomnych
- `execl()` – zastępowanie obrazu procesu
- `wait()` – oczekiwanie na zakończenie procesów potomnych
- `_exit()` – zakończenie procesu bez sprzątania stdio

### Sygnały:
- `sigaction()` – rejestracja obsługi sygnałów
- `kill()` – wysyłanie sygnałów do innych procesów
- `SIGUSR1`, `SIGUSR2`, `SIGINT` – sygnały sterujące

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

## ✅ Przykładowe testy

### Test 1: Limit pojemności pasażerów i rowerów

**Cel:** Sprawdzenie odmowy wejścia po osiągnięciu limitu

```bash
./main 2 10 3 8 50
```

**Oczekiwany wynik:** Pierwsze 10 osób wchodzi, reszta dostaje "Brak miejsca"

---

### Test 2: Wymuszenie odjazdu (SIGUSR1)

**Cel:** Weryfikacja przedwczesnego odjazdu

```bash
./main 2 15 5 10 20
```

**Oczekiwany wynik:** W logach pojawia się "[DYSPOZYTOR] Wymuszenie odjazdu" przed upływem pełnych 10s

---

### Test 3: Blokada dworca (SIGUSR2)

**Cel:** Sprawdzenie blokady i zakończenia systemu

```bash
./main 2 10 3 8 30
```

**Oczekiwany wynik:** Po ~10s komunikaty "Blokada dworca", kasa i kierowcy kończą pracę

---

### Test 4: Obsługa pasażerów VIP

**Cel:** Weryfikacja rejestracji VIP bez oczekiwania na bilet

```bash
./main 3 20 5 8 200
```

**Oczekiwany wynik:** W logach wpisy "[KASA] Rejestracja PID ... VIP 1 ..."

---

### Test 5: Dziecko bez opiekuna

**Cel:** Odmowa wejścia dziecku < 8 lat bez dorosłego

```bash
./main 2 10 3 8 50
```

**Oczekiwany wynik:** Logi zawierają "[DZIECKO ...] Bez opiekuna"

---

### Test 6: Stress test (bez sleep)

**Cel:** Sprawdzenie synchronizacji przy jednoczesnym dostępie wielu procesów

```bash
./main 5 50 20 4 100
```

**Oczekiwany wynik:** Brak deadlocków, wszystkie procesy kończą się poprawnie, synchronizacja działa

---

### Test 7: Sprzątanie zasobów IPC

**Cel:** Upewnienie się, że zasoby zostały usunięte

```bash
./main 2 10 3 8 25
# Po zakończeniu:
ipcs -m  # pamięć dzielona
ipcs -s  # semafory
ipcs -q  # kolejki komunikatów
```

**Oczekiwany wynik:** Brak pozostałości po symulacji w systemie

---

## 🔧 Parametry dostosowane do limitów systemowych

Dla systemu z limitami (w moim przypadku wynoszące tyle):
- `max user processes = 2528`
- `open files = 1024`

### Bezpieczne maksimum:

```bash
./main 20 100 50 2 1000
```

**Procesy:** ~1222 (20 drivers + 1 cashier + 1 dispatcher + 1000 passengers + ~200 dzieci)

### Zalecane testy:

```bash
# Mały (szybki)
./main 2 10 5 3 20

# Średni (typowy)
./main 3 20 10 5 50

# Duży (stabilność)
./main 5 50 20 4 100
```

## 🐛 Rozwiązywanie problemów

### Problem: Program się nie kończy

**Rozwiązanie:**
```bash
# Ctrl+C w terminalu lub:
pkill -SIGINT main
```

### Problem: Pozostałe zasoby IPC po błędnym zakończeniu

**Rozwiązanie:**
```bash
ipcrm -a  # usuwa wszystkie zasoby IPC użytkownika
make clean
```

### Problem: "Permission denied" przy kompilacji

**Rozwiązanie:**
```bash
chmod +x *.c
make clean
make
```

### Problem: Zbyt wiele procesów

**Rozwiązanie:** Zmniejsz parametr `TOTAL` lub zwiększ system limit:
```bash
ulimit -u 4096
```
---
## 🔗 Linki do Kluczowych Fragmentów Kodu

### a. Tworzenie i obsługa plików

Wykorzystanie `open()`, `write()`, `close()` do logowania zdarzeń:

**driver.c**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/driver.c#L29C1-L41C2

---

### b. Tworzenie procesów

Podstawa architektury - `fork()` + `exec()`:

**main.c**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/main.c#L261C1-L306C6


---


### d. Obsługa sygnałów

**dispatcher.c - Handler SIGINT**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/dispatcher.c#L39C1-L56C2

**driver.c - Handler SIGUSR1**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/driver.c#L71C1-L74C2

**main.c - Rejestracja handlera**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/main.c#L243C5-L250C6

---

### e. Synchronizacja procesów (semafory)

**driver.c - Operacje semaforowe**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/driver.c#L43C1-L69C2

**main.c - Inicjalizacja semaforów**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/main.c#L196C4-L222C6

---

### f. Łącza nazwane i nienazwane (pipe)

**passenger.c - Synchronizacja rodzic-dziecko**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/passenger.c#L206C3-L243C14

---

### g. Segmenty pamięci dzielonej

**main.c - Tworzenie i inicjalizacja**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/main.c#L182C1-L194C6


**ipc.h - Struktura pamięci dzielonej (linie 12-24)**
```c
struct BusState {
    int P;                    // Pojemność pasażerów
    int R;                    // Pojemność rowerów
    int T;                    // Czas oczekiwania
    int N;                    // Liczba autobusów
    int passengers;           // Aktualna liczba w autobusie
    int bikes;                // Aktualna liczba rowerów
    int departing;            // Flaga odjazdu
    int station_blocked;      // Flaga blokady dworca
    int active_passengers;    // Liczba aktywnych procesów
    pid_t driver_pid;         // PID kierowcy na dworcu
    int shutdown;             // Flaga shutdown
};
```

**main.c - Usuwanie pamięci**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/main.c#L46C1-L49C6

---

### h. Kolejki komunikatów

**main.c - Tworzenie kolejki**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/main.c#L223C1-L229C6

**ipc.h - Struktura wiadomości (linie 26-33)**
```c
struct msg {
    long type;        // Typ: REGISTER lub PID pasażera
    pid_t pid;        // PID pasażera
    int vip;          // Czy VIP (0/1)
    int bike;         // Czy rower (0/1)
    int child;        // Czy dziecko (0/1)
    int ticket_ok;    // Czy bilet OK (0/1)
};
```

**passenger.c - Wysyłanie zapytania**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/passenger.c#L125C4-L135C6

**cashier.c - Odbieranie i odpowiedź**

https://github.com/Gabkaja/Projekt_Autobus_podmiejski/blob/7d8c3b4949de56cdc9be5be789c8596f7732f9d6/cashier.c#L71C1-L112C6


---



## 👤 Autor

Gabriela Pater
Projekt na zajęcia z Systemów Operacyjnych

---

**Uwaga:** Program wymaga uprawnień do tworzenia zasobów IPC. W przypadku problemów sprawdź limity systemowe (`ulimit -a`) i uprawnienia użytkownika.
