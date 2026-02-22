/*
 * passenger.c – Proces pasażera.
 *
 * Każdy pasażer jest osobnym procesem, tworzonym przez passenger_generator.
 * Po uruchomieniu pasażer losuje wiek i typ, a następnie przechodzi przez
 * kolejne etapy symulacji.
 *
 * Etapy życia pasażera:
 *   1. Sprawdzenie, czy dworzec jest otwarty (flagi shutdown/station_blocked).
 *   2. Losowanie wieku (1–80 lat) i typu:
 *        - VIP       ( 1%) – omija kasę i wchodzi bezpośrednio do autobusu.
 *        - Opiekun   (24%) – rejestruje się w kasie i rezerwuje 2 miejsca;
 *                            dziecko jest reprezentowane przez wewnętrzny wątek.
 *        - Rowerzysta(25%) – rejestruje się w kasie; do autobusu wchodzi
 *                            przez bramkę sem[2] (rowerową).
 *        - Zwykły    (50%) – rejestruje się w kasie; bramka sem[1].
 *        - Dziecko    (<8 lat bez opiekuna) – odrzucany natychmiast.
 *   3. Rejestracja w kasie: wysłanie MSG_REGISTER i oczekiwanie na bilet.
 *   4. Wsiadanie do autobusu: blokowanie na odpowiedniej bramce (sem[1] lub sem[2]),
 *      sprawdzenie dostępności miejsca pod mutexem sem[0], rezerwacja miejsca.
 *   5. Oczekiwanie na powrót autobusu: semop(sem[5], -1) + flaga passenger_trip_completed.
 *   6. Zakończenie.
 *
 * Wątek dziecka (child_thread):
 *   Tworzony wewnątrz procesu opiekuna przez pthread_create.
 *   Czeka na sygnał ctx.boarded (opiekun wsiadł), następnie na ctx.trip_done (autobus wrócił).
 *   Nie korzysta z IPC bezpośrednio – koordynacja przez pthread_mutex i pthread_cond.
 *
 * Kolejność blokowania semaforów (zapobieganie zakleszczeniom):
 *   Zawsze: bramka (sem[1] lub sem[2]) PRZED mutexem (sem[0]).
 *   Musi być spójna z kolejnością w driver.c: sem[1] → sem[2] → sem[0].
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

/* Globalne zasoby IPC – dostępne zarówno w wątku głównym, jak i w wątku dziecka */
static int             shmid, semid, msgid, msgid_reply;
static struct BusState *bus;

/* =========================================================
 * Kontekst współdzielony między wątkiem opiekuna a wątkiem dziecka.
 * Synchronizacja przez pthread_mutex + pthread_cond, gdyż wątki
 * należą do tego samego procesu i nie mogą używać semaforów IPC
 * bez ryzyka zakłócenia globalnych liczników.
 * ========================================================= */
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int      boarded;       /* Flaga: opiekun wsiadł do autobusu (budzi wątek dziecka po raz pierwszy)  */
    int      trip_done;     /* Flaga: autobus wrócił z trasy (budzi wątek dziecka po raz drugi)         */
    pid_t    guardian_pid;  /* PID opiekuna – używany wyłącznie do logowania                            */
} ChildCtx;

/* =========================================================
 * Funkcje pomocnicze: timestamp i logowanie
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
 * Operacje na semaforze mutex (sem[0])
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
 * Cykl życia:
 *   1. Oczekiwanie na ctx->boarded – opiekun wsiadł, dziecko wsiadło razem z nim.
 *   2. Logowanie faktu wsiadania.
 *   3. Oczekiwanie na ctx->trip_done – autobus wrócił z trasy.
 *   4. Logowanie powrotu i zakończenie wątku.
 * ========================================================= */
static void *child_thread(void *arg)
{
    ChildCtx *ctx = (ChildCtx *)arg;
    char b[64], ln[256];

    /* Oczekiwanie na wsiadanie opiekuna */
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

    /* Oczekiwanie na powrót autobusu z trasy */
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
 * Operacje na bramkach pasażerskich
 *
 * sem[1] – bramka dla pasażerów bez roweru.
 * sem[2] – bramka dla pasażerów z rowerem.
 *
 * Wzięcie bramki serializuje wejście pasażerów danego rodzaju do autobusu.
 * Kolejność: najpierw bramka, dopiero potem mutex ogólny (sem[0]).
 * Odwrócenie kolejności mogłoby spowodować zakleszczenie z driver.c,
 * który blokuje bramki pod swoim mutexem przy zamykaniu drzwi.
 * ========================================================= */
static int gate_lock(int g)
{
    struct sembuf sb = {(unsigned short)g, -1, SEM_UNDO};
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR)              continue;
        if (errno == EIDRM || errno == EINVAL) return -1;  /* IPC usunięte */
        return -1;
    }
    return 0;
}

static void gate_unlock(int g)
{
    struct sembuf sb = {(unsigned short)g, 1, SEM_UNDO};
    semop(semid, &sb, 1);
}

