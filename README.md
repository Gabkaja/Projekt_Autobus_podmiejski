
# 🚍 Symulacja „Autobus podmiejski"

Symulacja systemu obsługi autobusów podmiejskich działająca na procesach z użyciem System V IPC (pamięć dzielona, semafory, kolejki komunikatów), sygnałów oraz plików.



---

## 📦 Funkcjonalności

- Flota N autobusów o pojemności P pasażerów i R miejsc na rowery
- Dwa wejścia do autobusu (bagaż / rowery) — przez każdą bramkę może przejść jedna osoba na raz
- Odjazdy co T sekund + możliwość wymuszenia odjazdu (dyspozytor SIGUSR1)
- Losowe czasy powrotu Ti dla każdego kursu
- Kasa rejestruje wszystkich pasażerów (VIP też); zwykli pasażerowie dostają odpowiedź „bilet OK"
- VIP (~1%): mają wcześniej bilet, omijają płatność, ale są rejestrowani
- Dzieci < 8 lat: tylko z dorosłym opiekunem; dziecko zajmuje osobne miejsce
- Blokada dworca (SIGUSR2): pasażerowie nie wchodzą, system kończy pracę bez „jeżdżenia na pusto"
- Raport z przebiegu w pliku `report.txt` (append)
- Kontrolowane zakończenie po Ctrl+C (SIGINT) — sprzątanie IPC i zamknięcie procesów

---

## 🖥️ Wymagania

- **System:** Linux
- **Pakiety:** gcc, make
- **Brak dodatkowych bibliotek** (używane standardowe nagłówki systemowe POSIX/System V)

### Instalacja wymaganych pakietów:
```bash
sudo apt update
sudo apt install build-essential
```

---

## 📁 Struktura projektu
```
.
├── ipc.h
├── main.c
├── driver.c
├── cashier.c
├── dispatcher.c
├── passenger.c
├── Makefile
└── report.txt         # tworzy się po uruchomieniu
```

---

## 🔨 Kompilacja

W katalogu projektu:
```bash
make
```

Powstaną pliki wykonywalne:
```
./main
./driver
./cashier
./dispatcher
./passenger
```

Czyszczenie:
```bash
make clean
```

---

## ▶️ Uruchomienie

Program główny wymaga parametrów:
```bash
./main N P R T TOTAL
```

Gdzie:

- **N** – liczba autobusów (np. 2)
- **P** – pojemność pasażerów (np. 10)
- **R** – liczba miejsc na rowery (np. 3)
- **T** – czas odjazdu w sekundach (np. 8)
- **TOTAL** – liczba pasażerów w symulacji (np. 25)

### Przykład:
```bash
./main 2 10 3 8 25
```

Alternatywnie:
```bash
make run
```

---

## 📝 Logi

Wszystkie zdarzenia (rejestracje w kasie, wejścia, odjazdy, powroty, blokady, zakończenia) zapisują się do:
```
report.txt
```

Podgląd na żywo:
```bash
tail -f report.txt
```

Domyślnie log jest dopisywany (append). Plik `report.txt` usuwany jest przy `make clean`.

---

## 🧭 Sterowanie i sygnały

- **SIGUSR1** — wymuszenie odjazdu aktualnego autobusu na dworcu (wysyłane przez dyspozytora po ~5 s)
- **SIGUSR2** — blokada dworca (po ~10 s): pasażerowie nie wchodzą, kasa kończy, kierowcy kończą bez jeżdżenia na pusto
- **Ctrl+C (SIGINT)** — grzeczne zakończenie:
  - ustawienie `shutdown=1`, blokada dworca
  - kierowcy kończą po bieżącej fazie
  - sprzątane są zasoby IPC (shm, sem, msg) i pliki kluczy

---

## 🔎 Przykładowy przebieg (skrócony)
```
[HH:MM:SS] [KIEROWCA] Autobus na dworcu
[HH:MM:SS] [KASA] Rejestracja PID ... VIP 0 DZIECKO 0
[HH:MM:SS] [PASAZER ...] Wejscie
...
[HH:MM:SS] [KIEROWCA] Odjazd: X pasazerow, Y rowerow
[HH:MM:SS] [DYSPOZYTOR] Blokada dworca
[HH:MM:SS] [PASAZER ...] Brak biletu/Anulacja
[HH:MM:SS] [KIEROWCA] Koniec pracy
```



---

## 🧹 Sprzątanie

Po zakończeniu lub przed ponownym uruchomieniem:
```bash
make clean
```

Usuwa:

