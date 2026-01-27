#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include "ipc.h"

int shmid, semid, msgid;
struct BusState* bus;
pid_t dispatcher_pid = 0;

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

void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

void cleanup() {
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl IPC_RMID");
    }
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("semctl IPC_RMID");
    }
    if (msgctl(msgid, IPC_RMID, NULL) == -1) {
        perror("msgctl IPC_RMID");
    }
    unlink(SHM_PATH);
    unlink(SEM_PATH);
    unlink(MSG_PATH);
}

void handle_sigint(int sig) {
    (void)sig;
    if (bus) {
        bus->shutdown = 1;
        bus->station_blocked = 1;
    }
    
    if (dispatcher_pid > 0) {
        kill(dispatcher_pid, SIGINT);
    }
    
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [MAIN] Shutdown initiated\n", b);
    log_write(ln);
}

void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "Uzycie: %s N P R T\n", argv[0]);
        fprintf(stderr, "  N - liczba autobusow\n");
        fprintf(stderr, "  P - maksymalna liczba pasazerow w autobusie\n");
        fprintf(stderr, "  R - maksymalna liczba rowerow w autobusie\n");
        fprintf(stderr, "  T - czas oczekiwania na dworcu (sekundy)\n");
        return EXIT_FAILURE;
    }

    int N = atoi(argv[1]);
    int P = atoi(argv[2]);
    int R = atoi(argv[3]);
    int T = atoi(argv[4]);

    if (N <= 0 || P <= 0 || R < 0 || T <= 0) {
        fprintf(stderr, "Niepoprawne parametry\n");
        return EXIT_FAILURE;
    }

    // Tworzenie pliku raportu
    int fdrep = creat("report.txt", 0600);
    if (fdrep == -1) {
        perror("creat report");
        return EXIT_FAILURE;
    }
    close(fdrep);

    // Tworzenie plików kluczy
    creat(SHM_PATH, 0600);
    creat(SEM_PATH, 0600);
    creat(MSG_PATH, 0600);

    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1) {
        perror("ftok");
        cleanup();
        return EXIT_FAILURE;
    }

    // Tworzenie pamięci dzielonej
    shmid = shmget(shm_key, sizeof(struct BusState), IPC_CREAT | 0600);
    if (shmid == -1) {
        perror("shmget");
        cleanup();
        return EXIT_FAILURE;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        cleanup();
        return EXIT_FAILURE;
    }

    // Tworzenie semaforów: 0-mutex, 1-gate bez roweru, 2-gate z rowerem, 3-dworzec
    semid = semget(sem_key, 4, IPC_CREAT | 0600);
    if (semid == -1) {
        perror("semget");
        cleanup();
        return EXIT_FAILURE;
    }

    semctl(semid, 0, SETVAL, 1); // mutex
    semctl(semid, 1, SETVAL, 1); // gate bez roweru
    semctl(semid, 2, SETVAL, 1); // gate z rowerem
    semctl(semid, 3, SETVAL, 1); // dworzec (tylko jeden autobus)

    // Tworzenie kolejki komunikatów
    msgid = msgget(msg_key, IPC_CREAT | 0600);
    if (msgid == -1) {
        perror("msgget");
        cleanup();
        return EXIT_FAILURE;
    }

    // Inicjalizacja stanu systemu
    bus->P = P;
    bus->R = R;
    bus->T = T;
    bus->N = N;
    bus->passengers = 0;
    bus->bikes = 0;
    bus->departing = 0;
    bus->station_blocked = 0;
    bus->active_passengers = 0;
    bus->boarded_passengers = 0;
    bus->driver_pid = 0;
    bus->shutdown = 0;

    // Obsługa sygnałów
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    char b[64];
    ts(b, sizeof(b));
    char ln[256];
    snprintf(ln, sizeof(ln), "[%s] [MAIN] Start systemu: N=%d P=%d R=%d T=%d\n", 
             b, N, P, R, T);
    log_write(ln);

    // Tworzenie kierowców (N autobusów)
    for (int i = 0; i < N; i++) {
        pid_t p = fork();
        if (p == -1) {
            perror("fork driver");
        }
        else if (p == 0) {
            execl("./driver", "driver", NULL);
            perror("exec driver");
            _exit(1);
        }
    }

    // Tworzenie kasjera
    pid_t p1 = fork();
    if (p1 == -1) {
        perror("fork cashier");
    }
    else if (p1 == 0) {
        execl("./cashier", "cashier", NULL);
        perror("exec cashier");
        _exit(1);
    }

    // Tworzenie dyspozytora
    pid_t p2 = fork();
    if (p2 == -1) {
        perror("fork dispatcher");
    }
    else if (p2 == 0) {
        execl("./dispatcher", "dispatcher", NULL);
        perror("exec dispatcher");
        _exit(1);
    }
    dispatcher_pid = p2;

    // Tworzenie generatora pasażerów
    pid_t p3 = fork();
    if (p3 == -1) {
        perror("fork generator");
    }
    else if (p3 == 0) {
        execl("./passenger_generator", "passenger_generator", NULL);
        perror("exec passenger_generator");
        _exit(1);
    }

    // Czekanie na zakończenie wszystkich procesów
    while (wait(NULL) > 0);

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [MAIN] System zakonczony\n", b);
    log_write(ln);

    if (shmdt(bus) == -1) {
        perror("shmdt");
    }

    cleanup();
    return 0;
}