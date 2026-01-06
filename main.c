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
    if (fd == -1) { perror("open report"); return; }
    write(fd, s, (int)strlen(s));
    close(fd);
}

void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    strftime(buf, n, "%H:%M:%S", tm_info);
}

void cleanup() {
    shmctl(shmid, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);
    msgctl(msgid, IPC_RMID, NULL);
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
    char b[64]; ts(b, sizeof(b));
    char ln[128]; snprintf(ln, sizeof(ln), "[%s] [MAIN] SIGINT shutdown\n", b);
    log_write(ln);
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "Uzycie: %s N P R T TOTAL\n", argv[0]);
        return EXIT_FAILURE;
    }
    int N = atoi(argv[1]);
    int P = atoi(argv[2]);
    int R = atoi(argv[3]);
    int T = atoi(argv[4]);
    int TOTAL = atoi(argv[5]);
    if (N <= 0 || P <= 0 || R < 0 || T <= 0 || TOTAL <= 0) {
        fprintf(stderr, "Niepoprawne parametry\n");
        return EXIT_FAILURE;
    }

    int fdrep = creat("report.txt", 0600);
    if (fdrep == -1) perror("creat report");
    else close(fdrep);

    int fds = creat(SHM_PATH, 0600); if (fds == -1) perror("creat shm"); else close(fds);
    int fde = creat(SEM_PATH, 0600); if (fde == -1) perror("creat sem"); else close(fde);
    int fdm = creat(MSG_PATH, 0600); if (fdm == -1) perror("creat msg"); else close(fdm);

    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');
    if (shm_key == -1 || sem_key == -1 || msg_key == -1) { perror("ftok"); return EXIT_FAILURE; }

    shmid = shmget(shm_key, sizeof(struct BusState), IPC_CREAT | 0600);
    if (shmid == -1) { perror("shmget"); return EXIT_FAILURE; }
    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) { perror("shmat"); return EXIT_FAILURE; }

    semid = semget(sem_key, 4, IPC_CREAT | 0600);
    if (semid == -1) { perror("semget"); return EXIT_FAILURE; }
    if (semctl(semid, 0, SETVAL, 1) == -1) perror("semctl");
    if (semctl(semid, 1, SETVAL, 1) == -1) perror("semctl");
    if (semctl(semid, 2, SETVAL, 1) == -1) perror("semctl");
    if (semctl(semid, 3, SETVAL, 1) == -1) perror("semctl");

    msgid = msgget(msg_key, IPC_CREAT | 0600);
    if (msgid == -1) { perror("msgget"); return EXIT_FAILURE; }

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

    struct sigaction sa; sa.sa_handler = handle_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1) perror("sigaction SIGINT");

    for (int i = 0; i < N; i++) {
        pid_t p = fork();
        if (p == -1) { perror("fork driver"); }
        else if (p == 0) { execl("./driver", "driver", NULL); perror("exec driver"); _exit(1); }
    }

    pid_t p1 = fork();
    if (p1 == -1) perror("fork cashier");
    else if (p1 == 0) { execl("./cashier", "cashier", NULL); perror("exec cashier"); _exit(1); }

    pid_t p2 = fork();
    if (p2 == -1) perror("fork dispatcher");
    else if (p2 == 0) { execl("./dispatcher", "dispatcher", NULL); perror("exec dispatcher"); _exit(1); }

    srand((unsigned)time(NULL));
    for (int i = 0; i < TOTAL; i++) {
        struct timespec tsd;
        tsd.tv_sec = 0;
        tsd.tv_nsec = ((long)((rand() % 1000 + 200) * 1000)) * 1000L;
        nanosleep(&tsd, NULL);
        pid_t p = fork();
        if (p == -1) perror("fork passenger");
        else if (p == 0) { execl("./passenger", "passenger", NULL); perror("exec passenger"); _exit(1); }
    }

    while (wait(NULL) > 0) {}

    int fd = open("report.txt", O_RDONLY);
    if (fd != -1) {
        char buf[128];
        read(fd, buf, sizeof(buf));
        close(fd);
    }

    shmdt(bus);
    cleanup();
    return 0;
}