/* =========================================================
 * Oczekiwanie na powrót autobusu
 *
 * Protokół:
 *   1. Konsumuj jeden token z sem[5] (semop -1).
 *   2. Sprawdź, czy flaga passenger_trip_completed[my_pid] jest ustawiona.
 *   3. Jeśli tak – wyzeruj flagę i wyjdź.
 *   4. Jeśli nie – oddaj token (semop +1) i poczekaj sekundę, następnie wróć do kroku 1.
 *
 * Kierowca po powrocie:
 *   a. Ustawia passenger_trip_completed[pid] = 1 pod mutexem.
 *   b. Podnosi sem[5] dla każdego dorosłego pasażera.
 *
 * Pasażer zawsze najpierw konsumuje token, dopiero potem sprawdza flagę,
 * co eliminuje wyścig między sprawdzeniem flagi a podniesieniem semafora.
 * ========================================================= */
static void wait_for_trip_end(pid_t my_pid)
{
    char b[64], ln[256];

    for (;;) {
        struct sembuf sb_wait = {5, -1, 0};
        if (semop(semid, &sb_wait, 1) == -1) {
            if (errno == EINTR) continue;
            /* Usunięcie IPC (EIDRM/EINVAL) – system jest zamykany, kończymy oczekiwanie */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] IPC usuniete podczas czekania - koniec\n",
                     b, (int)my_pid);
            log_write(ln);
            return;
        }

        /* Weryfikacja, czy odebrany token należy do tego pasażera */
        if (my_pid > 0 && my_pid < MAX_PID
            && bus->passenger_trip_completed[my_pid]) {
            bus->passenger_trip_completed[my_pid] = 0;
            return;
        }

        /* Token nie był przeznaczony dla tego pasażera – oddaj go i zaczekaj */
        struct sembuf sb_ret = {5, 1, 0};
        semop(semid, &sb_ret, 1);
        sleep(1);
    }
}

/* =========================================================
 * Funkcja główna pasażera
 * ========================================================= */
