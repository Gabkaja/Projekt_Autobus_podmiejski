/*
 * cashier.c – Proces kasjera.
 *
 * Kasjer oczekuje na żądania rejestracji od pasażerów (kolejka MSG_PATH, typ MSG_REGISTER),
 * a następnie odsyła każdemu bilet przez kolejkę odpowiedzi (MSG_REPLY_PATH).
 * Pasażerowie VIP nie wysyłają żądań do kasjera – omijają kasę całkowicie.
 *
 * Cykl życia:
 *   1. Podłączenie do istniejących zasobów IPC (main je tworzy przed forkiem).
 *   2. Pętla główna: blokujące oczekiwanie na kolejne żądanie rejestracji.
 *   3. Przy wiadomości wake-up (pid=0): sprawdzenie flagi shutdown.
 *      Jeśli shutdown – drenaż kolejki żądań (obsługa pasażerów, którzy zdążyli
 *      nadejść przed zamknięciem), następnie zakończenie pracy.
 *   4. Przy normalnym żądaniu: inkrementacja licznika cashier_processed,
 *      ustawienie ticket_ok=1 i odesłanie wiadomości do kolejki odpowiedzi.
 *
 * Obsługa sygnałów:
 *   Kasjer ignoruje SIGINT, SIGUSR1, SIGUSR2 i SIGHUP.
 *   Wyłącza się wyłącznie przez wake-up message od main, co zapobiega
 *   sytuacji, w której kasjer ginie przed pasażerami, którzy już wysłali
 *   żądanie i czekają na bilet.
 */

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

/* Globalne identyfikatory IPC */
int shmid, msgid, msgid_reply, semid;
struct BusState* bus;

/* =========================================================
 * Funkcje pomocnicze: timestamp i logowanie
 * ========================================================= */

/*
 * ts – Formatuje bieżącą godzinę do bufora w formacie HH:MM:SS.
 */
void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

/*
 * log_write – Dopisuje wpis do dziennika kasjera (cashier.log).
 */
