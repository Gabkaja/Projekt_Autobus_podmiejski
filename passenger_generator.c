/*
 * passenger_generator.c
 * 
 * Generator pasażerów - tworzy nowe procesy pasażerów w nieskończonej pętli.
 * Odstępy między pasażerami: losowo 1-3 sekundy.
 * Kończy pracę gdy zostanie ustawiona flaga shutdown lub station_blocked.
 */

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

/* Generuje znacznik czasu HH:MM:SS */
void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

/* Zapis do logu generatora */
void log_write(const char* s) {
    int fd = open("generator.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Zapis do głównego raportu */
void log_main(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Blokada mutexa */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Odblokowanie mutexa */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/*
 * Obsługa SIGCHLD - zbiera zakończone procesy pasażerów.
 * Zapobiega powstawaniu zombie processes.
 */
void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    /* Generowanie kluczy IPC */
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');

    if (shm_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    /* Podłączenie do zasobów IPC (6 semaforów) */
    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 6, 0600);

    if (shmid == -1 || semid == -1) {
        perror("get ipc");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    /* Konfiguracja obsługi SIGCHLD */
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
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Start - tworzy pasazerow w nieskonczonosc\n", b);
    log_write(ln);
    log_main(ln);

    srand((unsigned)time(NULL));

    /*
     * GŁÓWNA PĘTLA GENERATORA
     * Tworzy nowe procesy pasażerów aż do shutdown.
     */
    for (;;) {
        /* Losowy odstęp 1-3 sekundy */
        int delay = 1 + (rand() % 3);
        sleep(delay);

        /* Sprawdzamy shutdown */
        sem_lock();
        int sd = bus->shutdown;
        int sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            break;
        }

        /*
         * Inkrementujemy licznik aktywnych pasażerów.
         * Proces pasażera zdekrementuje go po zakończeniu.
         */
        sem_lock();
        bus->active_passengers++;
        sem_unlock();

        /* Tworzenie nowego procesu pasażera */
        pid_t p = fork();
        if (p == -1) {
            perror("fork passenger");
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
        }
        else if (p == 0) {
            /* Proces potomny - uruchamiamy passenger */
            execl("./passenger", "passenger", NULL);
            perror("exec passenger");
            _exit(1);
        }
        /* Proces rodzica kontynuuje pętlę */
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Koniec pracy\n", b);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
