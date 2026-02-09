/*
 * passenger.c
 * 
 * Proces pasażera próbującego wsiąść do autobusu.
 * Sekwencja:
 * 1. Losowanie atrybutów (VIP, rower, wiek, dziecko)
 * 2. Rejestracja w kasie
 * 3. Oczekiwanie na bilet (jeśli nie VIP)
 * 4. Próby wsiadania (w pętli)
 * 5. Oczekiwanie na powrót
 * 
 * Pasażer z dzieckiem tworzy wątek (pthread) synchronizowany przez mutex i condition variable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ipc.h"

int shmid, semid, msgid;
struct BusState* bus;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

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

/* Zapis do logu pasażerów (thread-safe) */
void log_write(const char* s) {
    pthread_mutex_lock(&log_mutex);
    int fd = open("passenger.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd != -1) {
        write(fd, s, strlen(s));
        close(fd);
    }
    pthread_mutex_unlock(&log_mutex);
}

/* Zapis do głównego raportu (thread-safe) */
void log_main(const char* s) {
    pthread_mutex_lock(&log_mutex);
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd != -1) {
        write(fd, s, strlen(s));
        close(fd);
    }
    pthread_mutex_unlock(&log_mutex);
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

/* Blokada bramki */
void gate_lock(int gate) {
    struct sembuf sb = { gate, -1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Odblokowanie bramki */
void gate_unlock(int gate) {
    struct sembuf sb = { gate, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/*
 * Struktura argumentów dla wątku dziecka.
 * Zawiera wskaźniki do zmiennych współdzielonych z rodzicem.
 */
typedef struct {
    int is_parent;
    pid_t parent_pid;
    int* boarded;           /* Czy wsiedliśmy? */
    int* shutdown;          /* Czy system się kończy? */
    int* bus_returned;      /* Czy bus wrócił? */
    pthread_mutex_t* mutex;
    pthread_cond_t* cond;   /* Condition variable dla synchronizacji */
} child_arg_t;

/*
 * Funkcja wątku reprezentującego dziecko.
 * Fazy:
 * 1. Czeka aż rodzic wsiądzie LUB shutdown
 * 2. Loguje wsiadanie
 * 3. Czeka na powrót LUB shutdown
 * 4. Loguje dojazd
 */
void* child_thread(void* arg) {
    child_arg_t* carg = (child_arg_t*)arg;
    
    char b[64];
    char ln[256];
    
    /* FAZA 1: Oczekiwanie na wsiadanie rodzica */
    pthread_mutex_lock(carg->mutex);
    while (!(*carg->boarded) && !(*carg->shutdown)) {
        pthread_cond_wait(carg->cond, carg->mutex);
    }
    
    if (*carg->shutdown) {
        pthread_mutex_unlock(carg->mutex);
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [DZIECKO watek] Anulowano - system sie konczy (rodzic %d)\n", 
                 b, carg->parent_pid);
        log_write(ln);
        return NULL;
    }
    pthread_mutex_unlock(carg->mutex);
    
    /* Wsiedliśmy! */
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [DZIECKO watek] Wsiadlo z rodzicem %d\n", 
             b, carg->parent_pid);
    log_write(ln);
    
    /* FAZA 2: Oczekiwanie na powrót */
    pthread_mutex_lock(carg->mutex);
    while (!(*carg->bus_returned) && !(*carg->shutdown)) {
        pthread_cond_wait(carg->cond, carg->mutex);
    }
    
    if (*carg->shutdown) {
        pthread_mutex_unlock(carg->mutex);
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [DZIECKO watek] System sie konczy podczas podrozy (rodzic %d)\n", 
                 b, carg->parent_pid);
        log_write(ln);
        return NULL;
    }
    pthread_mutex_unlock(carg->mutex);
    
    /* Dojechaliśmy! */
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [DZIECKO watek] Dojechalo z rodzicem %d\n", 
             b, carg->parent_pid);
    log_write(ln);
    
    return NULL;
}

/*
 * Próba wsiadania do autobusu (wywoływana z mutex locked!).
 * 
 * Zwraca:
 *   1 = sukces (wsiadł)
 *  -1 = brak miejsca (czekaj)
 *   0 = shutdown (zakończ proces)
 * 
 * UWAGA: mutex jest JUŻ trzymany! Nie lockujemy ponownie.
 */
