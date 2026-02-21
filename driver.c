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
volatile sig_atomic_t force_flag = 0;

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
    int fd = open("driver.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

void log_main(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    /* Powtarzaj jeśli przerwane sygnałem – inaczej mutex może być wzięty
     * bez blokady i doprowadzić do wyścigu na pamięci dzielonej. */
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return; /* IPC usunięte */
        return;
    }
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* gate_lock/gate_unlock – sem[3] to "jeden autobus na dworcu".
 * SEM_UNDO gwarantuje zwolnienie semafora nawet gdy driver padnie. */
void gate_lock(int gate) {
    struct sembuf sb = { (unsigned short)gate, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

void gate_unlock(int gate) {
    struct sembuf sb = { (unsigned short)gate, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

void handle_usr1(int sig) {
    (void)sig;
    force_flag = 1;
}

void handle_usr2(int sig) {
    (void)sig;
    sem_lock();
    bus->station_blocked = 1;
    sem_unlock();
}

void handle_int(int sig) {
    (void)sig;
    sem_lock();
    bus->shutdown = 1;
    bus->station_blocked = 1;
    sem_unlock();
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
    semid = semget(sem_key, 7, 0600);  // 7 semaforów
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

    srand((unsigned)(getpid() ^ time(NULL)));

    char b[64];
    ts(b, sizeof(b));
    char ln[2048];  // Zwiększony bufor dla listy pasażerów
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Start pracy\n", b, getpid());
    log_write(ln);
    log_main(ln);

    for (;;) {
        // WAŻNE: Najpierw mutex, potem gate (unikamy deadlocka!)
        sem_lock();
        
        // Sprawdzamy czy nie ma już innego kierowcy
        if (bus->driver_pid != 0 && bus->driver_pid != getpid()) {
            int sd_tmp = bus->shutdown;
            int sb_tmp = bus->station_blocked;
            sem_unlock();
            
            // Jeśli shutdown - kończymy od razu
            if (sd_tmp || sb_tmp) {
                break;
            }
            
            sleep(1);
            continue;
        }
        
        int sb = bus->station_blocked;
        int sd = bus->shutdown;
        sem_unlock();

        // Kończymy TYLKO gdy shutdown lub station_blocked
        if (sd || sb) {
            break;
        }

        // Tylko jeden autobus na dworcu - gate[3]
        gate_lock(3);
        
        // Sprawdzamy ponownie po wzięciu gate
        sem_lock();
        if (bus->driver_pid != 0 && bus->driver_pid != getpid()) {
            sem_unlock();
            gate_unlock(3);
            sleep(1);
            continue;
        }
        
        bus->driver_pid = getpid();
        bus->departing = 0;
        sb = bus->station_blocked;
        sd = bus->shutdown;
        int wait_time = bus->T;
        sem_unlock();

        // Kończymy TYLKO gdy shutdown lub station_blocked
        if (sd || sb) {
            gate_unlock(3);
            break;
        }

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Autobus na dworcu\n", b, getpid());
        log_write(ln);
        log_main(ln);

        // Czekamy T sekund lub na sygnał od dyspozytora
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

        // WAŻNE: Jeśli shutdown ale są pasażerowie - dokończ trasę!
        if ((sd || sb) && current_passengers == 0) {
            // Shutdown i brak pasażerów - kończymy
            gate_unlock(3);
            break;
        }
        
        // Jeśli są pasażerowie - jedź nawet przy shutdown
        // (dokończ trasę z tymi co już wsiedli)

        force_flag = 0;

        // POPRAWKA: Bramki gate[1] i gate[2] są używane przez pasażerów przy wsiadaniu!
        // gate[1] - pasażerowie z rowerami
        // gate[2] - pasażerowie bez rowerów
        // Ustawiamy tylko flagę departing, żeby zablokować nowe wsiadania

        sem_lock();
        bus->departing = 1;
        int p = bus->passengers;
        int r = bus->bikes;
        int pcount = bus->passenger_count;
        bus->boarded_passengers += p;
        
        // Kopiujemy listę pasażerów
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

        // Reset liczników
        sem_lock();
        bus->passengers = 0;
        bus->bikes = 0;
        bus->passenger_count = 0;
        bus->driver_pid = 0;  // Czyścimy driver_pid
        sem_unlock();

        // Odblokowujemy tylko dworzec (gate[1] i gate[2] nie są używane!)
        gate_unlock(3);

        // Jazda (losowy czas 3-9s)
        // WAŻNE: Jeśli mamy pasażerów, MUSIMY dokończyć trasę nawet przy shutdown!
        int Ti =(rand() % 7) + 3;
        
        if (p > 0) {
            // Mamy pasażerów - MUSIMY ich odwieźć
            // Blokujemy SIGINT podczas jazdy
            sigset_t sigset, oldset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGINT);
            sigprocmask(SIG_BLOCK, &sigset, &oldset);
            
            sleep(Ti);
            
            // Odblokowujemy SIGINT
            sigprocmask(SIG_SETMASK, &oldset, NULL);
        } else {
            // Brak pasażerów (pustka) - możemy przerwać przy shutdown
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

        // Wyświetlamy listę pasażerów
        if (pcount > 0) {
            char plist_str[1024] = "[";
            for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
                char tmp[64];
                pid_t pid = plist[i];
                if (pid < 0) {
                    // Dziecko - wyświetlamy jako "dziecko_PID"
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

        // NOWA LOGIKA: Oznaczamy w shared memory że pasażerowie wrócili
        // Zamiast wysyłać wiadomości (bottleneck!), ustawiamy flagi i budzimy semaforem
        
        sem_lock();
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            pid_t passenger_pid = plist[i];
            
            // Pomijamy dzieci (negatywne PID-y) - one nie czekają na semaforze
            if (passenger_pid < 0) {
                continue;
            }
            
            // Ustawiamy flagę że ten pasażer może zakończyć
            if (passenger_pid > 0 && passenger_pid < MAX_PID) {
                bus->passenger_trip_completed[passenger_pid] = 1;
            }
        }
        sem_unlock();
        
        // Teraz budzimy wszystkich pasażerów podnosząc semafor trip_completed (sem[6])
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            pid_t passenger_pid = plist[i];
            
            // Pomijamy dzieci
            if (passenger_pid < 0) {
                continue;
            }
            
            // Podnosimy semafor - budzi jednego pasażera
            struct sembuf sb = { 6, 1, 0 };  // Bez SEM_UNDO!
            if (semop(semid, &sb, 1) == -1) {
                if (errno == EIDRM || errno == EINVAL) {
                    // Semafory usunięte - system się kończy
                    break;
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
