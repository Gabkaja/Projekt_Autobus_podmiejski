#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include "ipc.h"

int main() {
    int shmid = shmget(SHM_KEY, sizeof(struct BusState), 0);
    struct BusState* bus = shmat(shmid, NULL, 0);

    sleep(5);
    kill(bus->driver_pid, SIGUSR1);

    sleep(5);
    kill(bus->driver_pid, SIGUSR2);

    return 0;
}
