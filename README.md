
# Autobus Podmiejski — Symulacja wieloprocesowa

  

## Opis projektu

Projekt polega na stworzeniu symulacji systemu autobusów podmiejskich w środowisku wieloprocesowym.

Każdy element (pasażerowie, kasa, kierowca, dyspozytor) działa jako oddzielny proces i komunikuje się za pomocą mechanizmów IPC (kolejki komunikatów, semafory, pamięć współdzielona).

Celem jest odwzorowanie sytuacji na dworcu, gdzie autobusy odjeżdżają cyklicznie lub na polecenie dyspozytora, a pasażerowie pojawiają się w losowych momentach.

---

  

## Założenia systemu

- Na stanowisku zawsze stoi jeden autobus:

- **P** miejsc dla pasażerów,

- **R** miejsc dla pasażerów z rowerami.

- Odjazdy co **T** jednostek czasu lub na sygnał dyspozytora.

- System dysponuje **N** autobusami.

- Pasażerowie kupują bilety w kasie (VIP pomijają kolejkę).

- Dzieci poniżej 8 lat mogą podróżować tylko z opiekunem.

- Kierowca kontroluje bilety i limity miejsc.

- Dyspozytor może:

- **Sygnał 1** – wymusić natychmiastowy odjazd,

- **Sygnał 2** – zablokować wejście na dworzec.

  

---

  

## Scenariusze testowe

Testy sprawdzają różne sytuacje w systemie:

  

### Test 1 – Standardowy ruch

Normalny napływ pasażerów, odjazdy zgodnie z harmonogramem.

Sprawdzamy, czy autobusy rotują poprawnie i limity miejsc są zachowane.

  

### Test 2 – Duża liczba rowerów

Wysoki odsetek pasażerów z rowerami (ok. 40%).

System powinien odrzucać osoby ponad limit **R**, nawet jeśli są wolne miejsca ogólne.

  

### Test 3 – Blokada wejścia (sygnał 2)

Dyspozytor wysyła sygnał blokady.

Od tego momentu nikt nie może wejść na dworzec ani do kolejki.

Raport powinien odnotować próby wejścia w tym czasie.

  

### Test 4 – Dzieci bez opiekuna

Generowana jest populacja z dziećmi poniżej 8 lat.

Sprawdzamy, czy system poprawnie odrzuca dzieci bez dorosłego opiekuna.

  

### Test 5 – Wczesny odjazd (sygnał 1)

Dyspozytor wymusza wcześniejszy odjazd.

Autobus odjeżdża z niepełną liczbą pasażerów, a pozostali czekają na kolejny kurs.

  

---


## Technologie

- C  – implementacja procesów i komunikacji.

- Mechanizmy IPC: kolejki komunikatów, semafory, pamięć współdzielona.

  

---

  

## Raport z działania

Symulacja generuje raport zawierający:

- chronologiczną listę akcji procesów,

- próby wejścia do autobusu (udane i odrzucone),

- momenty wysłania sygnałów przez dyspozytora,

- czasy odjazdów i powrotów autobusów.

  
---

## Wsparcie dla systemu Linux

Symulacja została przetestowana i działa poprawnie w środowisku **Linux**.  
Testy zostały przeprowadzone na platformie **Raspberry Pi 3B+** z systemem **Raspberry Pi OS**.

Aplikacja uruchamia się z linii poleceń i nie wymaga dodatkowych modyfikacji systemowych.  
Zalecane jest korzystanie z aktualnej wersji systemu oraz standardowych narzędzi dostępnych w dystrybucji.



## Jak uruchomić używając Makefile:
Kompilacja wszystkich plików: `make` 

Uruchomienie symulacji: `make run` 

 Czyszczenie plików binarnych:`make clean`