int main(void)
{
    char b[64], ln[512];

    /* Podłączenie do istniejących zasobów IPC */
    key_t shm_key     = ftok(SHM_PATH,      'S');
    key_t sem_key     = ftok(SEM_PATH,       'E');
    key_t msg_key     = ftok(MSG_PATH,       'M');
    key_t msg_rpl_key = ftok(MSG_REPLY_PATH, 'R');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1 || msg_rpl_key == -1) {
        perror("ftok passenger");
        return 1;
    }

    shmid       = shmget(shm_key,  sizeof(struct BusState), 0600);
    semid       = semget(sem_key,  0, 0600);   /* nsems=0: podłącz do istniejącego zestawu */
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

    /* ===== Etap 1: Sprawdzenie dostępności dworca ===== */
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

    /* ===== Etap 2: Losowanie wieku i typu pasażera =====
     *
     * Wiek: rozkład jednostajny 1–80 lat.
     * Pasażer poniżej 8 lat = dziecko bez opiekuna → odrzucany natychmiast.
     *
     * Dla pasażerów w wieku >= 8 lat:
     *   r % 100 == 0         → VIP       ( 1%)
     *   r % 100 w [1..24]    → Opiekun   (24%)
     *   r % 100 w [25..49]   → Rowerzysta(25%)
     *   r % 100 w [50..99]   → Zwykły    (50%)
     */
    int age           = 1 + rand() % 80;
    int is_lone_child = (age < 8);

    int is_vip      = 0;
    int is_guardian = 0;
    int has_bike    = 0;

    if (!is_lone_child) {
        int r   = rand() % 100;
        is_vip      = (r == 0);
        is_guardian = (!is_vip && r <= 24);
        has_bike    = (!is_vip && !is_guardian && r <= 49);
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] Start: WIEK=%d VIP=%d SAM_DZIECKO=%d ROWER=%d OPIEKUN=%d\n",
             b, (int)my_pid, age, is_vip, is_lone_child, has_bike, is_guardian);
    log_write(ln);

    /* ===== Etap 3: Aktualizacja liczników typów pasażerów ===== */
    sem_lock();
    if (is_vip) {
        bus->total_vip++;
    } else {
        bus->total_non_vip++;
        if (has_bike) bus->total_bikes++;
    }
    sem_unlock();

    /* ===== Etap 4: Odrzucenie dziecka bez opiekuna ===== */
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

    /* ===== Etap 5: Uruchomienie wątku dziecka (tylko dla opiekuna) ===== */
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
            /* Nie udało się stworzyć wątku – pasażer kontynuuje jako zwykły */
            is_guardian = 0;
        } else {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] Opiekun: watek dziecka uruchomiony\n",
                     b, (int)my_pid);
            log_write(ln);
        }
    }

    /* ===== Etap 6: Rejestracja w kasie (tylko pasażerowie nie-VIP) ===== */
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
            /* Wątek dziecka musi zostać zakończony przed zwolnieniem zasobów */
            if (is_guardian && child_tid) {
                pthread_mutex_lock(&ctx.mtx);
                ctx.boarded = 1;
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

        /* Wysłanie żądania rejestracji do kasjera */
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

        /* Oczekiwanie na bilet z kolejki odpowiedzi.
         * Typ wiadomości = MSG_TICKET_REPLY + my_pid, co gwarantuje,
         * że pasażer odbierze wyłącznie własny bilet. */
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

    /* ===== Etap 7: Wsiadanie do autobusu =====
     *
     * Pasażer wchodzi przez właściwą bramkę (sem[1] lub sem[2]),
     * następnie pod mutexem sem[0] sprawdza dostępność miejsca.
     *
     * Protokół (kolejność blokowania spójna z driver.c):
     *   1. Weź bramkę (sem[1] lub sem[2]).
     *   2. Weź mutex (sem[0]) i sprawdź: autobus obecny, nie odjeżdża, jest miejsce.
     *   3a. Miejsce dostępne: zarezerwuj, zwolnij mutex i bramkę, potwierdź wejście.
     *   3b. Brak miejsca lub brak autobusu: zwolnij mutex i bramkę, odczekaj 1 sekundę.
     *
     * Opiekun rezerwuje 2 miejsca atomowo pod mutexem.
     * Dziecko nie przechodzi przez bramkę samodzielnie – jest wpisywane na listę
     * z ujemnym PID-em przez opiekuna i powiadamiane przez ctx.boarded.
     */
    int boarded = 0;
    int needed  = is_guardian ? 2 : 1;   /* Opiekun potrzebuje miejsca dla siebie i dziecka */
    int gate    = has_bike ? 2 : 1;      /* Rowerzysta używa bramki rowerzystów             */

    while (!boarded) {
        /* Szybkie sprawdzenie flag przed blokowaniem na bramce */
        sem_lock();
        sd       = bus->shutdown;
        sblocked = bus->station_blocked;
        sem_unlock();

        if (sd || sblocked) break;

        /* Wzięcie bramki – serializuje dostęp pasażerów danego typu */
        if (gate_lock(gate) == -1) break;

        /* Pod bramką: wzięcie mutexu i weryfikacja stanu autobusu */
        sem_lock();
        sd       = bus->shutdown;
        sblocked = bus->station_blocked;

        if (sd || sblocked) {
            sem_unlock();
            gate_unlock(gate);
            break;
        }

        if (bus->driver_pid != 0 && !bus->departing) {
            int free_seats = bus->P - bus->passengers;
            int bike_ok    = !has_bike || (bus->bikes < bus->R);
            int list_ok    = (bus->passenger_count + needed) <= MAX_BUS_CAPACITY;

            if (free_seats >= needed && bike_ok && list_ok) {
                /* Atomowa rezerwacja miejsca i wpisanie na listę pasażerów */
                bus->passengers += needed;
                if (has_bike) bus->bikes++;

                bus->passenger_list[bus->passenger_count++] = my_pid;
                if (is_guardian) {
                    /* Dziecko wpisujemy z ujemnym PID-em opiekuna jako identyfikator */
                    bus->passenger_list[bus->passenger_count++] = -my_pid;
                    bus->total_children_with_guardian++;
                }

                boarded = 1;
                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln),
                         "[%s] [PASAZER %d] Wsiadl bramka=%d"
                         " (miejsca: %d/%d row: %d/%d opiekun=%d)\n",
                         b, (int)my_pid, gate,
                         bus->passengers, bus->P, bus->bikes, bus->R, is_guardian);
                log_write(ln);
                log_main(ln);

                sem_unlock();
                gate_unlock(gate);

                /* Powiadomienie wątku dziecka – opiekun wsiadł, dziecko wsiadło razem */
                if (is_guardian && child_tid) {
                    pthread_mutex_lock(&ctx.mtx);
                    ctx.boarded = 1;
                    pthread_cond_broadcast(&ctx.cond);
                    pthread_mutex_unlock(&ctx.mtx);
                }
            } else {
                /* Brak miejsca – zwolnij blokady i zaczekaj na zwolnienie miejsca */
                sem_unlock();
                gate_unlock(gate);
                sleep(1);
            }
        } else {
            /* Brak autobusu na dworcu lub autobus właśnie odjeżdża – zaczekaj */
            sem_unlock();
            gate_unlock(gate);
            sleep(1);
        }
    }

    /* Pasażer nie zdołał wsiąść – shutdown lub blokada podczas oczekiwania */
    if (!boarded) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Nie wsiadt – shutdown/blokada\n", b, (int)my_pid);
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

    /* ===== Etap 8: Oczekiwanie na powrót autobusu ===== */
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] W autobusie - czeka na powrot\n", b, (int)my_pid);
    log_write(ln);

    wait_for_trip_end(my_pid);

    /* Powiadomienie wątku dziecka, że kurs dobiegł końca */
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

    /* Czekamy na zakończenie wątku dziecka przed zwolnieniem zasobów procesu */
    if (is_guardian && child_tid) {
        pthread_join(child_tid, NULL);
        pthread_mutex_destroy(&ctx.mtx);
        pthread_cond_destroy(&ctx.cond);
    }

    shmdt(bus);
    return 0;
}
