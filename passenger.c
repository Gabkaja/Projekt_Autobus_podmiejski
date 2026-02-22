/*
 * passenger.c – proces pasażera
 *
 * Każdy pasażer to osobny proces tworzony przez passenger_generator przez fork+exec.
 * Po starcie pasażer losuje swój wiek i typ, następnie przechodzi przez kolejne etapy:
 * sprawdzenie czy dworzec jest otwarty, ewentualna wizyta w kasie, wsiadanie do autobusu
 * i oczekiwanie na powrót z trasy.
 *
 * Wiek: 1–80 lat. Pasażer poniżej 8 lat bez opiekuna jest odrzucany natychmiast.
 *
 * Typy (dla wieku >= 8):
 *   VIP              ( 1%) – omija kasę, wsiada bezpośrednio
 *   Opiekun+dziecko  (24%) – idzie do kasy, rezerwuje 2 miejsca;
 *                            dziecko modelowane jest jako wątek pthread wewnątrz tego procesu
 *   Z rowerem        (25%) – idzie do kasy, potrzebuje miejsca na rower (wchodzi przez bramkę sem[2])
 *   Zwykły           (50%) – idzie do kasy, wchodzi przez bramkę sem[1]
 *
 * Semafory IPC:
 *   sem[0]  – mutex ogólny chroniący shared memory
 *   sem[1]  – bramka dla pasażerów bez roweru
 *   sem[2]  – bramka dla pasażerów z rowerem
 *   sem[6]  – sygnał powrotu autobusu; kierowca postuje +1 raz na każdego dorosłego pasażera
 *
 * Synchronizacja wewnętrzna opiekun ↔ wątek dziecka odbywa się przez:
 *   ChildCtx.mtx  – pthread_mutex
 *   ChildCtx.cond – pthread_cond
 *   ChildCtx.boarded   – flaga: opiekun wsiadł → budzi wątek dziecka po raz pierwszy
 *   ChildCtx.trip_done – flaga: autobus wrócił → budzi wątek dziecka po raz drugi
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

/* Globalne zasoby IPC – dostępne zarówno w wątku głównym (opiekuna) jak i w wątku dziecka. */
static int            shmid, semid, msgid, msgid_reply;
static struct BusState *bus;

/* Kontekst współdzielony między wątkiem opiekuna a wątkiem dziecka.
 * Wątek dziecka czeka na dwa kolejne sygnały: wsiadanie i powrót autobusu.
 * Opiekun ustawia odpowiednie flagi i rozgłasza przez pthread_cond_broadcast. */
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int      boarded;       /* 1 gdy opiekun (i dziecko) wsiedli do autobusu */
    int      trip_done;     /* 1 gdy autobus wrócił z trasy */
    pid_t    guardian_pid;  /* PID opiekuna – używany tylko do logowania */
} ChildCtx;

/* Formatuje aktualny czas jako HH:MM:SS. */
static void ts(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm *ti = localtime(&t);
    if (!ti) { snprintf(buf, n, "00:00:00"); return; }
    strftime(buf, n, "%H:%M:%S", ti);
}

