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

/* Formatuje aktualny czas jako HH:MM:SS. */
void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

/* Dopisuje do prywatnego logu generatora. */
void log_write(const char* s) {
    int fd = open("generator.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Dopisuje do wspólnego raportu symulacji. */
void log_main(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Zajmuje mutex sem[0]. */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

/* Zwalnia mutex sem[0]. */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Handler SIGCHLD – wywoływany gdy kończy się dowolny proces pasażera.
 *
 * Zbieramy statusy wszystkich zakończonych dzieci przez WNOHANG (nieblokująco),
 * żeby nie zostawiać zombie-procesów. Dla każdego zakończonego pasażera
 * podnosimy sem[5] o 1 – zwalniamy slot w liczniku aktywnych pasażerów.
 * To bezpośrednio budzi generator w pętli głównej, który czeka na semop(sem[5], -1)
 * i może teraz stworzyć nowego pasażera. Mechanizm ten zastępuje sleep() –
 * nowy pasażer pojawia się dokładnie wtedy gdy poprzedni skończy pracę. */
void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct sembuf sb = { 5, 1, 0 }; /* bez SEM_UNDO – celowo, patrz niżej */
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
    semid = semget(sem_key, 7, 0600);

    if (shmid == -1 || semid == -1) {
        perror("get ipc");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    /* SA_NOCLDSTOP = handler nie jest wołany przy SIGSTOP procesu potomnego,
     * tylko przy jego zakończeniu. SA_RESTART = przerwane semop wraca automatycznie. */
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

    /* Pętla tworzy do 5000 pasażerów przez cały czas działania systemu.
     * Nie tworzy ich wszystkich od razu – maksymalna liczba jednocześnie
     * żyjących procesów pasażerów jest ograniczona przez sem[5] do MAX_PASSENGERS.
     * Gdy pasażer kończy pracę, SIGCHLD uwalnia slot i pętla tworzy kolejnego. */
    for (int i = 0; i < 5000; i++) {
        int delay = 1 + (rand() % 3);
        /* Sprawdź shutdown przed blokującym semop – nie ma sensu czekać
         * na slot jeśli system właśnie się wyłącza. */
        sem_lock();
        int sd = bus->shutdown;
        int sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            break;
        }

        /* Blokujące P(sem[5]) – czekamy aż będzie wolny slot.
         * sem[5] startuje z wartością MAX_PASSENGERS i maleje o 1 przy każdym
         * forku. Gdy pasażer kończy pracę, SIGCHLD podnosi sem[5] z powrotem.
         * Bez SEM_UNDO – jeśli generator padnie przy aktywnych pasażerach,
         * sloty zostaną zablokowane, ale to i tak koniec symulacji. */
        struct sembuf sb_wait = { 5, -1, 0 };
        if (semop(semid, &sb_wait, 1) == -1) {
            if (errno == EIDRM || errno == EINVAL) {
                break; /* semafory usunięte – kończymy */
            }
            if (errno == EINTR) {
                /* Przerwane przez sygnał (np. SIGCHLD który zwolnił slot) –
                 * sprawdzamy shutdown i próbujemy ponownie. */
                continue;
            }
            perror("semop generator_limit");
            break;
        }

        /* Ponowne sprawdzenie po wyjściu z semop – shutdown mógł nadejść
         * gdy czekaliśmy na slot. Jeśli tak, zwalniamy slot i kończymy. */
        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            struct sembuf sb_rel = { 5, 1, 0 };
            semop(semid, &sb_rel, 1);
            break;
        }

        /* Inkrementujemy licznik przed forkiem, a nie po – jeśli fork się nie
         * uda to cofniemy go, ale nie grozi nam że pasażer skończy się zanim
         * zdążymy zliczyć, bo to on sam zmniejsza sem[5] przez SIGCHLD. */
        sem_lock();
        bus->generator_created++;
        sem_unlock();

        pid_t p = fork();
        if (p == -1) {
            perror("fork passenger");
            struct sembuf sb_rel = { 5, 1, 0 };
            semop(semid, &sb_rel, 1);
            sem_lock();
            bus->generator_created--;
            sem_unlock();
            continue;
        }
        else if (p == 0) {
            execl("./passenger", "passenger", NULL);
            perror("exec passenger");
            _exit(1);
        }
        
        /* Rodzic wraca natychmiast do pętli – nie śpi między pasażerami.
         * Tempo tworzenia pasażerów jest regulowane przez sem[5] i SIGCHLD,
         * a nie przez arbitralne sleep(). */
    }

    /* Wyłączamy handler SIGCHLD (nie będziemy już tworzyć nowych pasażerów)
     * i czekamy blokująco na zakończenie wszystkich żyjących procesów pasażerów.
     * Jest to kluczowe: main czeka tylko na bezpośrednie dzieci przez wait().
     * Pasażerowie są wnukami (dziećmi generatora), więc gdyby generator wyszedł
     * przed nimi, main odczytałby statystyki z shared memory zanim pasażerowie
     * zdążyli je zaktualizować – skutkowałoby to rozbieżnością w raporcie. */
    signal(SIGCHLD, SIG_DFL);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR);

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Koniec pracy (wszyscy pasazerowie zakonczeni)\n", b);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
