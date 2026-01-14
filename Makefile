CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread

TARGETS = main driver cashier dispatcher passenger

all: $(TARGETS)

main: main.c ipc.h
        $(CC) $(CFLAGS) -o main main.c $(LDFLAGS)

driver: driver.c ipc.h
        $(CC) $(CFLAGS) -o driver driver.c $(LDFLAGS)

cashier: cashier.c ipc.h
        $(CC) $(CFLAGS) -o cashier cashier.c $(LDFLAGS)

dispatcher: dispatcher.c ipc.h
        $(CC) $(CFLAGS) -o dispatcher dispatcher.c $(LDFLAGS)

passenger: passenger.c ipc.h
        $(CC) $(CFLAGS) -o passenger passenger.c $(LDFLAGS)

clean:
        rm -f $(TARGETS) report.txt *.key
        ipcrm -a 2>/dev/null || true

run: all
        ./main 2 10 5 3 20

test: all
        ./main 1 5 2 2 10

.PHONY: all clean run test