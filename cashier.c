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

int shmid, msgid, msgid_reply, semid;
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
    int fd = open("cashier.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
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
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

int main() {
    key_t shm_key     = ftok(SHM_PATH, 'S');
    key_t msg_key     = ftok(MSG_PATH, 'M');
    key_t msg_rpl_key = ftok(MSG_REPLY_PATH, 'R');
    key_t sem_key     = ftok(SEM_PATH, 'E');

    if (shm_key == -1 || msg_key == -1 || msg_rpl_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    shmid      = shmget(shm_key, sizeof(struct BusState), 0600);
    msgid      = msgget(msg_key, 0600);      /* kolejka żądań (odbiór) */
    msgid_reply= msgget(msg_rpl_key, 0600);  /* kolejka odpowiedzi (wysyłanie biletów) */
    semid      = semget(sem_key, 7, 0600);

    if (shmid == -1 || msgid == -1 || msgid_reply == -1 || semid == -1) {
        perror("get ipc cashier");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    sleep(20);//sleep do testow

    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [KASA] Start pracy\n", b);
    log_write(ln);
    log_main(ln);

    for (;;) {
        struct msg m;
        
        // ZAWSZE używamy blokującego msgrcv (BEZ IPC_NOWAIT)
        // Main wyśle nam wake-up message gdy będzie shutdown
        ssize_t r = msgrcv(msgid, &m, sizeof(m) - sizeof(long), MSG_REGISTER, 0);

        if (r < 0) {
            if (errno == EINTR) {
                // Przerwane przez sygnał - kontynuuj
                continue;
            }
            if (errno == EIDRM) {
                // Kolejka została usunięta - kończymy
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln), "[%s] [KASA] Kolejka usunieta - koniec\n", b);
                log_write(ln);
                log_main(ln);
                break;
            }
            perror("msgrcv");
            break;
        }
        
        // WAŻNE: Wake-up message (PID=0) sprawdzamy PRZED logowaniem
        if (m.pid == 0) {
            // To był wake-up message do przebudzenia
            sem_lock();
            int sd = bus->shutdown;
            sem_unlock();
            
            if (sd) {
                // Shutdown - kończymy pracę
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln), "[%s] [KASA] Otrzymano shutdown - koniec pracy\n", b);
                log_write(ln);
                log_main(ln);
                break;
            }
            // Nie było shutdown - czekamy dalej
            continue;
        }

        // Sprawdzamy shutdown PO otrzymaniu prawdziwego pasażera
        // (żeby nie stracić jego wiadomości)
        sem_lock();
        int sd = bus->shutdown;
        sem_unlock();

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KASA] Rejestracja PID=%d VIP=%d DZIECKO=%d\n",
                 b, m.pid, m.vip, m.child);
        log_write(ln);

        // Wysyłamy bilet dla WSZYSTKICH nie-VIP pasażerów
        if (!m.vip) {
            // ========== INKREMENTACJA LICZNIKA: KASA OBSŁUŻYŁA ==========
            sem_lock();
            bus->cashier_processed++;
            sem_unlock();
            
            m.ticket_ok = 1;
            m.type = MSG_TICKET_REPLY + m.pid;
            
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [KASA] Wysylam bilet dla PID=%d type=%ld\n", 
                     b, m.pid, m.type);
            log_write(ln);
            
            if (msgsnd(msgid_reply, &m, sizeof(m) - sizeof(long), 0) == -1) {
                if (errno == EIDRM) {
                    ts(b, sizeof(b));
                    snprintf(ln, sizeof(ln), "[%s] [KASA] Kolejka usunieta podczas wysylania - koniec\n", b);
                    log_write(ln);
                    log_main(ln);
                    break;
                } else {
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
        
        // Jeśli był shutdown, kończymy PO obsłużeniu tego pasażera
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
