CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
TARGETS = main driver cashier dispatcher passenger passenger_generator

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

passenger_generator: passenger_generator.c ipc.h
        $(CC) $(CFLAGS) -o passenger_generator passenger_generator.c

clean:
        rm -f $(TARGETS) report.txt *.key
        ipcs -m | grep $(USER) | awk '{print $$2}' | xargs -r ipcrm -m
        ipcs -s | grep $(USER) | awk '{print $$2}' | xargs -r ipcrm -s
        ipcs -q | grep $(USER) | awk '{print $$2}' | xargs -r ipcrm -q

.PHONY: all clean