# Makefile – Symulacja dworca autobusowego
#
# Projekt składa się z sześciu osobnych plików wykonywalnych:
#   main               – proces nadrzędny, tworzy zasoby IPC i uruchamia pozostałe procesy
#   driver             – proces kierowcy autobusu (instancjonowany N razy)
#   passenger          – proces pasażera (instancjonowany dynamicznie przez generator)
#   cashier            – proces kasjera obsługującego kolejkę rejestracji
#   dispatcher         – proces dyspozytora reagującego na sygnały zewnętrzne
#   passenger_generator – proces generatora tworzącego pasażerów
#
# Flagi kompilacji:
#   -D_POSIX_C_SOURCE=200809L  Udostępnia interfejsy POSIX.1-2008 (m.in. strftime,
#                              sigaction, sem_t) bez ostrzeżeń o niejawnych deklaracjach.
#   -Wall -Wextra              Włącza pełny zestaw ostrzeżeń kompilatora.
#   -pedantic                  Wymusza zgodność z wybranym standardem języka C.
#   -g                         Generuje symbole debugowania (usunąć przy kompilacji produkcyjnej).

CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -g -D_POSIX_C_SOURCE=200809L
LDFLAGS =

# Lista wszystkich plików wykonywalnych projektu
TARGETS = main driver passenger cashier dispatcher passenger_generator

# Wspólny nagłówek definiujący struktury i stałe IPC
DEPS = ipc.h

.PHONY: all clean run logs clean-ipc

# Domyślna reguła: kompilacja wszystkich plików wykonywalnych
all: $(TARGETS)

# Każdy moduł jest kompilowany jako osobny plik wykonywalny.
# main uruchamia driver, cashier, dispatcher i passenger_generator przez execl().
# passenger_generator uruchamia passenger przez execl().

main: main.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

driver: driver.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# passenger wymaga biblioteki pthreads ze względu na wątek dziecka (child_thread)
passenger: passenger.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread

cashier: cashier.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

dispatcher: dispatcher.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

passenger_generator: passenger_generator.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Uruchomienie symulacji z domyślnymi parametrami:
#   N=2 autobusy, P=10 miejsc pasażerskich, R=3 miejsca rowerowe, T=5s postój na dworcu
run: all
	./main 2 10 3 5

# Wyświetlenie ostatnich wpisów z dzienników symulacji
logs:
	@echo "=== report.txt ===" && tail -50 report.txt 2>/dev/null || true
	@echo "=== driver.log ===" && tail -20 driver.log 2>/dev/null || true
	@echo "=== passenger.log ===" && tail -20 passenger.log 2>/dev/null || true

# Usunięcie plików wykonywalnych, dzienników i plików kluczy IPC
clean:
	rm -f $(TARGETS) *.log report.txt bus_shm.key bus_sem.key bus_msg.key bus_msg_reply.key

# Awaryjne czyszczenie zasobów IPC pozostałych po nieprawidłowym zakończeniu programu.
# Przydatne gdy main nie zdążył wywołać cleanup() przed przerwaniem procesu.
clean-ipc:
	@echo "Czyszczenie zasobów IPC..."
	@ipcs -m | awk '/^0x/ {print $$2}' | xargs -r ipcrm -m 2>/dev/null || true
	@ipcs -s | awk '/^0x/ {print $$2}' | xargs -r ipcrm -s 2>/dev/null || true
	@ipcs -q | awk '/^0x/ {print $$2}' | xargs -r ipcrm -q 2>/dev/null || true
	@echo "Gotowe."
