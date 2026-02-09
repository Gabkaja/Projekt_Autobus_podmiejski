/*
 * driver.c
 * 
 * Proces kierowcy autobusu. Każdy kierowca w nieskończonej pętli:
 * 1. Zajmuje dworzec
 * 2. Czeka T sekund lub na SIGUSR1 (wymuszenie)
 * 3. Zabiera pasażerów i odjeżdża
 * 4. Jedzie (losowo 3-9 sekund)
 * 5. Rozwozi pasażerów i wraca
 * 
 * Tylko jeden autobus może być na dworcu jednocześnie (semafor gate[3]).
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "ipc.h"

int shmid, semid, msgid;
struct BusState* bus;
volatile sig_atomic_t force_flag = 0;  /* Flaga wymuszonego odjazdu */

/* Generuje znacznik czasu HH:MM:SS */
void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

/* Zapis do logu kierowcy */
void log_write(const char* s) {
    int fd = open("driver.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Zapis do głównego raportu */
void log_main(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Blokada mutexa */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Odblokowanie mutexa */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/*
 * Blokada bramki.
 * gate[1] - pasażerowie z rowerami
 * gate[2] - pasażerowie bez rowerów
 * gate[3] - dworzec (tylko jeden autobus)
 */
void gate_lock(int gate) {
    struct sembuf sb = { gate, -1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Odblokowanie bramki */
void gate_unlock(int gate) {
    struct sembuf sb = { gate, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Obsługa SIGUSR1 - wymuszenie odjazdu z dyspozytora */
void handle_usr1(int sig) {
    (void)sig;
    force_flag = 1;
}

/* Obsługa SIGUSR2 - blokada dworca */
void handle_usr2(int sig) {
    (void)sig;
    sem_lock();
    bus->station_blocked = 1;
    sem_unlock();
}

/* Obsługa SIGINT - shutdown */
void handle_int(int sig) {
    (void)sig;
    sem_lock();
    bus->shutdown = 1;
    bus->station_blocked = 1;
    sem_unlock();
}

int main() {
    /* Generowanie kluczy IPC */
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1) {
        perror("ftok");
        return 1;
    }

    /* Podłączenie do zasobów IPC */
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

    /* Konfiguracja obsługi sygnałów */
    struct sigaction sa1;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = handle_usr1;
    sigemptyset(&sa1.sa_mask);
    sa1.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa1, NULL);

    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = handle_usr2;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa2, NULL);

    struct sigaction sai;
    memset(&sai, 0, sizeof(sai));
    sai.sa_handler = handle_int;
    sigemptyset(&sai.sa_mask);
    sai.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sai, NULL);

    /* Inicjalizacja generatora losowego dla czasu jazdy */
    srand((unsigned)(getpid() ^ time(NULL)));

    char b[64];
    ts(b, sizeof(b));
    char ln[2048];
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Start pracy\n", b, getpid());
    log_write(ln);
    log_main(ln);

    /*
     * GŁÓWNA PĘTLA KIEROWCY
     * Każda iteracja: zajęcie dworca -> oczekiwanie -> jazda -> powrót
     */
    for (;;) {
        /*
         * ZAJĘCIE DWORCA
         * WAŻNE: Najpierw mutex, potem gate (unikamy deadlocka!)
         */
        sem_lock();
        
        /* Sprawdzamy czy nie ma już innego kierowcy */
        if (bus->driver_pid != 0 && bus->driver_pid != getpid()) {
            int sd_tmp = bus->shutdown;
            int sb_tmp = bus->station_blocked;
            sem_unlock();
            
            if (sd_tmp || sb_tmp) {
                break;
            }
            
            sleep(1);
            continue;
        }
        
        int sb = bus->station_blocked;
        int sd = bus->shutdown;
        sem_unlock();

        /* Kończymy TYLKO gdy shutdown lub station_blocked */
        if (sd || sb) {
            break;
        }

        /* Zajmujemy dworzec (gate[3]) - tylko jeden autobus */
        gate_lock(3);
        
        /* Double-check po zajęciu gate */
        sem_lock();
        if (bus->driver_pid != 0 && bus->driver_pid != getpid()) {
            sem_unlock();
            gate_unlock(3);
            sleep(1);
            continue;
        }
        
        /* Rejestrujemy się jako kierowca na dworcu */
        bus->driver_pid = getpid();
        bus->departing = 0;
        sb = bus->station_blocked;
        sd = bus->shutdown;
        int wait_time = bus->T;
        sem_unlock();

        if (sd || sb) {
            gate_unlock(3);
            break;
        }

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Autobus na dworcu\n", b, getpid());
        log_write(ln);
        log_main(ln);

        /*
         * OCZEKIWANIE NA PASAŻERÓW
         * Czekamy T sekund LUB na SIGUSR1 od dyspozytora.
         */
        int waited = 0;
        while (!force_flag && waited < wait_time) {
            sleep(1);
            waited++;

            sem_lock();
            sd = bus->shutdown;
            sb = bus->station_blocked;
            sem_unlock();

            if (sd || sb) break;
        }

        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        int current_passengers = bus->passengers;
        sem_unlock();

        /*
         * WAŻNE: Jeśli shutdown ale są pasażerowie - MUSIMY dokończyć trasę!
         * Nie możemy zostawić pasażerów w autobusie.
         */
        if ((sd || sb) && current_passengers == 0) {
            gate_unlock(3);
            break;
        }
        
        /* Reset flagi wymuszonego odjazdu */
        force_flag = 0;

        /*
         * ODJAZD
         * gate[1] i gate[2] są używane przez pasażerów przy wsiadaniu!
         * Ustawiamy tylko departing aby zablokować nowe wsiadania.
         */
        sem_lock();
        bus->departing = 1;
        int p = bus->passengers;
        int r = bus->bikes;
        int pcount = bus->passenger_count;
        bus->boarded_passengers += p;
        
        /* Kopiujemy listę pasażerów */
        pid_t plist[MAX_BUS_CAPACITY];
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            plist[i] = bus->passenger_list[i];
        }
        sem_unlock();

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Odjazd: %d pasazerow, %d rowerow\n", 
                 b, getpid(), p, r);
        log_write(ln);
        log_main(ln);

        /* Reset liczników - zwalniamy dworzec dla następnego */
        sem_lock();
        bus->passengers = 0;
        bus->bikes = 0;
        bus->passenger_count = 0;
        bus->driver_pid = 0;
        sem_unlock();
        
        pid_t my_pid = getpid();

        /* Zwalniamy dworzec */
        gate_unlock(3);

        /*
         * JAZDA (losowo 3-9 sekund)
         * KRYTYCZNE: Jeśli mamy pasażerów, MUSIMY dokończyć trasę nawet przy shutdown!
         * Blokujemy SIGINT podczas jazdy.
         */
        int Ti = (rand() % 7) + 3;
        
        if (p > 0) {
            /* Mamy pasażerów - MUSIMY ich odwieźć */
            sigset_t sigset, oldset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGINT);
            sigprocmask(SIG_BLOCK, &sigset, &oldset);
            
            sleep(Ti);
            
            /* Odblokowujemy SIGINT */
            sigprocmask(SIG_SETMASK, &oldset, NULL);
        } else {
            /* Brak pasażerów - możemy przerwać przy shutdown */
            for (int i = 0; i < Ti; i++) {
                sleep(1);
                sem_lock();
                sd = bus->shutdown;
                sb = bus->station_blocked;
                sem_unlock();
                if (sd || sb) break;
            }
        }

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Powrot po %ds\n", b, getpid(), Ti);
        log_write(ln);
        log_main(ln);

        /* Wyświetlanie listy rozwiezionych pasażerów */
        if (pcount > 0) {
            char plist_str[1024] = "[";
            for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
                char tmp[64];
                pid_t pid = plist[i];
                if (pid < 0) {
                    /* Dziecko (wątek) */
                    snprintf(tmp, sizeof(tmp), "dziecko_%d%s", -pid, (i < pcount - 1) ? ", " : "");
                } else {
                    snprintf(tmp, sizeof(tmp), "%d%s", pid, (i < pcount - 1) ? ", " : "");
                }
                strncat(plist_str, tmp, sizeof(plist_str) - strlen(plist_str) - 1);
            }
            strncat(plist_str, "]", sizeof(plist_str) - strlen(plist_str) - 1);
            
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Rozwieziono pasazerow: %s\n", 
                     b, getpid(), plist_str);
            log_write(ln);
            log_main(ln);
        }

        /*
         * POWIADOMIENIA DO PASAŻERÓW
         * Wysyłamy MSG_BUS_RETURNED + PID do każdego pasażera.
         * Dzieci (negatywne PID) pomijamy - wątki nie czekają na wiadomości.
         */
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            pid_t passenger_pid = plist[i];
            
            /* Pomijamy dzieci */
            if (passenger_pid < 0) {
                continue;
            }
            
            struct msg m;
            m.type = MSG_BUS_RETURNED + passenger_pid;
            m.driver_pid = my_pid;
            m.pid = passenger_pid;
            
            /* Non-blocking - ignorujemy błędy (pasażer mógł się zakończyć) */
            if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), IPC_NOWAIT) == -1) {
                if (errno != EIDRM) {
                    /* Ignorujemy */
                }
            }
        }

        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) break;
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Koniec pracy\n", b, getpid());
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