void log_write(const char* s) {
    int fd = open("cashier.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/*
 * log_main – Dopisuje wpis do raportu zbiorczego (report.txt).
 */
void log_main(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* =========================================================
 * Operacje na semaforze mutex (sem[0])
 * ========================================================= */

/*
 * sem_lock – Opuszcza semafor mutex (sem[0]).
 * Pętla powtarza operację przy EINTR (przerwanie przez sygnał),
 * aby mutex zawsze został wzięty przed dalszym działaniem.
 * Błędy EIDRM/EINVAL oznaczają, że IPC zostało usunięte – kończymy.
 */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR)             continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

/*
 * sem_unlock – Podnosi semafor mutex (sem[0]).
 */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* =========================================================
 * Funkcja główna kasjera
 * ========================================================= */

int main() {
    /* Generowanie kluczy IPC z istniejących plików-tokenów */
    key_t shm_key     = ftok(SHM_PATH,       'S');
    key_t msg_key     = ftok(MSG_PATH,        'M');
    key_t msg_rpl_key = ftok(MSG_REPLY_PATH,  'R');
    key_t sem_key     = ftok(SEM_PATH,        'E');

    if (shm_key == -1 || msg_key == -1 || msg_rpl_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    /* Podłączenie do istniejących zasobów IPC (tworzonych przez main) */
    shmid       = shmget(shm_key,     sizeof(struct BusState), 0600);
    msgid       = msgget(msg_key,     0600);     /* kolejka żądań – odbiór  */
    msgid_reply = msgget(msg_rpl_key, 0600);     /* kolejka odpowiedzi – wysyłanie biletów */
    semid       = semget(sem_key,     6,    0600);

    if (shmid == -1 || msgid == -1 || msgid_reply == -1 || semid == -1) {
        perror("get ipc cashier");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    /*
     * Kasjer ignoruje wszystkie sygnały zewnętrzne.
     * Wyłącznie wake-up message (pid=0) od main decyduje o zakończeniu.
     * Bez tego: Ctrl+C dociera do całej grupy procesów równocześnie,
     * kasjer ginie natychmiast i pasażerowie, którzy już wysłali żądanie,
     * zawieszają się na msgrcv w oczekiwaniu na bilet.
     */
    signal(SIGINT,  SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);

    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [KASA] Start pracy\n", b);
    log_write(ln);
    log_main(ln);

    /* =========================================================
     * Pętla główna: obsługa żądań rejestracji
     * ========================================================= */
    for (;;) {
        struct msg m;

        /*
         * Blokujące oczekiwanie na żądanie o typie MSG_REGISTER.
         * Main wyśle wiadomość wake-up (pid=0) gdy nastąpi shutdown,
         * co odblokuje to wywołanie i umożliwi kasjerowi zakończenie pracy.
         */
        ssize_t r = msgrcv(msgid, &m, sizeof(m) - sizeof(long), MSG_REGISTER, 0);

        if (r < 0) {
            if (errno == EINTR) continue;
            /* Kolejka usunięta przez cleanup – zakończymy normalnie */
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

        /* -------------------------------------------------------
         * Obsługa wiadomości wake-up (pid=0) – sygnał od main.
         * ------------------------------------------------------- */
        if (m.pid == 0) {
            sem_lock();
            int sd = bus->shutdown;
            sem_unlock();

            /* Fałszywy alarm (shutdown jeszcze nie ustawiony) – czekaj dalej */
            if (!sd) continue;

            /*
             * Potwierdzono shutdown.
             * Drenaż kolejki: obsługujemy wszystkich pasażerów, którzy zdążyli
             * wysłać żądanie przed zamknięciem systemu. Bez tego etapu pasażerowie
             * ci zawisliby na msgrcv oczekując na bilet, który nigdy nie nadejdzie.
             * Używamy IPC_NOWAIT – nie blokujemy się, gdy kolejka jest pusta.
             */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [KASA] Shutdown – drenaż kolejki zadan...\n", b);
            log_write(ln);
            log_main(ln);

            for (;;) {
                struct msg dm;
                ssize_t dr = msgrcv(msgid, &dm, sizeof(dm) - sizeof(long),
                                    MSG_REGISTER, IPC_NOWAIT);
                if (dr < 0) {
                    if (errno == ENOMSG)                     break;         /* kolejka pusta */
                    if (errno == EIDRM || errno == EINVAL)  goto cashier_done;
                    break;
                }
                if (dm.pid == 0) continue;   /* kolejny wake-up – ignorujemy */

                /* Obsługujemy pasażera: wystawiamy bilet przez kolejkę odpowiedzi */
                if (!dm.vip) {
                    sem_lock();
                    bus->cashier_processed++;
                    sem_unlock();

                    dm.ticket_ok = 1;
                    dm.type = MSG_TICKET_REPLY + dm.pid;
                    if (msgsnd(msgid_reply, &dm, sizeof(dm) - sizeof(long), IPC_NOWAIT) == -1) {
                        if (errno == EIDRM || errno == EINVAL) goto cashier_done;
                        /* EAGAIN: kolejka odpowiedzi pełna – pasażer wyjdzie przez EIDRM podczas cleanup */
                    }
                }
            }

            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [KASA] Drenaż zakończony – koniec pracy\n", b);
            log_write(ln);
            log_main(ln);
            break;
        }

        /* -------------------------------------------------------
         * Normalny pasażer: wystawienie biletu i odesłanie odpowiedzi.
         * Pasażerowie VIP nie docierają do kasjera – ten warunek jest
         * defensywny na wypadek niepoprawnego użycia kolejki.
         * ------------------------------------------------------- */
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KASA] Rejestracja PID=%d VIP=%d DZIECKO=%d\n",
                 b, m.pid, m.vip, m.child);
        log_write(ln);

        if (!m.vip) {
            /* Inkrementacja licznika pod mutexem – zapis do pamięci dzielonej */
            sem_lock();
            bus->cashier_processed++;
            sem_unlock();

            m.ticket_ok = 1;
            /* Typ wiadomości zakodowany jako base + PID, aby pasażer odebrał
             * wyłącznie własny bilet i nie kolidował z biletami innych. */
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
                snprintf(ln, sizeof(ln), "[%s] [KASA] Wyslano bilet dla PID=%d\n", b, m.pid);
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
