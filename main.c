#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <time.h>
#include "ipc.h"

int main() {
    srand(time(NULL));

    int shmid = shmget(SHM_KEY, sizeof(struct BusState), IPC_CREAT | 0660);
    int semid = semget(SEM_KEY, 1, IPC_CREAT | 0660);
    int msgid = msgget(MSG_KEY, IPC_CREAT | 0660);

    struct BusState* bus = shmat(shmid, NULL, 0);
    bus->passengers = 0;
    bus->bikes = 0;
    bus->blocked = 0;
    bus->active_passengers = TOTAL_PASSENGERS;

    semctl(semid, 0, SETVAL, 1); // mutex

    pid_t dpid = fork();
    if (dpid == 0) execl("./driver", "driver", NULL);
    bus->driver_pid = dpid;

    if (fork() == 0) execl("./cashier", "cashier", NULL);
    if (fork() == 0) execl("./dispatcher", "dispatcher", NULL);

    for (int i = 0; i < TOTAL_PASSENGERS; i++) {
        usleep((rand() % 1000 + 200) * 1000);
        if (fork() == 0) execl("./passenger", "passenger", NULL);
    }

    while (wait(NULL) > 0);

    shmctl(shmid, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);
    msgctl(msgid, IPC_RMID, NULL);

    return 0;
}