int try_board_locked(int bike, int with_child, int vip) {
    (void)vip;
    (void)bike;
    
    /* Odczyt stanu (mutex już trzymany) */
    int sd = bus->shutdown;
    int sb = bus->station_blocked;
    int dep = bus->departing;
    int pass = bus->passengers;
    int bks = bus->bikes;
    int P = bus->P;
    int R = bus->R;
    int pcount = bus->passenger_count;

    /* System zamknięty */
    if (sd || sb) {
        return 0;
    }

    /* Obliczamy wymagane zasoby */
    int needed_seats = with_child ? 2 : 1;       /* Rodzic + dziecko */
    int needed_bikes = bike ? 1 : 0;
    int needed_list_slots = with_child ? 2 : 1;  /* Miejsce w liście */

    /* Sprawdzamy czy jest miejsce */
    if (dep || pass + needed_seats > P || bks + needed_bikes > R || 
        pcount + needed_list_slots > MAX_BUS_CAPACITY) {
        return -1;  /* Brak miejsca */
    }

    /*
     * ATOMOWA OPERACJA WSIADANIA
     * Zwiększamy liczniki i dodajemy do listy.
     */
    bus->passengers += needed_seats;
    bus->bikes += needed_bikes;
    
    /* Dodajemy PID rodzica */
    if (pcount < MAX_BUS_CAPACITY) {
        bus->passenger_list[pcount] = getpid();
        bus->passenger_count++;
        
        /* Dziecko jako negatywny PID (wątek, nie proces) */
        if (with_child && pcount + 1 < MAX_BUS_CAPACITY) {
            bus->passenger_list[pcount + 1] = -getpid();
            bus->passenger_count++;
        }
    }

    return 1;  /* Sukces */
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

    /* Inicjalizacja generatora losowego */
    srand((unsigned)(getpid() ^ time(NULL)));

    /*
     * LOSOWANIE ATRYBUTÓW PASAŻERA
     * vip: 10% szans (1 z 10)
     * bike: 50% szans
     * age: 0-79 lat
     * with_child: 20% szans dla dorosłych (wiek >= 18)
     */
    int vip = (rand() % 10 == 0);
    int bike = rand() % 2;
    int age = rand() % 80;
    int with_child = (age >= 18 && rand() % 5 == 0);

    char b[64];
    char ln[256];

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Przybycie (VIP=%d wiek=%d rower=%d dziecko=%d)\n", 
             b, getpid(), vip, age, bike, with_child);
    log_write(ln);
    log_main(ln);

    /* Sprawdzamy czy dworzec jest otwarty */
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

    /* WALIDACJA: Dziecko bez opiekuna nie może jechać */
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

    /* REJESTRACJA W KASIE */
    struct msg m;
    m.type = MSG_REGISTER;
    m.pid = getpid();
    m.vip = vip;
    m.bike = bike;
    m.child = with_child ? 1 : 0;
    m.ticket_ok = vip ? 1 : 0;  /* VIP już mają bilety */

    /* Sprawdzamy shutdown przed wysłaniem */
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

    /* Wysłanie wiadomości rejestracyjnej */
    if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
        if (errno == EIDRM) {
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }
        perror("msgsnd register");
    }

    /*
     * OCZEKIWANIE NA BILET (tylko nie-VIP)
     * VIP-y pomijają tę fazę.
     * Czekamy BLOKUJĄCO na MSG_TICKET_REPLY + PID.
     */
    if (!vip) {
        long ticket_type = MSG_TICKET_REPLY + getpid();
        
        ssize_t rr = msgrcv(msgid, &m, sizeof(m) - sizeof(long), ticket_type, 0);
        
        if (rr >= 0) {
            /* Otrzymaliśmy bilet */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Otrzymano bilet!\n", b, getpid());
            log_write(ln);
        } else {
            /* Błąd - prawdopodobnie kolejka usunięta */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Blad msgrcv biletu (errno=%d: %s)\n", 
                     b, getpid(), errno, strerror(errno));
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }
    }

    /*
     * OBSŁUGA DZIECKA (wątek)
     * Tworzymy wątek PRZED próbą wsiadania.
     */
    pthread_t child_tid = 0;
    pthread_mutex_t child_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t child_cond = PTHREAD_COND_INITIALIZER;
    int child_boarded = 0;
    int child_shutdown = 0;
    int bus_returned = 0;
    
    if (with_child) {
        /* Dziecko też jest "aktywnym pasażerem" w statystykach */
        sem_lock();
        bus->active_passengers++;
        sem_unlock();

        child_arg_t carg;
        carg.is_parent = 0;
        carg.parent_pid = getpid();
        carg.boarded = &child_boarded;
        carg.shutdown = &child_shutdown;
        carg.bus_returned = &bus_returned;
        carg.mutex = &child_mutex;
        carg.cond = &child_cond;
        
        if (pthread_create(&child_tid, NULL, child_thread, &carg) != 0) {
            perror("pthread_create");
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            with_child = 0;  /* Kontynuuj bez dziecka */
        }
    }

    /*
     * GŁÓWNA PĘTLA WSIADANIA
     * Próbujemy wsiąść w pętli aż do sukcesu lub shutdown.
     * Używamy bramek (gate[1] dla rowerów, gate[2] bez rowerów).
     */
    int boarded = 0;
    for (;;) {
        /* Wybór bramki */
        int gate_num = bike ? 1 : 2;
        gate_lock(gate_num);
        
        /* Sprawdzamy stan po zajęciu bramki */
        sem_lock();
        int sd = bus->shutdown;
        int sb = bus->station_blocked;
        int dep = bus->departing;
        
        /* Shutdown - kończymy */
        if (sd || sb) {
            sem_unlock();
            gate_unlock(gate_num);
            
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] System zamkniety\n", b, getpid());
            log_write(ln);
            
            /* Sygnalizacja dziecku */
            if (with_child && child_tid) {
                pthread_mutex_lock(&child_mutex);
                child_shutdown = 1;
                pthread_cond_broadcast(&child_cond);
                pthread_mutex_unlock(&child_mutex);
                
                pthread_join(child_tid, NULL);
                sem_lock();
                bus->active_passengers--;
                sem_unlock();
            }
            
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }
        
        /* Autobus odjeżdża - czekamy na następny */
        if (dep) {
            sem_unlock();
            gate_unlock(gate_num);
            sleep(1);
            continue;
        }
        
        /* Próba wsiadania (mutex już trzymany!) */
        int result = try_board_locked(bike, with_child, vip);
        
        if (result == 0) {
            /* System się wyłącza */
            sem_unlock();
            gate_unlock(gate_num);
            
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] System zamkniety podczas wsiadania\n", b, getpid());
            log_write(ln);
            
            /* Sygnalizacja dziecku */
            if (with_child && child_tid) {
                pthread_mutex_lock(&child_mutex);
                child_shutdown = 1;
                pthread_cond_broadcast(&child_cond);
                pthread_mutex_unlock(&child_mutex);
                
                pthread_join(child_tid, NULL);
                sem_lock();
                bus->active_passengers--;
                sem_unlock();
            }
            
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }

        if (result == 1) {
            /* SUKCES - Wsiedliśmy! */
            boarded = 1;
            sem_unlock();
            gate_unlock(gate_num);
            
            /* Sygnalizacja dziecku że wsiedliśmy */
            if (with_child && child_tid) {
                pthread_mutex_lock(&child_mutex);
                child_boarded = 1;
                pthread_cond_signal(&child_cond);
                pthread_mutex_unlock(&child_mutex);
            }
            
            ts(b, sizeof(b));
            if (with_child) {
                snprintf(ln, sizeof(ln), "[%s] [DOROSLY+DZIECKO %d] Wsiadl (VIP=%d rower=%d)\n", 
                         b, getpid(), vip, bike);
            } else {
                snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Wsiadl (VIP=%d rower=%d)\n", 
                         b, getpid(), vip, bike);
            }
            log_write(ln);
            log_main(ln);
            break;
        }

        /* BRAK MIEJSCA (result == -1) */
        sem_unlock();
        gate_unlock(gate_num);
        
        sleep(1);
    }

    /*
     * OCZEKIWANIE NA POWRÓT AUTOBUSU
     * Jeśli wsiedliśmy, czekamy na MSG_BUS_RETURNED + PID od kierowcy.
     */
    if (boarded) {
        /* Czyścimy stare wiadomości */
        long return_type = MSG_BUS_RETURNED + getpid();
        struct msg old_msg;
        while (msgrcv(msgid, &old_msg, sizeof(old_msg) - sizeof(long), return_type, IPC_NOWAIT) >= 0) {
            /* Wyrzucamy stare wiadomości */
        }
        
        /* Czekamy BLOKUJĄCO na nową wiadomość */
        struct msg ret_msg;
        ssize_t rr = msgrcv(msgid, &ret_msg, sizeof(ret_msg) - sizeof(long), return_type, 0);
        
        if (rr >= 0) {
            /* Dojechaliśmy - logujemy tylko w passenger.log */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Dojechalem (bus %d)\n", 
                     b, getpid(), ret_msg.driver_pid);
            log_write(ln);
        } else if (errno == EIDRM) {
            /* Kolejka usunięta */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] System zakonczony podczas podrozy\n", b, getpid());
            log_write(ln);
        }
        
        /* Informujemy dziecko że bus wrócił */
        if (with_child && child_tid) {
            pthread_mutex_lock(&child_mutex);
            bus_returned = 1;
            pthread_cond_signal(&child_cond);
            pthread_mutex_unlock(&child_mutex);
            
            pthread_join(child_tid, NULL);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
        }
    }

    /* Dekrementacja licznika */
    sem_lock();
    bus->active_passengers--;
    sem_unlock();

    shmdt(bus);
    return 0;
}