/* Dopisuje do prywatnego logu pasażerów. */
static void log_write(const char *s)
{
    int fd = open("passenger.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Dopisuje do wspólnego raportu symulacji. */
static void log_main(const char *s)
{
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Blokujące P(sem[0]) – zajmuje mutex ogólny.
 * Ignoruje EINTR (przerwanie przez sygnał) i ponawia próbę. */
static void sem_lock(void)
{
    struct sembuf sb = {0, -1, SEM_UNDO};
    while (semop(semid, &sb, 1) == -1 && errno == EINTR)
        ;
}

/* V(sem[0]) – zwalnia mutex ogólny. */
static void sem_unlock(void)
{
    struct sembuf sb = {0, 1, SEM_UNDO};
    semop(semid, &sb, 1);
}

/* Funkcja wątku dziecka – modeluje zachowanie dziecka jadącego z opiekunem.
 *
 * Wątek czeka kolejno na dwa sygnały przez pthread_cond_wait:
 *  1. ctx->boarded == 1 – opiekun wsiadł, dziecko loguje "wsiadłem razem z opiekunem"
 *  2. ctx->trip_done == 1 – autobus wrócił, dziecko loguje "wróciłem" i kończy
 *
 * Użycie pthread zamiast osobnego procesu pozwala dziecku współdzielić
 * zasoby IPC i kontekst opiekuna bez dodatkowego fork+exec. */
static void *child_thread(void *arg)
{
    ChildCtx *ctx = (ChildCtx *)arg;
    char b[64], ln[256];

    /* Czekamy na sygnał wsiadania od opiekuna. */
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

    /* Czekamy na sygnał powrotu autobusu od opiekuna. */
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

/* Blokujące P(sem[gate]) – zajmuje bramkę wsiadania.
 * Zwraca 0 przy sukcesie, -1 gdy IPC zostało usunięte (czas kończyć). */
static int gate_lock(int g)
{
    struct sembuf sb = {(unsigned short)g, -1, SEM_UNDO};
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR)  continue;
        if (errno == EIDRM || errno == EINVAL) return -1;
        return -1;
    }
    return 0;
}

/* V(sem[gate]) – zwalnia bramkę wsiadania. */
static void gate_unlock(int g)
{
    struct sembuf sb = {(unsigned short)g, 1, SEM_UNDO};
    semop(semid, &sb, 1);
}

/* Czeka na powrót autobusu z trasy (blokuje wątek główny / opiekuna).
 *
 * Mechanizm oparty na kombinacji tablicy flag i semafora sem[6]:
 *  - Kierowca po powrocie ustawia passenger_trip_completed[pid] = 1
 *    i podnosi sem[6] o 1 dla każdego dorosłego pasażera.
 *  - Pasażer najpierw konsumuje token z sem[6] przez semop(-1),
 *    dopiero potem sprawdza czy flaga należy do niego.
 *  - Jeśli token był czyjś (race condition przy wielu pasażerach) –
 *    oddaje token z powrotem i czeka kolejną sekundę.
 *
 * Ta kolejność (consume → check, nie check → consume) zapobiega sytuacji
 * gdzie pasażer przeoczyłby swój token bo inny pasażer go skonsumował. */
static void wait_for_trip_end(pid_t my_pid)
{
    char b[64], ln[256];

    for (;;) {
        struct sembuf sb_wait = {6, -1, 0};
        if (semop(semid, &sb_wait, 1) == -1) {
            if (errno == EINTR) continue;
            /* EIDRM / EINVAL – zasoby IPC usunięte przez cleanup(), wychodzimy. */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] IPC usuniete podczas czekania - koniec\n",
                     b, (int)my_pid);
            log_write(ln);
            return;
        }

        /* Sprawdź czy pobrany token należy do nas. */
        if (my_pid > 0 && my_pid < MAX_PID
            && bus->passenger_trip_completed[my_pid]) {
            bus->passenger_trip_completed[my_pid] = 0; /* skasuj flagę */
            return; /* nasz token – podróż zakończona */
        }

        /* Nie nasz token – oddajemy go i czekamy dalej. */
        struct sembuf sb_ret = {6, 1, 0};
        semop(semid, &sb_ret, 1);
        sleep(1);
    }
}

