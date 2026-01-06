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

void ts(char* buf, size_t n) { time_t t = time(NULL); struct tm* tm_info = localtime(&t); strftime(buf, n, "%H:%M:%S", tm_info); }
void log_write(const char* s) { int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600); if (fd == -1) { perror("open report"); return; } write(fd, s, (int)strlen(s)); close(fd); }

void handle_int(int sig) {
    (void)sig;
    if (bus) {
        bus->station_blocked = 1;
        bus->shutdown = 1;
        if (bus->driver_pid) kill(bus->driver_pid, SIGUSR2);
    }
    char b[64]; ts(b, sizeof(b));
    char ln[128]; snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] SIGINT\n", b);
    log_write(ln);
    _exit(0);
}

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S'); if (shm_key == -1) { perror("ftok shm"); return 1; }
    shmid = shmget(shm_key, sizeof(struct BusState), 0600); if (shmid == -1) { perror("shmget"); return 1; }
    bus = shmat(shmid, NULL, 0); if (bus == (void*)-1) { perror("shmat"); return 1; }

    struct sigaction sai; memset(&sai, 0, sizeof(sai)); sai.sa_handler = handle_int; sigemptyset(&sai.sa_mask); sai.sa_flags = SA_RESTART; if (sigaction(SIGINT, &sai, NULL) == -1) perror("sigaction SIGINT");

    sleep(5);
    if (bus->driver_pid) {
        kill(bus->driver_pid, SIGUSR1);
        char b[64]; ts(b, sizeof(b));
        char ln[128]; snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Wymuszenie odjazdu\n", b);
        log_write(ln);
    }

    sleep(5);
    if (bus->driver_pid) {
        kill(bus->driver_pid, SIGUSR2);
        char b[64]; ts(b, sizeof(b));
        char ln[128]; snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Blokada dworca\n", b);
        log_write(ln);
    }

    char b2[64]; ts(b2, sizeof(b2)); char ln2[128]; snprintf(ln2, sizeof(ln2), "[%s] [DYSPOZYTOR] Koniec pracy\n", b2); log_write(ln2);
    shmdt(bus);
    return 0;
}