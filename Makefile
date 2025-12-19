# Makefile do projektu Autobus Podmiejski

CC = gcc
CFLAGS = -Wall -std=c99
TARGETS = main driver cashier dispatcher passenger

all: $(TARGETS)

main: main.c ipc.h
	$(CC) $(CFLAGS) -o main main.c

driver: driver.c ipc.h
	$(CC) $(CFLAGS) -o driver driver.c

cashier: cashier.c ipc.h
	$(CC) $(CFLAGS) -o cashier cashier.c

dispatcher: dispatcher.c ipc.h
	$(CC) $(CFLAGS) -o dispatcher dispatcher.c

passenger: passenger.c ipc.h
	$(CC) $(CFLAGS) -o passenger passenger.c

run: all
	./main

clean:
	rm -f $(TARGETS)
