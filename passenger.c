/*
 * passenger.c – Proces pasażera
 *
 * Wiek pasażera losowany z przedziału [1, 80] lat.
 * Pasażer poniżej 8 lat bez opiekuna jest odrzucany natychmiast.
 *
 * Typy pasażerów (dla osób w wieku >= 8 lat):
 *   VIP              ( 1%) – omija kasę
 *   Opiekun+dziecko  (24%) – idzie do kasy, rezerwuje 2 miejsca;
 *                            dziecko = pthread wewnątrz tego procesu
 *   Z rowerem        (25%) – idzie do kasy, potrzebuje miejsca na rower
 *   Zwykły           (50%) – idzie do kasy
 *
 * Synchronizacja IPC:
 *   sem[0]  – mutex ogólny dla shared memory
 *   sem[6]  – sygnał powrotu autobusu (driver postuje 1 raz per dorosły pasażer)
 *
 * Synchronizacja wewnątrz procesu (opiekun ↔ wątek dziecka):
 *   ChildCtx.mtx  – pthread_mutex
 *   ChildCtx.cond – pthread_cond
 *   ChildCtx.boarded   – opiekun wsiadł → budzi dziecko
 *   ChildCtx.trip_done – autobus wrócił → budzi dziecko po raz drugi
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include "ipc.h"

/* =========================================================
 * Globalne zasoby IPC (dostępne też w wątku dziecka)
 * ========================================================= */
static int            shmid, semid, msgid, msgid_reply;
static struct BusState *bus;

/* =========================================================
 * Kontekst współdzielony między wątkiem opiekuna a wątkiem dziecka
 * ========================================================= */
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int      boarded;       /* flaga: opiekun (i dziecko) wsiedli */
    int      trip_done;     /* flaga: autobus wrócił z trasy      */
    pid_t    guardian_pid;
} ChildCtx;

/* =========================================================
 * Narzędzia: timestamp + logowanie
 * ========================================================= */
static void ts(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm *ti = localtime(&t);
    if (!ti) { snprintf(buf, n, "00:00:00"); return; }
    strftime(buf, n, "%H:%M:%S", ti);
}

static void log_write(const char *s)
{
    int fd = open("passenger.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

static void log_main(const char *s)
{
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* =========================================================
 * Semafory IPC
 * ========================================================= */
static void sem_lock(void)
{
    struct sembuf sb = {0, -1, SEM_UNDO};
    while (semop(semid, &sb, 1) == -1 && errno == EINTR)
        ;
}

static void sem_unlock(void)
{
    struct sembuf sb = {0, 1, SEM_UNDO};
    semop(semid, &sb, 1);
}

/* =========================================================
 * Wątek dziecka
 *
 * Życie wątku:
 *  1. Czeka aż opiekun wsiądzie (ctx->boarded)
 *  2. Loguje "wsiadłem razem z opiekunem"
 *  3. Czeka aż autobus wróci (ctx->trip_done)
 *  4. Loguje "wróciłem" i kończy
 * ========================================================= */
static void *child_thread(void *arg)
{
    ChildCtx *ctx = (ChildCtx *)arg;
    char b[64], ln[256];

    /* --- czekamy aż opiekun wsiądzie --- */
    pthread_mutex_lock(&ctx->mtx);
    while (!ctx->boarded)
        pthread_cond_wait(&ctx->cond, &ctx->mtx);
    pthread_mutex_unlock(&ctx->mtx);

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [DZIECKO opiekuna %d] Wsiadlem do autobusu razem z opiekunem\n",
             b, (int)ctx->guardian_pid);
    log_write(ln);
    log_main(ln);

    /* --- czekamy na powrót autobusu --- */
    pthread_mutex_lock(&ctx->mtx);
    while (!ctx->trip_done)
        pthread_cond_wait(&ctx->cond, &ctx->mtx);
    pthread_mutex_unlock(&ctx->mtx);

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [DZIECKO opiekuna %d] Wrocil z trasy - koniec\n",
             b, (int)ctx->guardian_pid);
    log_write(ln);
    log_main(ln);

    return NULL;
}

/* =========================================================
 * Czekanie na powrót autobusu (dla głównego wątku / opiekuna)
 *
 * Driver po powrocie z trasy:
 *   1. (pod mutexem) Ustawia passenger_trip_completed[pid] = 1
 *   2. Robi semop(sem[6], +1) dla każdego dorosłego pasażera
 *
 * Pasażer zawsze najpierw konsumuje token (semop -1),
 * dopiero potem sprawdza flagę – zapobiega kumulacji tokenów.
 * ========================================================= */
static void wait_for_trip_end(pid_t my_pid)
{
    char b[64], ln[256];

    for (;;) {
        struct sembuf sb_wait = {6, -1, 0};
        if (semop(semid, &sb_wait, 1) == -1) {
            if (errno == EINTR) continue;
            /* EIDRM / EINVAL – IPC usunięte, wychodzimy */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] IPC usuniete podczas czekania - koniec\n",
                     b, (int)my_pid);
            log_write(ln);
            return;
        }

        /* Sprawdź czy token jest nasz */
        if (my_pid > 0 && my_pid < MAX_PID
            && bus->passenger_trip_completed[my_pid]) {
            bus->passenger_trip_completed[my_pid] = 0;
            return;
        }

        /* Nie nasz – oddaj i czekaj dalej */
        struct sembuf sb_ret = {6, 1, 0};
        semop(semid, &sb_ret, 1);
        sleep(1);
    }
}

