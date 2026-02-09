/*
 * dispatcher.c
 * 
 * Proces dyspozytora zarządzającego operacjami dworca.
 * Obsługuje sygnały i steruje pracą systemu:
 * - SIGUSR1: wymuszony odjazd autobusu
 * - SIGUSR2: blokada dworca
 * - SIGINT: shutdown systemu
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include "ipc.h"

int shmid;
struct BusState* bus;
volatile sig_atomic_t should_exit = 0;

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

/* Zapis do logu dyspozytora */
void log_write(const char* s) {
    int fd = open("dispatcher.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
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

/*
 * Obsługa SIGINT - graceful shutdown całego systemu.
 * Ustawia flagi shutdown i station_blocked.
 */
void handle_int(int sig) {
    (void)sig;
    if (bus) {
        bus->shutdown = 1;
        bus->station_blocked = 1;
        
        char b[64];
        ts(b, sizeof(b));
        char ln[128];
        snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] SIGINT - rozpoczynam shutdown systemu\n", b);
        log_write(ln);
        log_main(ln);
    }
    should_exit = 1;
}

/*
 * Obsługa SIGUSR1 - wymuszenie odjazdu autobusu.
 * Przekazuje sygnał do aktualnego kierowcy (bus->driver_pid).
 */
void handle_usr1(int sig) {
    (void)sig;
    if (bus && bus->driver_pid > 0) {
        kill(bus->driver_pid, SIGUSR1);
        char b[64];
        ts(b, sizeof(b));
        char ln[128];
        snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Wymuszenie odjazdu\n", b);
        log_write(ln);
        log_main(ln);
    }
}

/*
 * Obsługa SIGUSR2 - blokada dworca i shutdown.
 * Powiadamia kierowcę i proces główny (main).
 * To jest awaryjne zamknięcie dworca.
 */
void handle_usr2(int sig) {
    (void)sig;
    if (bus) {
        bus->station_blocked = 1;
        bus->shutdown = 1;
        if (bus->driver_pid > 0) {
            kill(bus->driver_pid, SIGUSR2);
        }
        
        /* Powiadomienie procesu głównego */
        kill(getppid(), SIGUSR2);
        
        char b[64];
        ts(b, sizeof(b));
        char ln[128];
        snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Blokada dworca\n", b);
        log_write(ln);
        log_main(ln);
    }
    should_exit = 1;
}

int main() {
    /* Podłączenie do pamięci dzielonej */
    key_t shm_key = ftok(SHM_PATH, 'S');
    if (shm_key == -1) {
        perror("ftok shm");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    if (shmid == -1) {
        perror("shmget");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Start pracy\n", b);
    log_write(ln);
    log_main(ln);

    /* Konfiguracja obsługi SIGINT */
    struct sigaction sai;
    memset(&sai, 0, sizeof(sai));
    sai.sa_handler = handle_int;
    sigemptyset(&sai.sa_mask);
    sai.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sai, NULL);

    /* Konfiguracja obsługi SIGUSR1 */
    struct sigaction sa1;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = handle_usr1;
    sigemptyset(&sa1.sa_mask);
    sa1.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa1, NULL);

    /* Konfiguracja obsługi SIGUSR2 */
    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = handle_usr2;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa2, NULL);

    /*
     * Główna pętla - czeka na sygnały.
     * pause() minimalizuje zużycie CPU.
     */
    while (!should_exit) {
        pause();
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Koniec pracy\n", b);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
