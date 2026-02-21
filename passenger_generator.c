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

void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;
    
    // Zbieramy wszystkie zakończone procesy
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // Zwolnij slot w semaforze generator_limit (sem[5])
        struct sembuf sb = { 5, 1, 0 };  // Bez SEM_UNDO!
        semop(semid, &sb, 1);
    }
    errno = saved_errno;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');

    if (shm_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 7, 0600);  // 7 semaforów

    if (shmid == -1 || semid == -1) {
        perror("get ipc");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    // Ustawiamy handler SIGCHLD
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
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Start\n", b);
    log_write(ln);
    log_main(ln);

    srand((unsigned)time(NULL));

    for (int i=0;i<5000;i++) {
		// Losowy odstęp 1-3 sekundy
        int delay = 1 + (rand() % 3);
        //sleep(delay);
        // Sprawdź shutdown
        sem_lock();
        int sd = bus->shutdown;
        int sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            break;
        }

        // BLOKUJ na semaforze 5 - czeka aż będzie wolny slot (max 100 pasażerów)
        // Gdy pasażer zakończy pracę -> SIGCHLD -> semop(5, +1) -> generator budzi się i tworzy nowego
        struct sembuf sb_wait = { 5, -1, 0 };  // Bez SEM_UNDO!
        if (semop(semid, &sb_wait, 1) == -1) {
            if (errno == EIDRM || errno == EINVAL) {
                // Semafory usunięte - kończymy
                break;
            }
            if (errno == EINTR) {
                // Przerwane przez sygnał - sprawdź shutdown i próbuj ponownie
                continue;
            }
            perror("semop generator_limit");
            break;
        }

        // Mamy slot - sprawdź ponownie shutdown przed forkiem
        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            // Zwolnij slot i zakończ
            struct sembuf sb_rel = { 5, 1, 0 };
            semop(semid, &sb_rel, 1);
            break;
        }

        // ========== INKREMENTACJA LICZNIKA: GENERATOR UTWORZYŁ ==========
        sem_lock();
        bus->generator_created++;
        sem_unlock();

        // Tworzymy pasażera
        pid_t p = fork();
        if (p == -1) {
            perror("fork passenger");
            // Fork się nie udał - zwolnij slot i cofnij licznik
            struct sembuf sb_rel = { 5, 1, 0 };
            semop(semid, &sb_rel, 1);
            sem_lock();
            bus->generator_created--;
            sem_unlock();
            continue;
        }
        else if (p == 0) {
            // Proces dziecka - exec passenger
            execl("./passenger", "passenger", NULL);
            perror("exec passenger");
            _exit(1);
        }
        
        // Rodzic - pasażer utworzony, natychmiast wracamy do pętli!
        // Gdy tylko pasażer zakończy pracę -> SIGCHLD zwolni slot -> tworzymy nowego
    }

    // KLUCZOWE: czekamy na zakończenie WSZYSTKICH procesów pasażerów
    // zanim generator wyjdzie. Main czeka tylko na bezpośrednie dzieci
    // (wait() nie widzi wnuków), więc gdyby generator wyszedł przed
    // pasażerami, main odczytałby statystyki zanim pasażerowie skończyli
    // aktualizować liczniki w shared memory → rozbieżność w statystykach.
    //
    // Wyłączamy handler SIGCHLD (już niepotrzebny - nie tworzymy nowych
    // pasażerów) i czekamy blokująco na wszystkich pozostałych.
    signal(SIGCHLD, SIG_DFL);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR);

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Koniec pracy (wszyscy pasazerowie zakonczeni)\n", b);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
