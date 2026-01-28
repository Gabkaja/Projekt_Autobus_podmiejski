#include <stdio.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ipc.h"

int shmid, msgid, semid;
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
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    semop(semid, &sb, 1);
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t msg_key = ftok(MSG_PATH, 'M');
    key_t sem_key = ftok(SEM_PATH, 'E');

    if (shm_key == -1 || msg_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    msgid = msgget(msg_key, 0600);
    semid = semget(sem_key, 4, 0600);

    if (shmid == -1 || msgid == -1 || semid == -1) {
        perror("get ipc");
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
    snprintf(ln, sizeof(ln), "[%s] [KASA] Start pracy\n", b);
    log_write(ln);

    int no_msg_count = 0;
    for (;;) {
        // NAJPIERW sprawdzamy shutdown PRZED odbieraniem wiadomości
        sem_lock();
        int sd = bus->shutdown;
        sem_unlock();

        if (sd) {
            break;
        }

        struct msg m;
        ssize_t r = msgrcv(msgid, &m, sizeof(m) - sizeof(long), MSG_REGISTER, IPC_NOWAIT);

        if (r < 0) {
            if (errno == ENOMSG) {
                no_msg_count++;

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

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KASA] Rejestracja PID=%d VIP=%d DZIECKO=%d\n",
                 b, m.pid, m.vip, m.child);
        log_write(ln);

        // Wysyłamy bilet tylko dla nie-VIP dorosłych (nie dzieci)
        if (!m.vip && !m.child) {
            m.ticket_ok = 1;
            m.type = MSG_TICKET_REPLY + m.pid;
            if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
                perror("msgsnd reply");
            }
        }
        // VIP i dzieci nie dostają osobnych biletów (dzieci wchodzą z rodzicem)
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [KASA] Koniec pracy\n", b);
    log_write(ln);

    shmdt(bus);
    return 0;
}