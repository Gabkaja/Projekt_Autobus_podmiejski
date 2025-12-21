#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include "ipc.h"

int shmid, semid;
struct BusState* bus;

void sem_lock() {
    struct sembuf sb = { 0, -1, 0 };
    semop(semid, &sb, 1);
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, 0 };
    semop(semid, &sb, 1);
}

void force_departure(int sig) {
    printf("[KIEROWCA] Wymuszony odjazd\n");
}

void block_station(int sig) {
    sem_lock();
    bus->blocked = 1;
    sem_unlock();
    printf("[KIEROWCA] Dworzec zablokowany\n");
}

int main() {
    signal(SIGUSR1, force_departure);
    signal(SIGUSR2, block_station);

    shmid = shmget(SHM_KEY, sizeof(struct BusState), 0);
    semid = semget(SEM_KEY, 1, 0);
    bus = shmat(shmid, NULL, 0);

    while (1) {
        sleep(5);

        sem_lock();
        if (bus->active_passengers == 0 && bus->passengers == 0) {
            sem_unlock();
            printf("[KIEROWCA] Koniec pracy\n");
            break;
        }

        printf("[KIEROWCA] Odjazd: %d pasazerow, %d rowerow\n",
            bus->passengers, bus->bikes);

        bus->passengers = 0;
        bus->bikes = 0;
        sem_unlock();

        sleep(3);
    }

    return 0;
}