- binarki (`main`, `driver`, `cashier`, `dispatcher`, `passenger`)
- pliki tymczasowe (`report.txt`, pliki kluczy `bus_*.key`)
- zasoby IPC są zwalniane przez `main` przy normalnym wyjściu

---

## ⚙️ Użyte mechanizmy systemowe

- **Procesy:** `fork()`, `exec()`, `wait()`
- **Sygnały:** `sigaction()`, `SIGUSR1`, `SIGUSR2`, `SIGINT`
- **IPC:** `ftok()`, `shmget/shmat/shmdt/shmctl`, `semget/semop/semctl`, `msgget/msgsnd/msgrcv`
- **Dodatkowo:** `pipe()` (synchronizacja dorosły–dziecko)
- **Pliki:** `creat()`, `open()`, `write()`, `read()`, `close()`, `unlink()`

---
## ✅ Przykładowe testy

Poniżej przedstawiono przykładowe scenariusze testowe wraz z ich celem. Testy mają na celu weryfikację poprawności działania mechanizmów projektu w różnych warunkach.

### Test 1: Limit pojemności pasażerów i rowerów

- **Opis:** Uruchomienie symulacji z parametrami `P=10`, `R=3` oraz większą liczbą pasażerów niż limit.
- **Cel:** Sprawdzenie, czy system poprawnie odmawia wejścia pasażerom po osiągnięciu maksymalnej pojemności autobusu i miejsc na rowery.
```bash
./main 2 10 3 8 50
```

### Test 2: Wymuszenie odjazdu (SIGUSR1)

- **Opis:** Dyspozytor wysyła sygnał `SIGUSR1` do kierowcy po kilku sekundach od startu.
- **Cel:** Weryfikacja, czy autobus odjeżdża z niepełnym składem pasażerów i zdarzenie jest poprawnie rejestrowane w logu.
```bash
./main 2 15 5 10 20
```

W logach powinien pojawić się wpis o wymuszonym odjeździe przed upływem pełnego czasu T.

### Test 3: Blokada dworca (SIGUSR2)

- **Opis:** Dyspozytor wysyła sygnał `SIGUSR2` po określonym czasie.
- **Cel:** Sprawdzenie, czy po blokadzie dworca pasażerowie nie wchodzą do autobusu, kasa kończy pracę, a kierowcy kończą cykl bez wykonywania pustych kursów.
```bash
./main 2 10 3 8 30
```

Po ~10s dyspozytor blokuje dworzec — w logach powinny pojawić się odpowiednie komunikaty o blokadzie i zakończeniu pracy.

### Test 4: Obsługa pasażerów VIP

- **Opis:** Symulacja z dużą liczbą pasażerów (np. `TOTAL=200`), aby zwiększyć prawdopodobieństwo wystąpienia VIP.
- **Cel:** Weryfikacja, czy pasażerowie VIP są rejestrowani w kasie, omijają płatność i wchodzą bez oczekiwania na bilet.
```bash
./main 3 20 5 8 200
```

W logach powinny pojawić się wpisy z oznaczeniem `VIP 1`.

### Test 5: Dziecko bez opiekuna

- **Opis:** Wylosowanie pasażera w wieku poniżej 8 lat bez dorosłego opiekuna.
- **Cel:** Sprawdzenie, czy system odmawia wejścia dziecku bez opiekuna i rejestruje zdarzenie w logu.
```bash
./main 2 10 3 8 50
```

W logach powinien pojawić się komunikat `"Bez opiekuna"` dla dzieci bez dorosłego towarzysza.

### Test 6: Zakończenie pracy bez blokady

- **Opis:** Wyłączenie wysyłania sygnału `SIGUSR2` w dyspozytorze i oczekiwanie na obsłużenie wszystkich pasażerów.
- **Cel:** Weryfikacja, czy kierowcy kończą pracę naturalnie po rozwiezieniu wszystkich pasażerów.

> **Uwaga:** Wymaga modyfikacji kodu dyspozytora (zakomentowanie wysyłania `SIGUSR2`).

### Test 7: Sprzątanie zasobów IPC

- **Opis:** Po zakończeniu symulacji sprawdzenie zasobów IPC w systemie.
- **Cel:** Upewnienie się, że pamięć dzielona, semafory i kolejki komunikatów zostały poprawnie usunięte.
```bash
./main 2 10 3 8 25
# Po zakończeniu:
ipcs -m  # pamięć dzielona
ipcs -s  # semafory
ipcs -q  # kolejki komunikatów
```

System nie powinien pozostawiać zasobów IPC związanych z symulacją.

---

