#include <stdio.h>
#include <unistd.h>
#include <signal.h>
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

    /* Kasjer ignoruje sygnały zewnętrzne – kończy pracę wyłącznie przez
     * wake-up message (pid=0) wysłany przez main podczas shutdown.
     * Bez tego: Ctrl+C trafia do całej grupy procesów jednocześnie,
     * kasjer ginie natychmiast i zostaje zombie zanim main zdąży wait(). */
    signal(SIGINT,  SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
   
    //sleep(10);
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [KASA] Start pracy\n", b);
    log_write(ln);
    log_main(ln);

    for (;;) {
        struct msg m;

        /* Blokujące oczekiwanie na żądanie rejestracji.
         * Main wyśle wake-up (pid=0) gdy nadejdzie shutdown. */
        ssize_t r = msgrcv(msgid, &m, sizeof(m) - sizeof(long), MSG_REGISTER, 0);

        if (r < 0) {
            if (errno == EINTR)  continue;
            if (errno == EIDRM || errno == EINVAL) {
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln), "[%s] [KASA] Kolejka usunieta - koniec\n", b);
                log_write(ln);
                log_main(ln);
                break;
            }
            perror("msgrcv");
            break;
        }

        /* --- Wake-up message (pid=0) oznacza shutdown --- */
        if (m.pid == 0) {
            sem_lock();
            int sd = bus->shutdown;
            sem_unlock();

            if (!sd) continue;   /* fałszywy alarm – czekaj dalej */

            /* Shutdown potwierdzony.
             * KLUCZOWE: zanim wyjdziemy, opróżniamy całą kolejkę żądań
             * przez IPC_NOWAIT. Dzięki temu każdy pasażer który zdążył
             * wysłać MSG_REGISTER PRZED shutdownem dostanie swój bilet
             * i wyjdzie normalnie zamiast wisieć na msgrcv(msgid_reply).
             * Bez tego drenażu mielibyśmy: total_sent_to_cashier >> cashier_processed. */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [KASA] Shutdown – drenaż kolejki zadan...\n", b);
            log_write(ln);
            log_main(ln);

            for (;;) {
                struct msg dm;
                ssize_t dr = msgrcv(msgid, &dm, sizeof(dm) - sizeof(long),
                                    MSG_REGISTER, IPC_NOWAIT);
                if (dr < 0) {
                    if (errno == ENOMSG) break;      /* kolejka pusta – koniec */
                    if (errno == EIDRM || errno == EINVAL) goto cashier_done;
                    break;
                }
                if (dm.pid == 0) continue;           /* kolejny wake-up – ignoruj */

                if (!dm.vip) {
                    sem_lock();
                    bus->cashier_processed++;
                    sem_unlock();

                    dm.ticket_ok = 1;
                    dm.type = MSG_TICKET_REPLY + dm.pid;
                    if (msgsnd(msgid_reply, &dm, sizeof(dm) - sizeof(long),
                               IPC_NOWAIT) == -1) {
                        if (errno == EIDRM || errno == EINVAL) goto cashier_done;
                        /* EAGAIN = kolejka odpowiedzi pełna – pasażer i tak
                         * zaraz wyjdzie przez EIDRM przy cleanup(), nie tracimy go */
                    }
                }
            }

            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [KASA] Drenaż zakończony – koniec pracy\n", b);
            log_write(ln);
            log_main(ln);
            break;
        }

        /* --- Normalny pasażer --- */
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KASA] Rejestracja PID=%d VIP=%d DZIECKO=%d\n",
                 b, m.pid, m.vip, m.child);
        log_write(ln);

        if (!m.vip) {
            sem_lock();
            bus->cashier_processed++;
            sem_unlock();

            m.ticket_ok = 1;
            m.type = MSG_TICKET_REPLY + m.pid;

            if (msgsnd(msgid_reply, &m, sizeof(m) - sizeof(long), 0) == -1) {
                if (errno == EIDRM || errno == EINVAL) {
                    ts(b, sizeof(b));
                    snprintf(ln, sizeof(ln),
                             "[%s] [KASA] IPC usuniete podczas wysylania biletu – koniec\n", b);
                    log_write(ln);
                    log_main(ln);
                    goto cashier_done;
                }
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln),
                         "[%s] [KASA] BLAD msgsnd biletu dla PID=%d (errno=%d: %s)\n",
                         b, m.pid, errno, strerror(errno));
                log_write(ln);
                log_main(ln);
            } else {
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln), "[%s] [KASA] Wyslano bilet dla PID=%d\n",
                         b, m.pid);
                log_write(ln);
            }
        }
    }

cashier_done:

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [KASA] Koniec pracy\n", b);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
