#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "ipc.h"

int shmid, semid;
struct BusState* bus;
volatile sig_atomic_t force_flag = 0;

void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        perror("localtime");
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

void log_write(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) {
        perror("open report");
        return;
    }
    if (write(fd, s, strlen(s)) == -1) {
        perror("write report");
    }
    if (close(fd) == -1) {
        perror("close report");
    }
}

void sem_lock() {
    struct sembuf sb = { 0, -1, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop lock");
    }
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop unlock");
    }
}

void gate_lock(int gate) {
    struct sembuf sb = { gate, -1, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop gate_lock");
    }
}

void gate_unlock(int gate) {
    struct sembuf sb = { gate, 1, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop gate_unlock");
    }
}

void handle_usr1(int sig) {
    (void)sig;
    force_flag = 1;
}

void handle_usr2(int sig) {
    (void)sig;
    sem_lock();
    bus->station_blocked = 1;
    sem_unlock();
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Blokada dworca\n", b, getpid());
    log_write(ln);
}

void handle_int(int sig) {
    (void)sig;
    sem_lock();
    bus->shutdown = 1;
    bus->station_blocked = 1;
    sem_unlock();
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] SIGINT\n", b, getpid());
    log_write(ln);
}

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S');
    if (shm_key == -1) {
        perror("ftok shm");
        return 1;
    }

    key_t sem_key = ftok(SEM_PATH, 'E');
    if (sem_key == -1) {
        perror("ftok sem");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    if (shmid == -1) {
        perror("shmget");
        return 1;
    }

    semid = semget(sem_key, 4, 0600);
    if (semid == -1) {
        perror("semget");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    struct sigaction sa1;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = handle_usr1;
    sigemptyset(&sa1.sa_mask);
    sa1.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &sa1, NULL) == -1) {
        perror("sigaction SIGUSR1");
    }

    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = handle_usr2;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR2, &sa2, NULL) == -1) {
        perror("sigaction SIGUSR2");
    }

    struct sigaction sai;
    memset(&sai, 0, sizeof(sai));
    sai.sa_handler = handle_int;
    sigemptyset(&sai.sa_mask);
    sai.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sai, NULL) == -1) {
        perror("sigaction SIGINT");
    }

    srand((unsigned)(getpid() ^ time(NULL)));

    for (;;) {
        gate_lock(3);

        sem_lock();
        bus->driver_pid = getpid();
        bus->departing = 0;
        int sb = bus->station_blocked;
        int sd = bus->shutdown;
        int wait_time = bus->T;
        sem_unlock();

        if (sd || sb) {
            gate_unlock(3);
            break;
        }

        char b[64];
        ts(b, sizeof(b));
        char ln[128];
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Autobus na dworcu\n", b, getpid());
        log_write(ln);

        int waited = 0;
        while (!force_flag && waited < wait_time) {
            sleep(1);
            waited++;

            sem_lock();
            sd = bus->shutdown;
            sb = bus->station_blocked;
            sem_unlock();

            if (sd || sb) break;
        }

        if (sd || sb) {
            gate_unlock(3);
            break;
        }

        force_flag = 0;

        gate_lock(1);
        gate_lock(2);

        sem_lock();
        bus->departing = 1;
        int p = bus->passengers;
        int r = bus->bikes;
        sem_unlock();

        char b2[64];
        ts(b2, sizeof(b2));
        char ln2[160];
        snprintf(ln2, sizeof(ln2), "[%s] [KIEROWCA %d] Odjazd: %d pasazerow, %d rowerow\n", b2, getpid(), p, r);
        log_write(ln2);

        sem_lock();
        bus->passengers = 0;
        bus->bikes = 0;
        sem_unlock();

        gate_unlock(1);
        gate_unlock(2);
        gate_unlock(3);

        int Ti = (rand() % 7) + 3;
        sleep(Ti);

        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) break;

        char b3[64];
        ts(b3, sizeof(b3));
        char ln3[128];
        snprintf(ln3, sizeof(ln3), "[%s] [KIEROWCA %d] Powrot po %ds\n", b3, getpid(), Ti);
        log_write(ln3);
    }

    char b4[64];
    ts(b4, sizeof(b4));
    char ln4[128];
    snprintf(ln4, sizeof(ln4), "[%s] [KIEROWCA %d] Koniec pracy\n", b4, getpid());
    log_write(ln4);

    if (shmdt(bus) == -1) {
        perror("shmdt");
    }

    return 0;
}cat 