/* =========================================================
 * MAIN
 * ========================================================= */
int main(void)
{
    char b[64], ln[512];

    /* --- Połącz z IPC --- */
    key_t shm_key     = ftok(SHM_PATH,       'S');
    key_t sem_key     = ftok(SEM_PATH,        'E');
    key_t msg_key     = ftok(MSG_PATH,        'M');
    key_t msg_rpl_key = ftok(MSG_REPLY_PATH,  'R');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1 || msg_rpl_key == -1) {
        perror("ftok passenger");
        return 1;
    }

    shmid       = shmget(shm_key,  sizeof(struct BusState), 0600);
    semid       = semget(sem_key,  0, 0600);   /* nsems=0: istniejący zestaw */
    msgid       = msgget(msg_key,  0600);
    msgid_reply = msgget(msg_rpl_key, 0600);

    if (shmid == -1 || semid == -1 || msgid == -1 || msgid_reply == -1) {
        perror("get ipc passenger");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void *)-1) { perror("shmat passenger"); return 1; }

    pid_t my_pid = getpid();
    srand((unsigned)(my_pid ^ (unsigned)time(NULL)));
    ts(b, sizeof(b));

    /* ===== 1. Sprawdź czy dworzec jest otwarty ===== */
    sem_lock();
    int sd       = bus->shutdown;
    int sblocked = bus->station_blocked;
    sem_unlock();

    if (sd || sblocked) {
        sem_lock(); bus->total_station_blocked++; sem_unlock();
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Dworzec zamkniety - odchodzi\n", b, (int)my_pid);
        log_write(ln);
        shmdt(bus);
        return 0;
    }

    /* ===== 2. Losuj wiek i typ pasażera ===== */
    /*
     * Wiek: 1–80 lat (jednostajny rozkład).
     * Pasażer poniżej 8 lat = dziecko bez opiekuna → odrzucany natychmiast.
     *
     * Dla pozostałych (wiek >= 8):
     *   r % 100:
     *    0        → VIP       ( 1%)
     *    1–24     → opiekun   (24%)
     *    25–49    → rowerzysta(25%)
     *    50–99    → zwykły    (50%)
     */
    int age           = 1 + rand() % 80;
    int is_lone_child = (age < 8);       /* wiek < 8 → brak opiekuna */

    int is_vip      = 0;
    int is_guardian = 0;
    int has_bike    = 0;

    if (!is_lone_child) {
        int r   = rand() % 100;
        is_vip      = (r == 0);                                  /*  1% */
        is_guardian = (!is_vip && r <= 24);                      /* 24% */
        has_bike    = (!is_vip && !is_guardian && r <= 49);      /* 25% */
        /* pozostałe 50% = zwykły */
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] Start: WIEK=%d VIP=%d SAM_DZIECKO=%d ROWER=%d OPIEKUN=%d\n",
             b, (int)my_pid, age, is_vip, is_lone_child, has_bike, is_guardian);
    log_write(ln);

    /* ===== 3. Liczniki typów ===== */
    sem_lock();
    if (is_vip) {
        bus->total_vip++;
    } else {
        bus->total_non_vip++;
        if (has_bike) bus->total_bikes++;
    }
    sem_unlock();

    /* ===== 4. Dziecko bez opiekuna – odrzucamy od razu ===== */
    if (is_lone_child) {
        sem_lock(); bus->total_children_without_guardian++; sem_unlock();
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Dziecko bez opiekuna (wiek=%d lat) - odrzucony\n",
                 b, (int)my_pid, age);
        log_write(ln);
        log_main(ln);
        shmdt(bus);
        return 0;
    }

    /* ===== 5. Opiekun: przygotuj wątek dziecka ===== */
    pthread_t child_tid = 0;
    ChildCtx  ctx;
    if (is_guardian) {
        pthread_mutex_init(&ctx.mtx,  NULL);
        pthread_cond_init (&ctx.cond, NULL);
        ctx.boarded      = 0;
        ctx.trip_done    = 0;
        ctx.guardian_pid = my_pid;

        if (pthread_create(&child_tid, NULL, child_thread, &ctx) != 0) {
            perror("pthread_create child");
            /* Nie udało się stworzyć wątku – degradujemy do zwykłego pasażera */
            is_guardian = 0;
        } else {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] Opiekun: watek dziecka uruchomiony\n",
                     b, (int)my_pid);
            log_write(ln);
        }
    }

    /* ===== 6. Kasa (tylko nie-VIP) ===== */
    if (!is_vip) {
        sem_lock();
        sd       = bus->shutdown;
        sblocked = bus->station_blocked;
        if (sd || sblocked) {
            bus->total_station_blocked++;
            sem_unlock();
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] Dworzec zamkniety przed kasa - odchodzi\n",
                     b, (int)my_pid);
            log_write(ln);
            /* Zakończ wątek dziecka jeśli istnieje */
            if (is_guardian && child_tid) {
                pthread_mutex_lock(&ctx.mtx);
                ctx.boarded = 1;  /* odblokuj wątek... */
                ctx.trip_done = 1;
                pthread_cond_broadcast(&ctx.cond);
                pthread_mutex_unlock(&ctx.mtx);
                pthread_join(child_tid, NULL);
                pthread_mutex_destroy(&ctx.mtx);
                pthread_cond_destroy(&ctx.cond);
            }
            shmdt(bus);
            return 0;
        }
        bus->total_sent_to_cashier++;
        sem_unlock();

        /* Wyślij rejestrację do kasy */
        struct msg m;
        memset(&m, 0, sizeof(m));
        m.type  = MSG_REGISTER;
        m.pid   = my_pid;
        m.vip   = 0;
        m.bike  = has_bike;
        m.child = is_guardian;

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Ide do kasy (ROWER=%d OPIEKUN=%d)\n",
                 b, (int)my_pid, has_bike, is_guardian);
        log_write(ln);

        if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
            if (errno != EIDRM) perror("msgsnd register");
            if (is_guardian && child_tid) {
                pthread_mutex_lock(&ctx.mtx);
                ctx.boarded = 1; ctx.trip_done = 1;
                pthread_cond_broadcast(&ctx.cond);
                pthread_mutex_unlock(&ctx.mtx);
                pthread_join(child_tid, NULL);
                pthread_mutex_destroy(&ctx.mtx);
                pthread_cond_destroy(&ctx.cond);
            }
            shmdt(bus);
            return 0;
        }

        /* Odbierz bilet z kolejki odpowiedzi */
        long   reply_type = MSG_TICKET_REPLY + (long)my_pid;
        ssize_t rcv;
        do {
            rcv = msgrcv(msgid_reply, &m, sizeof(m) - sizeof(long), reply_type, 0);
        } while (rcv < 0 && errno == EINTR);

        if (rcv < 0 || !m.ticket_ok) {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] Brak biletu / blad kasy - odchodzi\n",
                     b, (int)my_pid);
            log_write(ln);
            if (is_guardian && child_tid) {
                pthread_mutex_lock(&ctx.mtx);
                ctx.boarded = 1; ctx.trip_done = 1;
                pthread_cond_broadcast(&ctx.cond);
                pthread_mutex_unlock(&ctx.mtx);
                pthread_join(child_tid, NULL);
                pthread_mutex_destroy(&ctx.mtx);
                pthread_cond_destroy(&ctx.cond);
            }
            shmdt(bus);
            return 0;
        }

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Otrzymal bilet - ide na przystanek\n",
                 b, (int)my_pid);
        log_write(ln);

    } else {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] VIP - omijam kase\n", b, (int)my_pid);
        log_write(ln);
    }

    /* ===== 7. Wsiadanie do autobusu ===== */
    /*
     * Opiekun rezerwuje 2 miejsca atomowo pod mutexem.
     * Dziecko NIE próbuje wejść samodzielnie – wchodzi logicznie
     * razem z opiekunem, a fizycznie jest powiadamiane przez ctx.boarded.
     */
    int boarded = 0;
    int needed  = is_guardian ? 2 : 1;

    while (!boarded) {
        sem_lock();
        sd       = bus->shutdown;
        sblocked = bus->station_blocked;

        if (sd || sblocked) {
            sem_unlock();
            break;
        }

        if (bus->driver_pid != 0 && !bus->departing) {
            int free_seats = bus->P   - bus->passengers;
            int bike_ok    = !has_bike || (bus->bikes < bus->R);
            int list_ok    = (bus->passenger_count + needed) <= MAX_BUS_CAPACITY;

            if (free_seats >= needed && bike_ok && list_ok) {
                /* Rezerwujemy miejsca */
                bus->passengers += needed;
                if (has_bike) bus->bikes++;

                /* Wpisujemy opiekuna na listę */
                bus->passenger_list[bus->passenger_count++] = my_pid;

                /* Dziecko jako ujemny PID – driver pominie go przy sem[6] */
                if (is_guardian) {
                    bus->passenger_list[bus->passenger_count++] = -my_pid;
                    bus->total_children_with_guardian++;
                }

                boarded = 1;
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln),
                         "[%s] [PASAZER %d] Wsiadl (miejsca: %d/%d row: %d/%d opiekun=%d)\n",
                         b, (int)my_pid,
                         bus->passengers, bus->P, bus->bikes, bus->R, is_guardian);
                log_write(ln);
                log_main(ln);
                sem_unlock();

                /* Powiadom wątek dziecka że oboje wsiedli */
                if (is_guardian && child_tid) {
                    pthread_mutex_lock(&ctx.mtx);
                    ctx.boarded = 1;
                    pthread_cond_broadcast(&ctx.cond);
                    pthread_mutex_unlock(&ctx.mtx);
                }
            } else {
                sem_unlock();
                sleep(1);
            }
        } else {
            sem_unlock();
            sleep(1);
        }
    }

    if (!boarded) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Nie wsiadt – shutdown/blokada\n", b, (int)my_pid);
        log_write(ln);
        /* Zakończ wątek dziecka bezpiecznie */
        if (is_guardian && child_tid) {
            pthread_mutex_lock(&ctx.mtx);
            ctx.boarded = 1; ctx.trip_done = 1;
            pthread_cond_broadcast(&ctx.cond);
            pthread_mutex_unlock(&ctx.mtx);
            pthread_join(child_tid, NULL);
            pthread_mutex_destroy(&ctx.mtx);
            pthread_cond_destroy(&ctx.cond);
        }
        shmdt(bus);
        return 0;
    }

    /* ===== 8. Czekanie na powrót autobusu ===== */
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] W autobusie - czeka na powrot\n", b, (int)my_pid);
    log_write(ln);

    wait_for_trip_end(my_pid);

    /* Powiadom wątek dziecka że autobus wrócił */
    if (is_guardian && child_tid) {
        pthread_mutex_lock(&ctx.mtx);
        ctx.trip_done = 1;
        pthread_cond_broadcast(&ctx.cond);
        pthread_mutex_unlock(&ctx.mtx);
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] Wrocil z trasy - koniec pracy\n", b, (int)my_pid);
    log_write(ln);
    log_main(ln);

    /* Czekaj na zakończenie wątku dziecka przed zwolnieniem zasobów */
    if (is_guardian && child_tid) {
        pthread_join(child_tid, NULL);
        pthread_mutex_destroy(&ctx.mtx);
        pthread_cond_destroy(&ctx.cond);
    }

    shmdt(bus);
    return 0;
}
