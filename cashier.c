/*
 * cashier.c
 * 
 * Proces kasjera obsługującego rejestrację i sprzedaż biletów.
 * Nasłuchuje wiadomości od pasażerów i wydaje bilety wszystkim nie-VIP.
 * Działa w trybie blokującym, może być obudzony wake-up message (PID=0).
 */

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

/* Zapis do logu kasjera */
void log_write(const char* s) {
    int fd = open("cashier.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
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

/* Blokada mutexa (dostęp do pamięci dzielonej) */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Odblokowanie mutexa */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

int main() {
    /* Generowanie kluczy IPC */
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t msg_key = ftok(MSG_PATH, 'M');
    key_t sem_key = ftok(SEM_PATH, 'E');

    if (shm_key == -1 || msg_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    /* Podłączenie do istniejących zasobów IPC */
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
    log_main(ln);

    /*
     * Główna pętla obsługi pasażerów.
     * BLOKUJĄCE msgrcv (bez IPC_NOWAIT) - czeka na wiadomości.
     * Main może obudzić wysyłając wake-up message podczas shutdown.
     */
    for (;;) {
        struct msg m;
        
        /* Odbieranie MSG_REGISTER w trybie blokującym */
        ssize_t r = msgrcv(msgid, &m, sizeof(m) - sizeof(long), MSG_REGISTER, 0);

        if (r < 0) {
            if (errno == EINTR) {
                /* Przerwane przez sygnał - kontynuacja */
                continue;
            }
            if (errno == EIDRM) {
                /* Kolejka usunięta - koniec pracy */
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln), "[%s] [KASA] Kolejka usunieta - koniec\n", b);
                log_write(ln);
                log_main(ln);
                break;
            }
            perror("msgrcv");
            break;
        }
        
        /*
         * WAŻNE: Wake-up message (PID=0) sprawdzamy PRZED logowaniem.
         * Main wysyła pustą wiadomość aby przerwać blokujące msgrcv
         * i umożliwić sprawdzenie flagi shutdown.
         */
        if (m.pid == 0) {
            sem_lock();
            int sd = bus->shutdown;
            sem_unlock();
            
            if (sd) {
                /* Shutdown potwierdzony */
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln), "[%s] [KASA] Otrzymano shutdown - koniec pracy\n", b);
                log_write(ln);
                log_main(ln);
                break;
            }
            /* Nie było shutdown - czekamy dalej */
            continue;
        }

        /*
         * Prawdziwa wiadomość od pasażera.
         * Sprawdzamy shutdown PRZED wysłaniem biletu, ale PO odebraniu
         * wiadomości (żeby nie stracić rejestracji).
         */
        sem_lock();
        int sd = bus->shutdown;
        sem_unlock();

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KASA] Rejestracja PID=%d VIP=%d DZIECKO=%d\n",
                 b, m.pid, m.vip, m.child);
        log_write(ln);

        /*
         * Wysyłanie biletu dla nie-VIP.
         * VIP-y już mają bilety i nie potrzebują obsługi.
         * Typ: MSG_TICKET_REPLY + PID (tylko właściwy pasażer odbierze).
         */
        if (!m.vip) {
            m.ticket_ok = 1;
            m.type = MSG_TICKET_REPLY + m.pid;
            
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [KASA] Wysylam bilet dla PID=%d type=%ld\n", 
                     b, m.pid, m.type);
            log_write(ln);
            
            if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
                if (errno == EIDRM) {
                    /* Kolejka usunięta */
                    ts(b, sizeof(b));
                    snprintf(ln, sizeof(ln), "[%s] [KASA] Kolejka usunieta podczas wysylania - koniec\n", b);
                    log_write(ln);
                    log_main(ln);
                    break;
                } else {
                    /* Inny błąd - logujemy ale kontynuujemy */
                    ts(b, sizeof(b));
                    snprintf(ln, sizeof(ln), "[%s] [KASA] BLAD msgsnd biletu dla PID=%d (errno=%d: %s)\n", 
                             b, m.pid, errno, strerror(errno));
                    log_write(ln);
                    log_main(ln);
                }
            } else {
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln), "[%s] [KASA] Wyslano bilet dla PID=%d\n", b, m.pid);
                log_write(ln);
            }
        }
        
        /*
         * Jeśli był shutdown, kończymy DOPIERO PO obsłużeniu pasażera.
         * Każdy zarejestrowany pasażer dostanie odpowiedź.
         */
        if (sd) {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [KASA] Shutdown - koncze po obsluzeniu PID=%d\n", b, m.pid);
            log_write(ln);
            log_main(ln);
            break;
        }
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [KASA] Koniec pracy\n", b);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
