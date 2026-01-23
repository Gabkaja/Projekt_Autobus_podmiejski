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
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

void log_write(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

void sem_lock() {
    struct sembuf sb = { 0, -1, 0 };
    semop(semid, &sb, 1);
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, 0 };
    semop(semid, &sb, 1);
}

void gate_lock(int gate) {
    struct sembuf sb = { gate, -1, 0 };
    semop(semid, &sb, 1);
}

void gate_unlock(int gate) {
    struct sembuf sb = { gate, 1, 0 };
    semop(semid, &sb, 1);
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
}

void handle_int(int sig) {
    (void)sig;
    sem_lock();
    bus->shutdown = 1;
    bus->station_blocked = 1;
    sem_unlock();
}

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');

    if (shm_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 4, 0600);

    if (shmid == -1 || semid == -1) {
        perror("get ipc");
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
    sigaction(SIGUSR1, &sa1, NULL);

    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = handle_usr2;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa2, NULL);

    struct sigaction sai;
    memset(&sai, 0, sizeof(sai));
    sai.sa_handler = handle_int;
    sigemptyset(&sai.sa_mask);
    sai.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sai, NULL);

    srand((unsigned)(getpid() ^ time(NULL)));

    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Start pracy\n", b, getpid());
    log_write(ln);

    for (;;) {
        // Tylko jeden autobus na dworcu - gate[3]
        gate_lock(3);

        sem_lock();
        bus->driver_pid = getpid();
        bus->departing = 0;
        int sb = bus->station_blocked;
        int sd = bus->shutdown;
        int wait_time = bus->T;
        int boarded = bus->boarded_passengers;
        int total = bus->total_passengers;
        int cashier_done = bus->cashier_done;
        int gen_done = bus->generator_done;
        int active = bus->active_passengers;
        sem_unlock();

        // Warunki zakończenia pracy
        if (sd || sb) {
            gate_unlock(3);
            break;
        }

        // Auto-shutdown: generator i kasa skończyli, wszyscy pasażerowie obsłużeni
        if (gen_done && cashier_done && active == 0 && boarded >= total) {
            gate_unlock(3);
            break;
        }

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Autobus na dworcu\n", b, getpid());
        log_write(ln);

        // Czekamy T sekund lub na sygnał od dyspozytora
        int waited = 0;
        while (!force_flag && waited < wait_time) {
            sleep(1);
            waited++;

            sem_lock();
            sd = bus->shutdown;
            sb = bus->station_blocked;
            gen_done = bus->generator_done;
            cashier_done = bus->cashier_done;
            active = bus->active_passengers;
            boarded = bus->boarded_passengers;
            sem_unlock();

            if (sd || sb) break;
            if (gen_done && cashier_done && active == 0 && boarded >= total) break;
        }

        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        gen_done = bus->generator_done;
        cashier_done = bus->cashier_done;
        active = bus->active_passengers;
        boarded = bus->boarded_passengers;
        sem_unlock();

        if (sd || sb) {
            gate_unlock(3);
            break;
        }

        if (gen_done && cashier_done && active == 0 && boarded >= total) {
            gate_unlock(3);
            break;
        }

        force_flag = 0;

        // Blokujemy wejścia
        gate_lock(1);
        gate_lock(2);

        sem_lock();
        bus->departing = 1;
        int p = bus->passengers;
        int r = bus->bikes;
        bus->boarded_passengers += p;
        sem_unlock();

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Odjazd: %d pasazerow, %d rowerow\n", 
                 b, getpid(), p, r);
        log_write(ln);

        // Reset liczników
        sem_lock();
        bus->passengers = 0;
        bus->bikes = 0;
        sem_unlock();

        // Odblokowujemy wejścia i dworzec
        gate_unlock(1);
        gate_unlock(2);
        gate_unlock(3);

        // Jazda (losowy czas 3-9s)
        int Ti = (rand() % 7) + 3;
        sleep(Ti);

        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) break;

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Powrot po %ds\n", b, getpid(), Ti);
        log_write(ln);
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Koniec pracy\n", b, getpid());
    log_write(ln);

    shmdt(bus);
    return 0;
}