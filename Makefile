CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
TARGETS = main driver passenger cashier dispatcher passenger_generator

all: $(TARGETS)

main: main.c ipc.h
        $(CC) $(CFLAGS) -o main main.c

driver: driver.c ipc.h
        $(CC) $(CFLAGS) -o driver driver.c

passenger: passenger.c ipc.h
        $(CC) $(CFLAGS) -o passenger passenger.c

cashier: cashier.c ipc.h
        $(CC) $(CFLAGS) -o cashier cashier.c

dispatcher: dispatcher.c ipc.h
        $(CC) $(CFLAGS) -o dispatcher dispatcher.c

passenger_generator: passenger_generator.c ipc.h
        $(CC) $(CFLAGS) -o passenger_generator passenger_generator.c

clean:
        rm -f $(TARGETS) *.log report.txt *.key
        ipcrm -a 2>/dev/null || true

.PHONY: all clean