#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <fcntl.h>
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
    struct sembuf sb = { 0, -1, 0 };
    semop(semid, &sb, 1);
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, 0 };
    semop(semid, &sb, 1);
}

void gate_lock(int gate) {
    struct sembuf sb = { gate, -1, 0 };
    semop(semid, &sb, 1);
}

void gate_unlock(int gate) {
    struct sembuf sb = { gate, 1, 0 };
    semop(semid, &sb, 1);
}

int try_board(int bike, int with_child, int vip) {
    (void)vip;
    int gate = bike ? 2 : 1;
    
    // ATOMOWA operacja: lock gate -> sprawdź warunki -> wsiądź/odrzuć -> unlock
    gate_lock(gate);

    sem_lock();
    int sd = bus->shutdown;
    int sb = bus->station_blocked;
    int dep = bus->departing;
    int pass = bus->passengers;
    int bks = bus->bikes;
    int P = bus->P;
    int R = bus->R;

    // Nie możemy wsiąść - system zamknięty lub autobus odjeżdża
    if (sd || sb) {
        sem_unlock();
        gate_unlock(gate);
        return 0; // System się wyłącza - kończymy proces
    }

    // Sprawdzamy czy jest miejsce
    int needed_seats = with_child ? 2 : 1;
    int needed_bikes = bike ? 1 : 0;

    if (pass + needed_seats > P || bks + needed_bikes > R || dep) {
        sem_unlock();
        gate_unlock(gate);
        return -1; // Brak miejsca - czekamy na następny autobus
    }

    // Wchodzimy - ATOMOWO
    bus->passengers += needed_seats;
    bus->bikes += needed_bikes;
    sem_unlock();

    gate_unlock(gate);
    return 1; // Sukces
}

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1) {
        perror("ftok");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 4, 0600);
    msgid = msgget(msg_key, 0600);

    if (shmid == -1 || semid == -1 || msgid == -1) {
        perror("get ipc");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    srand((unsigned)(getpid() ^ time(NULL)));

    int vip = (rand() % 100 == 0); // 1% VIP
    int bike = rand() % 2;
    int age = rand() % 80;
    int with_child = (age >= 18 && rand() % 5 == 0); // 20% dorosłych z dzieckiem

    char b[64];
    char ln[256];

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Przybycie (VIP=%d wiek=%d rower=%d dziecko=%d)\n", 
             b, getpid(), vip, age, bike, with_child);
    log_write(ln);

    // Sprawdzamy czy dworzec jest otwarty
    sem_lock();
    int sb = bus->station_blocked;
    int sd = bus->shutdown;
    sem_unlock();

    if (sb || sd) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Dworzec zamkniety\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        shmdt(bus);
        return 0;
    }

    // Dziecko bez opiekuna nie może jechać
    if (age < 8) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [DZIECKO %d] Bez opiekuna - odmowa\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        shmdt(bus);
        return 0;
    }

    // Rejestracja w kasie
    struct msg m;
    m.type = MSG_REGISTER;
    m.pid = getpid();
    m.vip = vip;
    m.bike = bike;
    m.child = 0;
    m.ticket_ok = vip ? 1 : 0;

    // Sprawdź shutdown przed wysłaniem
    sem_lock();
    sd = bus->shutdown;
    sb = bus->station_blocked;
    sem_unlock();

    if (sd || sb) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Dworzec zamkniety przed rejestracją\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        shmdt(bus);
        return 0;
    }

    if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
        perror("msgsnd register");
    }

    // Czekamy na bilet (jeśli nie VIP)
    if (!vip) {
        int got_ticket = 0;
        int no_msg_count = 0;

        for (;;) {
            long ticket_type = MSG_TICKET_REPLY + getpid();
            ssize_t rr = msgrcv(msgid, &m, sizeof(m) - sizeof(long), ticket_type, IPC_NOWAIT);
            
            if (rr >= 0) {
                got_ticket = 1;
                break;
            }

            if (errno != ENOMSG) {
                perror("msgrcv ticket");
                break;
            }

            sem_lock();
            sd = bus->shutdown;
            sb = bus->station_blocked;
            sem_unlock();

            if (sd || sb) break;

            no_msg_count++;
            if (no_msg_count > 100) {
                sleep(1);
                no_msg_count = 0;
            }
        }

        if (!got_ticket || !m.ticket_ok) {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Brak biletu\n", b, getpid());
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }
    }

    // Obsługa pasażera z dzieckiem
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
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
        }
        else if (cpid == 0) {
            // Proces dziecka - NIE rejestruje się w kasie, tylko czeka na rodzica
            close(pipefd[1]);

            char bufc;
            read(pipefd[0], &bufc, 1);
            close(pipefd[0]);

            // Dziecko wchodzi przez bramkę bez roweru (synchronicznie z rodzicem)
            gate_lock(1);
            gate_unlock(1);

            sem_lock();
            bus->active_passengers--;
            sem_unlock();

            shmdt(bus);
            return 0;
        }
        else {
            // Proces rodzica
            close(pipefd[0]);

            // Próbujemy wsiąść (rodzic + dziecko)
            for (;;) {
                int result = try_board(bike, 1, vip);

                if (result == 0) {
                    // System się wyłącza
                    close(pipefd[1]);
                    waitpid(cpid, NULL, 0);
                    sem_lock();
                    bus->active_passengers -= 2;
                    sem_unlock();
                    shmdt(bus);
                    return 0;
                }

                if (result == 1) {
                    // Sukces - wsiedliśmy
                    write(pipefd[1], "X", 1);
                    close(pipefd[1]);
                    waitpid(cpid, NULL, 0);

                    ts(b, sizeof(b));
                    snprintf(ln, sizeof(ln), "[%s] [DOROSLY+DZIECKO %d] Wsiadl (VIP=%d rower=%d)\n", 
                             b, getpid(), vip, bike);
                    log_write(ln);

                    sem_lock();
                    bus->active_passengers -= 2;
                    sem_unlock();
                    shmdt(bus);
                    return 0;
                }

                // Brak miejsca - czekamy na następny autobus
                sleep(1);

                sem_lock();
                sd = bus->shutdown;
                sb = bus->station_blocked;
                sem_unlock();

                if (sd || sb) {
                    close(pipefd[1]);
                    waitpid(cpid, NULL, 0);
                    sem_lock();
                    bus->active_passengers -= 2;
                    sem_unlock();
                    shmdt(bus);
                    return 0;
                }
            }
        }
    }

    // Zwykły pasażer - próbujemy wsiąść
    for (;;) {
        int result = try_board(bike, 0, vip);

        if (result == 0) {
            // System się wyłącza
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] System zamkniety\n", b, getpid());
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }

        if (result == 1) {
            // Sukces
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Wsiadl (VIP=%d rower=%d)\n", 
                     b, getpid(), vip, bike);
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }

        // Brak miejsca - czekamy na następny autobus
        sleep(1);

        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Dworzec zamkniety podczas oczekiwania\n", 
                     b, getpid());
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }
    }

    return 0;
}