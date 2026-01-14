#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ipc.h"

int shmid, semid, msgid;
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

void sem_lock() {
    struct sembuf sb = { 0, -1, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop lock");
    }
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop unlock");
    }
}

void gate_lock(int gate) {
    struct sembuf sb = { gate, -1, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop gate_lock");
    }
}

void gate_unlock(int gate) {
    struct sembuf sb = { gate, 1, 0 };
    if (semop(semid, &sb, 1) == -1) {
        perror("semop gate_unlock");
    }
}

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S');
    if (shm_key == -1) {
        perror("ftok shm");
        return 1;
    }

    key_t sem_key = ftok(SEM_PATH, 'E');
    if (sem_key == -1) {
        perror("ftok sem");
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

    semid = semget(sem_key, 4, 0600);
    if (semid == -1) {
        perror("semget");
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

    srand((unsigned)(getpid() ^ time(NULL)));
    int vip = (rand() % 100 == 0);
    int bike = rand() % 2;
    int age = rand() % 80;
    int with_child = (rand() % 5 == 0);

    char b[64];
    ts(b, sizeof(b));
    char ln[256];

    struct msg m;
    m.type = REGISTER;
    m.pid = getpid();
    m.vip = vip;
    m.bike = bike;
    m.child = 0;
    m.ticket_ok = vip ? 1 : 0;

    if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
        perror("msgsnd register");
    }

    if (!vip) {
        int got = 0;
        int no_msg_count = 0;

        for (int attempts = 0; attempts < 10000; attempts++) {
            ssize_t rr = msgrcv(msgid, &m, sizeof(m) - sizeof(long), getpid(), IPC_NOWAIT);
            if (rr >= 0) {
                got = 1;
                break;
            }
            if (errno != ENOMSG) {
                perror("msgrcv ticket");
                break;
            }

            sem_lock();
            int shutdown = bus->shutdown;
            int blocked = bus->station_blocked;
            sem_unlock();

            if (shutdown || blocked) break;

            no_msg_count++;
            if (no_msg_count > 100) {
                sleep(1);
                no_msg_count = 0;
            }
        }

        if (!got || !m.ticket_ok) {
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Brak biletu/Anulacja\n", b, getpid());
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            if (shmdt(bus) == -1) {
                perror("shmdt");
            }
            return 0;
        }
    }

    sem_lock();
    if (bus->shutdown || bus->station_blocked) {
        sem_unlock();
        snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Dworzec zablokowany\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        if (shmdt(bus) == -1) {
            perror("shmdt");
        }
        return 0;
    }
    sem_unlock();

    if (age < 8 && !with_child) {
        snprintf(ln, sizeof(ln), "[%s] [DZIECKO %d] Bez opiekuna\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        if (shmdt(bus) == -1) {
            perror("shmdt");
        }
        return 0;
    }

    if (with_child) {
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            perror("pipe");
        }

        sem_lock();
        bus->active_passengers++;
        sem_unlock();

        pid_t cpid = fork();
        if (cpid == -1) {
            perror("fork child");
        }
        else if (cpid == 0) {
            if (close(pipefd[1]) == -1) {
                perror("close pipe write");
            }

            struct msg cm;
            cm.type = REGISTER;
            cm.pid = getpid();
            cm.vip = vip;
            cm.bike = 0;
            cm.child = 1;
            cm.ticket_ok = 1;

            if (msgsnd(msgid, &cm, sizeof(cm) - sizeof(long), 0) == -1) {
                perror("msgsnd child register");
            }

            char bufc;
            if (read(pipefd[0], &bufc, 1) == -1) {
                perror("read pipe");
            }
            if (close(pipefd[0]) == -1) {
                perror("close pipe read");
            }

            gate_lock(1);
            gate_unlock(1);

            sem_lock();
            bus->active_passengers--;
            sem_unlock();

            if (shmdt(bus) == -1) {
                perror("shmdt");
            }
            return 0;
        }
        else {
            if (close(pipefd[0]) == -1) {
                perror("close pipe read");
            }

            gate_lock(bike ? 2 : 1);

            sem_lock();
            if (bus->shutdown || bus->station_blocked || bus->departing) {
                sem_unlock();
                gate_unlock(bike ? 2 : 1);
                snprintf(ln, sizeof(ln), "[%s] [DOROSLY+DZIECKO %d] Odmowa\n", b, getpid());
                log_write(ln);
                sem_lock();
                bus->active_passengers -= 2;
                sem_unlock();
                if (close(pipefd[1]) == -1) {
                    perror("close pipe write");
                }
                if (shmdt(bus) == -1) {
                    perror("shmdt");
                }
                return 0;
            }

            if (bus->passengers + 2 > bus->P || (bike && bus->bikes + 1 > bus->R)) {
                sem_unlock();
                gate_unlock(bike ? 2 : 1);
                snprintf(ln, sizeof(ln), "[%s] [DOROSLY+DZIECKO %d] Brak miejsca\n", b, getpid());
                log_write(ln);
                sem_lock();
                bus->active_passengers -= 2;
                sem_unlock();
                if (close(pipefd[1]) == -1) {
                    perror("close pipe write");
                }
                if (shmdt(bus) == -1) {
                    perror("shmdt");
                }
                return 0;
            }

            bus->passengers += 2;
            if (bike) bus->bikes += 1;
            sem_unlock();
            gate_unlock(bike ? 2 : 1);

            if (write(pipefd[1], "X", 1) == -1) {
                perror("write pipe");
            }
            if (close(pipefd[1]) == -1) {
                perror("close pipe write");
            }

            snprintf(ln, sizeof(ln), "[%s] [DOROSLY+DZIECKO %d] Wejscie\n", b, getpid());
            log_write(ln);

            sem_lock();
            bus->active_passengers -= 2;
            sem_unlock();

            if (shmdt(bus) == -1) {
                perror("shmdt");
            }
            return 0;
        }
    }

    gate_lock(bike ? 2 : 1);

    sem_lock();
    if (bus->shutdown || bus->station_blocked || bus->departing) {
        sem_unlock();
        gate_unlock(bike ? 2 : 1);
        snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Odmowa\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        if (shmdt(bus) == -1) {
            perror("shmdt");
        }
        return 0;
    }

    if (bus->passengers >= bus->P || (bike && bus->bikes >= bus->R)) {
        sem_unlock();
        gate_unlock(bike ? 2 : 1);
        snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Brak miejsca\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        if (shmdt(bus) == -1) {
            perror("shmdt");
        }
        return 0;
    }

    bus->passengers++;
    if (bike) bus->bikes++;
    sem_unlock();
    gate_unlock(bike ? 2 : 1);

    snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Wejscie\n", b, getpid());
    log_write(ln);

    sem_lock();
    bus->active_passengers--;
    sem_unlock();

    if (shmdt(bus) == -1) {
        perror("shmdt");
    }

    return 0;
}