int main(void)
{
    char b[64], ln[512];

    /* Łączymy się z istniejącymi zasobami IPC (bez IPC_CREAT).
     * nsems=0 w semget oznacza "nie twórz, przyłącz do istniejącego". */
    key_t shm_key     = ftok(SHM_PATH,       'S');
    key_t sem_key     = ftok(SEM_PATH,        'E');
    key_t msg_key     = ftok(MSG_PATH,        'M');
    key_t msg_rpl_key = ftok(MSG_REPLY_PATH,  'R');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1 || msg_rpl_key == -1) {
        perror("ftok passenger");
        return 1;
    }

    shmid       = shmget(shm_key,  sizeof(struct BusState), 0600);
    semid       = semget(sem_key,  0, 0600);
    msgid       = msgget(msg_key,  0600);
    msgid_reply = msgget(msg_rpl_key, 0600);

    if (shmid == -1 || semid == -1 || msgid == -1 || msgid_reply == -1) {
        perror("get ipc passenger");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void *)-1) { perror("shmat passenger"); return 1; }

    pid_t my_pid = getpid();
    /* XOR pid z czasem daje unikalne ziarno dla każdego procesu. */
    srand((unsigned)(my_pid ^ (unsigned)time(NULL)));
    ts(b, sizeof(b));

    /* ===== Krok 1: Sprawdź czy dworzec jest otwarty ===== */
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

    /* ===== Krok 2: Losuj wiek i typ pasażera =====
     *
     * Wiek: 1–80 lat (jednostajny rozkład).
     * Pasażer poniżej 8 lat = dziecko bez opiekuna → odrzucany natychmiast.
     *
     * Dla pozostałych (wiek >= 8), r = rand() % 100 wyznacza typ:
     *   r == 0       → VIP        ( 1%)
     *   r w 1–24     → opiekun    (24%)
     *   r w 25–49    → rowerzysta (25%)
     *   r w 50–99    → zwykły     (50%)
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

    /* ===== Krok 3: Zaktualizuj liczniki typów w shared memory ===== */
    sem_lock();
    if (is_vip) {
        bus->total_vip++;
    } else {
        bus->total_non_vip++;
        if (has_bike) bus->total_bikes++;
    }
    sem_unlock();

    /* ===== Krok 4: Dziecko bez opiekuna – odrzucamy od razu ===== */
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

    /* ===== Krok 5: Opiekun – uruchom wątek dziecka =====
     *
     * Wątek startuje od razu ale czeka na ctx.boarded == 1 zanim cokolwiek zaloguje.
     * Jeśli pthread_create się nie uda, degradujemy opiekuna do zwykłego pasażera
     * (is_guardian = 0) – traci jedno miejsce, ale nie zawiesza systemu. */
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
            is_guardian = 0; /* degradacja – kontynuujemy jako zwykły pasażer */
        } else {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] Opiekun: watek dziecka uruchomiony\n",
                     b, (int)my_pid);
            log_write(ln);
        }
    }

    /* ===== Krok 6: Kasa (tylko nie-VIP) =====
     *
     * Pasażer wysyła MSG_REGISTER do kolejki żądań i blokuje się czekając
     * na bilet w kolejce odpowiedzi (typ = MSG_TICKET_BASE + własny pid).
     * VIP-owie ten krok całkowicie pomijają. */
    if (!is_vip) {
        sem_lock();
        sd       = bus->shutdown;
        sblocked = bus->station_blocked;
        if (sd || sblocked) {
            /* Dworzec zamknął się gdy pasażer był w drodze do kasy. */
            bus->total_station_blocked++;
            sem_unlock();
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] Dworzec zamkniety przed kasa - odchodzi\n",
                     b, (int)my_pid);
            log_write(ln);
            /* Zanim wyjdziemy, musimy bezpiecznie zakończyć wątek dziecka. */
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

        /* Wysyłamy żądanie rejestracji do kasjera. */
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

        /* Czekamy na bilet z kolejki odpowiedzi. Typ wiadomości = MSG_TICKET_BASE + pid
         * gwarantuje że odbierzemy tylko swój bilet, a nie cudzej odpowiedzi. */
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

    /* ===== Krok 7: Wsiadanie do autobusu =====
     *
     * Pasażer wchodzi przez właściwą bramkę:
     *   sem[1] – pasażerowie bez roweru
     *   sem[2] – pasażerowie z rowerem
     *
     * Protokół wsiadania (kolejność blokowania MUSI być spójna z driver.c):
     *   1. Weź bramkę (sem[1] lub sem[2])
     *   2. Pod bramką weź mutex (sem[0]) i sprawdź dostępność miejsc
     *   3. Jeśli miejsce jest – zarezerwuj atomowo i wsiądź
     *   4. Zwolnij mutex, zwolnij bramkę
     *   5. Jeśli miejsca brak lub autobus odjeżdża – zwolnij mutex i bramkę, poczekaj 1s i wróć do 1
     *
     * Opiekun rezerwuje 2 miejsca naraz pod mutexem (on + dziecko).
     * Dziecko nie przechodzi przez bramkę samodzielnie – wchodzi logicznie razem
     * z opiekunem i jest powiadamiane przez ctx.boarded. */
    int boarded = 0;
    int needed  = is_guardian ? 2 : 1;
    int gate    = has_bike ? 2 : 1;

    while (!boarded) {
        /* Szybkie sprawdzenie shutdown bez zajmowania bramki. */
        sem_lock();
        sd       = bus->shutdown;
        sblocked = bus->station_blocked;
        sem_unlock();

        if (sd || sblocked) break;

        /* Zajmujemy bramkę – serializuje wejście pasażerów tego samego typu. */
        if (gate_lock(gate) == -1) break;

        /* Pod bramką zajmujemy mutex i sprawdzamy stan autobusu. */
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
                /* Rezerwujemy miejsca – atomowo pod mutexem. */
                bus->passengers += needed;
                if (has_bike) bus->bikes++;

                /* Dorosły pasażer zapisywany z dodatnim PID, dziecko z ujemnym.
                 * Kierowca rozróżnia je przy logowaniu i powiadamianiu po kursie. */
                bus->passenger_list[bus->passenger_count++] = my_pid;
                if (is_guardian) {
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

                /* Powiadamiamy wątek dziecka że oboje wsiedli. */
                if (is_guardian && child_tid) {
                    pthread_mutex_lock(&ctx.mtx);
                    ctx.boarded = 1;
                    pthread_cond_broadcast(&ctx.cond);
                    pthread_mutex_unlock(&ctx.mtx);
                }
            } else {
                /* Brak miejsca – zwalniamy bramkę i czekamy sekundę. */
                sem_unlock();
                gate_unlock(gate);
                sleep(1);
            }
        } else {
            /* Brak autobusu na dworcu lub autobus właśnie odjeżdża – czekamy. */
            sem_unlock();
            gate_unlock(gate);
            sleep(1);
        }
    }

    if (!boarded) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Nie wsiadt – shutdown/blokada\n", b, (int)my_pid);
        log_write(ln);
        /* Wątek dziecka musi zostać zakończony zanim zwolnimy zasoby. */
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

    /* ===== Krok 8: Czekanie na powrót autobusu =====
     *
     * Pasażer blokuje się w wait_for_trip_end() dopóki kierowca nie wróci
     * z trasy i nie ustawi flagi passenger_trip_completed[my_pid].
     * Szczegóły mechanizmu opisane przy definicji wait_for_trip_end(). */
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] W autobusie - czeka na powrot\n", b, (int)my_pid);
    log_write(ln);

    wait_for_trip_end(my_pid);

    /* Informujemy wątek dziecka o powrocie autobusu. */
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

    /* Czekamy na zakończenie wątku dziecka zanim zwolnimy ChildCtx –
     * inaczej wątek mógłby pisać do już zwolnionej pamięci stosu. */
    if (is_guardian && child_tid) {
        pthread_join(child_tid, NULL);
        pthread_mutex_destroy(&ctx.mtx);
        pthread_cond_destroy(&ctx.cond);
    }

    shmdt(bus);
    return 0;
}
