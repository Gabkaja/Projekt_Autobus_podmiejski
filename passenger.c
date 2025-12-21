#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <time.h>
#include "ipc.h"

void sem_lock(int semid) {
    struct sembuf sb = { 0, -1, 0 };
    semop(semid, &sb, 1);
}

void sem_unlock(int semid) {
    struct sembuf sb = { 0, 1, 0 };
    semop(semid, &sb, 1);
}

int main() {
    srand(getpid());
    int bike = rand() % 2;
    int vip = (rand() % 100 == 0);

    int shmid = shmget(SHM_KEY, sizeof(struct BusState), 0);
    int semid = semget(SEM_KEY, 1, 0);
    int msgid = msgget(MSG_KEY, 0);

    struct BusState* bus = shmat(shmid, NULL, 0);
    struct msg m;

    sem_lock(semid);
    if (bus->blocked) {
        printf("[PASAZER %d] Dworzec zablokowany\n", getpid());
        bus->active_passengers--;
        sem_unlock(semid);
        return 0;
    }
    sem_unlock(semid);

    if (!vip) {
        m.type = 1;
        m.pid = getpid();
        msgsnd(msgid, &m, sizeof(pid_t), 0);
        msgrcv(msgid, &m, sizeof(pid_t), getpid(), 0);
    }

    sem_lock(semid);
    if (bus->passengers >= MAX_P || (bike && bus->bikes >= MAX_R)) {
        printf("[PASAZER %d] Brak miejsca\n", getpid());
        bus->active_passengers--;
        sem_unlock(semid);
        return 0;
    }

    bus->passengers++;
    if (bike) bus->bikes++;
    printf("[PASAZER %d] Wszedl do autobusu\n", getpid());
    bus->active_passengers--;
    sem_unlock(semid);

    return 0;
}
