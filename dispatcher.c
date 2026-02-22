/*
 * dispatcher.c – Proces dyspozytora.
 *
 * Dyspozytor jest pośrednikiem między operatorem (sygnałami zewnętrznymi)
 * a kierowcami autobusów. Nie wykonuje żadnej aktywnej pracy – czeka na
 * sygnały przez pause() i reaguje zgodnie z ich znaczeniem.
 *
 * Obsługiwane sygnały:
 *   SIGINT  – Inicjuje zamknięcie systemu: ustawia flagi shutdown i station_blocked
 *             w pamięci dzielonej, a następnie kończy własną pętlę.
 *   SIGUSR1 – Wymusza natychmiastowy odjazd autobusu: przekazuje SIGUSR1 do
 *             aktualnego kierowcy (bus->driver_pid).
 *   SIGUSR2 – Blokada dworca: ustawia flagi zamknięcia, wysyła SIGUSR2 do kierowcy
 *             i do procesu main (getppid()), a następnie kończy pracę.
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

/* Globalne identyfikatory IPC i wskaźnik do pamięci dzielonej */
int shmid;
struct BusState* bus;

/* Flaga ustawiana przez handlery sygnałów, by wyjść z pętli głównej */
volatile sig_atomic_t should_exit = 0;

/* =========================================================
 * Funkcje pomocnicze: timestamp i logowanie
 * ========================================================= */

/*
 * ts – Formatuje bieżącą godzinę do bufora w formacie HH:MM:SS.
 */
void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

/*
 * log_write – Dopisuje wpis do dziennika dyspozytora (dispatcher.log).
 */
void log_write(const char* s) {
    int fd = open("dispatcher.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/*
 * log_main – Dopisuje wpis do raportu zbiorczego (report.txt).
 */
void log_main(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* =========================================================
 * Handlery sygnałów
 * ========================================================= */

/*
 * handle_int – Handler SIGINT.
 * Ustawia flagi shutdown i station_blocked w pamięci dzielonej,
 * co powoduje, że kierowcy i pasażerowie zaczną kończyć pracę.
 * Ustawia should_exit, by wyjść z pętli pause().
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
 * handle_usr1 – Handler SIGUSR1.
 * Przekazuje SIGUSR1 bezpośrednio do kierowcy stojącego na dworcu,
 * wymuszając natychmiastowy odjazd bez oczekiwania na upłynięcie czasu T.
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
 * handle_usr2 – Handler SIGUSR2 (blokada dworca).
 * Ustawia flagi zamknięcia w pamięci dzielonej, przekazuje SIGUSR2
 * do kierowcy (by ten nie przyjmował nowych pasażerów) i do procesu
 * main (by ten zainicjował właściwy shutdown). Kończy własną pętlę.
 */
void handle_usr2(int sig) {
    (void)sig;
    if (bus) {
        bus->station_blocked = 1;
        bus->shutdown = 1;

        if (bus->driver_pid > 0)
            kill(bus->driver_pid, SIGUSR2);

        /* Powiadomienie main o blokadzie – main obsługuje SIGUSR2 tak samo jak SIGINT */
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

/* =========================================================
 * Funkcja główna dyspozytora
 * ========================================================= */

int main() {
    /* Generowanie klucza i podłączenie do pamięci dzielonej */
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

    /* Rejestracja handlerów sygnałów z SA_RESTART, aby pause() nie przerywało
     * się przypadkowo przy sygnałach nieobsługiwanych (np. SIGCHLD z tłem). */
    struct sigaction sai;
    memset(&sai, 0, sizeof(sai));
    sai.sa_handler = handle_int;
    sigemptyset(&sai.sa_mask);
    sai.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sai, NULL);

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

    /* Pętla główna: dyspozytor nie wykonuje aktywnej pracy – usypia
     * się przez pause() i jest budzony wyłącznie przez sygnały. */
    while (!should_exit)
        pause();

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Koniec pracy\n", b);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
