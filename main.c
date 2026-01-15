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
        perror("localtime");
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
    if (unlink(SHM_PATH) == -1 && errno != ENOENT) {
        perror("unlink SHM_PATH");
    }
    if (unlink(SEM_PATH) == -1 && errno != ENOENT) {
        perror("unlink SEM_PATH");
    }
    if (unlink(MSG_PATH) == -1 && errno != ENOENT) {
        perror("unlink MSG_PATH");
    }
}

void handle_sigint(int sig) {
    (void)sig;
    if (bus) {
        bus->shutdown = 1;
        bus->station_blocked = 1;
    }
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [MAIN] SIGINT shutdown\n", b);
    log_write(ln);
}

void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "Uzycie: %s N P R T TOTAL\n", argv[0]);
        fprintf(stderr, "  N - liczba autobusow (kierowcow)\n");
        fprintf(stderr, "  P - maksymalna liczba pasazerow w autobusie\n");
        fprintf(stderr, "  R - maksymalna liczba rowerow w autobusie\n");
        fprintf(stderr, "  T - czas oczekiwania na dworcu (sekundy)\n");
        fprintf(stderr, "  TOTAL - calkowita liczba pasazerow do obslugi\n");
        return EXIT_FAILURE;
    }

    int N = atoi(argv[1]);
    int P = atoi(argv[2]);
    int R = atoi(argv[3]);
    int T = atoi(argv[4]);
    int TOTAL = atoi(argv[5]);

    if (N <= 0 || N > 100) {
        fprintf(stderr, "Niepoprawna liczba autobusow N (musi byc 1-100): %d\n", N);
        return EXIT_FAILURE;
    }
    if (P <= 0 || P > 1000) {
        fprintf(stderr, "Niepoprawna pojemnosc P (musi byc 1-1000): %d\n", P);
        return EXIT_FAILURE;
    }
    if (R < 0 || R > 100) {
        fprintf(stderr, "Niepoprawna liczba rowerow R (musi byc 0-100): %d\n", R);
        return EXIT_FAILURE;
    }
    if (T <= 0 || T > 3600) {
        fprintf(stderr, "Niepoprawny czas T (musi byc 1-3600): %d\n", T);
        return EXIT_FAILURE;
    }
    if (TOTAL <= 0 || TOTAL > 10000) {
        fprintf(stderr, "Niepoprawna liczba pasazerow TOTAL (musi byc 1-10000): %d\n", TOTAL);
        return EXIT_FAILURE;
    }

    int fdrep = creat("report.txt", 0600);
    if (fdrep == -1) {
        perror("creat report");
        return EXIT_FAILURE;
    }
    if (close(fdrep) == -1) {
        perror("close report");
    }

    int fds = creat(SHM_PATH, 0600);
    if (fds == -1) {
        perror("creat shm");
        return EXIT_FAILURE;
    }
    if (close(fds) == -1) {
        perror("close shm");
    }

    int fde = creat(SEM_PATH, 0600);
    if (fde == -1) {
        perror("creat sem");
        return EXIT_FAILURE;
    }
    if (close(fde) == -1) {
        perror("close sem");
    }

    int fdm = creat(MSG_PATH, 0600);
    if (fdm == -1) {
        perror("creat msg");
        return EXIT_FAILURE;
    }
    if (close(fdm) == -1) {
        perror("close msg");
    }

    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');

    if (shm_key == -1) {
        perror("ftok shm");
        cleanup();
        return EXIT_FAILURE;
    }
    if (sem_key == -1) {
        perror("ftok sem");
        cleanup();
        return EXIT_FAILURE;
    }
    if (msg_key == -1) {
        perror("ftok msg");
        cleanup();
        return EXIT_FAILURE;
    }

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

    semid = semget(sem_key, 4, IPC_CREAT | 0600);
    if (semid == -1) {
        perror("semget");
        cleanup();
        return EXIT_FAILURE;
    }

    if (semctl(semid, 0, SETVAL, 1) == -1) {
        perror("semctl sem0");
        cleanup();
        return EXIT_FAILURE;
    }
    if (semctl(semid, 1, SETVAL, 1) == -1) {
        perror("semctl sem1");
        cleanup();
        return EXIT_FAILURE;
    }
    if (semctl(semid, 2, SETVAL, 1) == -1) {
        perror("semctl sem2");
        cleanup();
        return EXIT_FAILURE;
    }
    if (semctl(semid, 3, SETVAL, 1) == -1) {
        perror("semctl sem3");
        cleanup();
        return EXIT_FAILURE;
    }

    msgid = msgget(msg_key, IPC_CREAT | 0600);
    if (msgid == -1) {
        perror("msgget");
        cleanup();
        return EXIT_FAILURE;
    }

    bus->P = P;
    bus->R = R;
    bus->T = T;
    bus->N = N;
    bus->passengers = 0;
    bus->bikes = 0;
    bus->departing = 0;
    bus->station_blocked = 0;
    bus->active_passengers = TOTAL;
    bus->driver_pid = 0;
    bus->shutdown = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
    }

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("sigaction SIGCHLD");
    }

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

    pid_t p1 = fork();
    if (p1 == -1) {
        perror("fork cashier");
    }
    else if (p1 == 0) {
        execl("./cashier", "cashier", NULL);
        perror("exec cashier");
        _exit(1);
    }

    pid_t p2 = fork();
    if (p2 == -1) {
        perror("fork dispatcher");
    }
    else if (p2 == 0) {
        execl("./dispatcher", "dispatcher", NULL);
        perror("exec dispatcher");
        _exit(1);
    }

    srand((unsigned)time(NULL));
    for (int i = 0; i < TOTAL; i++) {
        sleep(1);

        pid_t p = fork();
        if (p == -1) {
            perror("fork passenger");
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

    if (shmdt(bus) == -1) {
        perror("shmdt");
    }

    cleanup();
    return 0;
}