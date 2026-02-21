# Makefile - Symulacja dworca autobusowego
#
# -D_POSIX_C_SOURCE=200809L  włącza usleep(), nanosleep(), strftime() itp.
#                             bez ostrzeżeń o niejawnych deklaracjach
# -Wall -Wextra               wszystkie ostrzeżenia
# -pedantic                   zgodność ze standardem
# -g                          symbole debugowania (usuń dla release)

CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -g -D_POSIX_C_SOURCE=200809L
LDFLAGS =

# Pliki wykonywalne
TARGETS = main driver passenger cashier dispatcher passenger_generator

# Wspólny nagłówek
DEPS = ipc.h

.PHONY: all clean run logs

all: $(TARGETS)

# Każdy plik .c kompiluje się do osobnego pliku wykonywalnego
# (każdy to oddzielny process exec-owany przez main/generator)

main: main.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

driver: driver.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

passenger: passenger.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

cashier: cashier.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

dispatcher: dispatcher.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

passenger_generator: passenger_generator.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Uruchomienie z domyślnymi parametrami: 2 autobusy, 10 miejsc, 3 rowery, 5s postój
run: all
	./main 2 10 3 5

# Podgląd logów
logs:
	@echo "=== report.txt ===" && tail -50 report.txt 2>/dev/null || true
	@echo "=== driver.log ===" && tail -20 driver.log 2>/dev/null || true
	@echo "=== passenger.log ===" && tail -20 passenger.log 2>/dev/null || true

# Czyszczenie plików binarnych i logów
clean:
	rm -f $(TARGETS) *.log report.txt bus_shm.key bus_sem.key bus_msg.key bus_msg_reply.key

# Czyszczenie IPC po awarii (jeśli program nie posprzątał)
clean-ipc:
	@echo "Czyszczenie zasobów IPC..."
	@ipcs -m | awk '/^0x/ {print $$2}' | xargs -r ipcrm -m 2>/dev/null || true
	@ipcs -s | awk '/^0x/ {print $$2}' | xargs -r ipcrm -s 2>/dev/null || true
	@ipcs -q | awk '/^0x/ {print $$2}' | xargs -r ipcrm -q 2>/dev/null || true
	@echo "Gotowe."
