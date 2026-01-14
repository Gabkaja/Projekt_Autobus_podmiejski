#include <stdio.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ipc.h"

int shmid, msgid;
struct BusState* bus;

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

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S');
    if (shm_key == -1) {
        perror("ftok shm");
        return 1;
    }

    key_t msg_key = ftok(MSG_PATH, 'M');
    if (msg_key == -1) {
        perror("ftok msg");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    if (shmid == -1) {
        perror("shmget");
        return 1;
    }

    msgid = msgget(msg_key, 0600);
    if (msgid == -1) {
        perror("msgget");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    int no_msg_count = 0;
    for (;;) {
        struct msg m;
        ssize_t r = msgrcv(msgid, &m, sizeof(m) - sizeof(long), REGISTER, IPC_NOWAIT);

        if (r < 0) {
            if (errno == ENOMSG) {
                no_msg_count++;

                if (bus->shutdown || bus->active_passengers == 0) {
                    break;
                }

                if (no_msg_count > 100) {
                    sleep(1);
                    no_msg_count = 0;
                }
                continue;
            }
            else {
                perror("msgrcv");
                break;
            }
        }

        no_msg_count = 0;

        char b[64];
        ts(b, sizeof(b));
        char ln[256];
        snprintf(ln, sizeof(ln), "[%s] [KASA] Rejestracja PID %d VIP %d DZIECKO %d\n",
            b, m.pid, m.vip, m.child);
        log_write(ln);

        if (!m.vip) {
            m.ticket_ok = 1;
            m.type = m.pid;
            if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
                perror("msgsnd reply");
            }
        }
    }

    char b2[64];
    ts(b2, sizeof(b2));
    char ln2[128];
    snprintf(ln2, sizeof(ln2), "[%s] [KASA] Koniec pracy\n", b2);
    log_write(ln2);

    if (shmdt(bus) == -1) {
        perror("shmdt");
    }

    return 0;
}