/*
 * passenger_generator.c – Proces generatora pasażerów.
 *
 * Generator cyklicznie tworzy nowe procesy pasażerów przez fork + exec.
 * Liczba jednocześnie żywych procesów pasażerów jest ograniczona przez
 * semafor sem[4] (początkowo MAX_PASSENGERS). Generator blokuje się na tym
 * semaforze, gdy limit zostanie osiągnięty, i wznawia pracę dopiero gdy
 * pasażer zakończy działanie i SIGCHLD uwolni jeden slot.
 *
 * Przepływ:
 *   1. Oczekiwanie na wolny slot przez semop(sem[4], -1).
 *   2. Weryfikacja flag shutdown/station_blocked.
 *   3. Inkrementacja bus->generator_created i tworzenie procesu pasażera.
 *   4. Natychmiastowy powrót do kroku 1 (bez oczekiwania na ukończenie pasażera).
 *
 * SIGCHLD: handler zbiera zakończone procesy pasażerów i zwalnia slot w sem[4].
 *
 * Zakończenie:
 *   Po przerwaniu pętli generator oczekuje blokująco na wszystkich pasażerów
 *   (waitpid w trybie blokującym). Jest to konieczne, ponieważ main() czeka
 *   tylko na bezpośrednie procesy potomne – generator jest rodzicem pasażerów,
 *   więc to on musi je zebrać przed własnym wyjściem. Gdyby generator wyszedł
 *   wcześniej, pasażerowie mogliby nie zdążyć zaktualizować liczników w pamięci
 *   dzielonej przed odczytem statystyk przez main.
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

/* Globalne identyfikatory IPC */
int shmid, semid;
struct BusState* bus;

/* =========================================================
 * Funkcje pomocnicze: timestamp i logowanie
 * ========================================================= */

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
    int fd = open("generator.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

void log_main(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* =========================================================
 * Operacje na semaforze mutex (sem[0])
 * ========================================================= */

void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR)              continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* =========================================================
 * Handler SIGCHLD
 * ========================================================= */

/*
 * handle_sigchld – Zbiera zakończone procesy pasażerów i zwalnia sloty.
 * Dla każdego zebranego procesu podnosi sem[4] o 1, co pozwala generatorowi
 * odblokować się na semop(-1) i uruchomić kolejnego pasażera.
 * Brak SEM_UNDO jest zamierzony: token sem[4] musi pozostać po zakończeniu
 * handlera, tak aby generatorowa semop(-1) mogła go skonsumować.
 */
void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct sembuf sb = { 4, 1, 0 };
        semop(semid, &sb, 1);
    }
    errno = saved_errno;
}

/* =========================================================
 * Funkcja główna generatora
 * ========================================================= */

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    /* Generowanie kluczy i podłączenie do istniejących zasobów IPC */
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');

    if (shm_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

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

    /* Rejestracja handlera SIGCHLD – musi być przed pierwszym forkiem */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1)
        perror("sigaction SIGCHLD");

    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Start\n", b);
    log_write(ln);
    log_main(ln);

    srand((unsigned)time(NULL));

    /* =========================================================
     * Pętla generowania pasażerów (do 5000 łącznie)
     * ========================================================= */
    for (int i = 0; i < 5000; i++) {
        /* Wstępne sprawdzenie flag przed blokowaniem się na semaforze */
        sem_lock();
        int sd = bus->shutdown;
        int sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) break;

        /*
         * Oczekiwanie na wolny slot pasażera (sem[4]).
         * Generator blokuje się tutaj, gdy w systemie jest już MAX_PASSENGERS
         * aktywnych pasażerów. Gdy jeden z nich zakończy pracę, handler SIGCHLD
         * wywoła semop(+1) i generator się odblokuje.
         */
        struct sembuf sb_wait = { 4, -1, 0 };
        if (semop(semid, &sb_wait, 1) == -1) {
            if (errno == EIDRM || errno == EINVAL) break;  /* IPC usunięte */
            if (errno == EINTR) { i--; continue; }         /* Przerwanie przez sygnał – ponów */
            perror("semop generator_limit");
            break;
        }

        /* Ponowna weryfikacja po odlokowaniu – stan mógł się zmienić w czasie oczekiwania */
        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            /* Zwolnij slot, który właśnie wzięliśmy, i zakończ */
            struct sembuf sb_rel = { 4, 1, 0 };
            semop(semid, &sb_rel, 1);
            break;
        }

        /* Inkrementacja licznika – rejestrujemy zamiar stworzenia pasażera
         * przed forkiem, aby uniknąć luki statystycznej przy ewentualnym błędzie fork. */
        sem_lock();
        bus->generator_created++;
        sem_unlock();

        /* Tworzenie procesu pasażera */
        pid_t p = fork();
        if (p == -1) {
            perror("fork passenger");
            /* Wycofanie licznika i zwolnienie slotu przy błędzie fork */
            struct sembuf sb_rel = { 4, 1, 0 };
            semop(semid, &sb_rel, 1);
            sem_lock();
            bus->generator_created--;
            sem_unlock();
            continue;
        }
        else if (p == 0) {
            /* Proces dziecka: zastąp się obrazem pasażera */
            execl("./passenger", "passenger", NULL);
            perror("exec passenger");
            _exit(1);
        }

        /* Proces rodzica: natychmiast wraca do pętli i czeka na kolejny slot.
         * Slot zostanie zwolniony przez SIGCHLD gdy ten pasażer zakończy pracę. */
    }

    /*
     * Oczekiwanie na zakończenie wszystkich procesów pasażerów.
     * Wyłączamy handler SIGCHLD (SIG_DFL) i czekamy blokująco.
     * Jest to konieczne, bo main() zbiera tylko bezpośrednie procesy potomne
     * (kierowcy, kasjer, dyspozytor, generator), natomiast procesami-wnukami
     * (pasażerami) zajmuje się właśnie generator. Gdyby generator zakończył się
     * przed pasażerami, main mógłby odczytać statystyki z pamięci dzielonej
     * zanim pasażerowie zdążą je zaktualizować.
     */
    signal(SIGCHLD, SIG_DFL);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR);

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [GENERATOR] Koniec pracy (wszyscy pasazerowie zakonczeni)\n", b);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
