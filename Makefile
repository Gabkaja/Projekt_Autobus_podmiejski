# Makefile do projektu Autobus Podmiejski

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L

.PHONY: all clean run

all: main driver cashier dispatcher passenger

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
        ./main 2 10 3 8 25

clean:
        rm -f main driver cashier dispatcher passenger report.txt bus_shm.key bus_sem.key bus_msg.key
