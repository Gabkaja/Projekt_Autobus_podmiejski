#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include "ipc.h"

int shmid, semid;
struct BusState* bus;

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

void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Uzycie: %s TOTAL\n", argv[0]);
        return 1;
    }

    int TOTAL = atoi(argv[1]);
    if (TOTAL <= 0) {
        fprintf(stderr, "Niepoprawna liczba pasazerow\n");
        return 1;
    }

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
        struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("sigaction SIGCHLD");
    }
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Start - utworzy %d pasazerow\n", b, TOTAL);
    log_write(ln);

    srand((unsigned)time(NULL));

    for (int i = 0; i < TOTAL; i++) {
        // Losowy odstęp 1-3 sekundy
        int delay = 1 + (rand() % 3);
        sleep(delay);

        // Sprawdź czy system się nie wyłącza
        sem_lock();
        int sd = bus->shutdown;
        int sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            break;
        }

        // Inkrementuj licznik aktywnych pasażerów
        sem_lock();
        bus->active_passengers++;
        sem_unlock();

        pid_t p = fork();
        if (p == -1) {
            perror("fork passenger");
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
        }
        else if (p == 0) {
            execl("./passenger", "passenger", NULL);
            perror("exec passenger");
            _exit(1);
        }

    }
    while (wait(NULL) > 0) {
        if (errno == ECHILD) {
            break;
        }
    }
    // Oznaczamy że generator skończył pracę
    sem_lock();
    bus->generator_done = 1;
    sem_unlock();

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Koniec - utworzono wszystkich pasazerow\n", b);
    log_write(ln);

    shmdt(bus);
    return 0;
}