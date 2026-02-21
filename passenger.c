/*
 * passenger.c - Proces pasażera
 *
 * Typy pasażerów (losowe):
 *   VIP         (~20%) - omija kasę, wsiada bez biletu
 *   Opiekun     (~15%) - idzie do kasy, wsiada za 2 miejsca (on + dziecko)
 *   Z rowerem   (~20%) - idzie do kasy, potrzebuje miejsca na rower
 *   Zwykły      (~35%) - idzie do kasy
 *   Dziecko bez opiekuna (~10%) - odrzucany natychmiast
 *
 * Synchronizacja:
 *   sem[0] - mutex dla pamięci dzielonej
 *   sem[6] - sygnał powrotu autobusu (driver postuje N razy dla N pasażerów)
 *            + passenger_trip_completed[pid] jako flaga kto konkretnie może wyjść
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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

static int shmid, semid, msgid, msgid_reply;
static struct BusState *bus;

static void ts(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
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

/*
 * Czeka na zakończenie trasy autobusu.
 *
 * Driver po powrocie z trasy:
 *   1. (pod mutexem) Ustawia passenger_trip_completed[pid] = 1 dla każdego pasażera
 *   2. (po mutexie)  Robi semop(sem[6], +1) dla każdego dorosłego pasażera
 *
 * Pasażer ZAWSZE najpierw konsumuje token (semop -1), a potem sprawdza flagę.
 * Jeśli token nie jest jego (spurious wakeup – inny pasażer wziął nie swój),
 * oddaje go z powrotem i czeka dalej.
 *
 * Ważne: NIE sprawdzamy flagi przed semop, bo wtedy pasażer mógłby wyjść bez
 * konsumowania tokenu → nagromadzenie "duchowych" tokenów i niepotrzebne
 * spurious wakeups w kolejnych kursach.
 *
 * W przypadku shutdown main.c postuje MAX_PASSENGERS sygnałów na sem[6]
 * żeby obudzić każdego zablokowanego pasażera.
 */
static void wait_for_trip_end(pid_t my_pid)
{
    char b[64];
    char ln[256];

    for (;;) {
        /* Zablokuj na semaforze trip_completed – ZAWSZE najpierw semop,
         * żeby nie zgubić tokenu i nie dopuścić do kumulacji. */
        struct sembuf sb_wait = {6, -1, 0};
        if (semop(semid, &sb_wait, 1) == -1) {
            if (errno == EINTR) {
                /* Przerwane sygnałem – wróć do czekania */
                continue;
            }
            /* EIDRM / EINVAL – IPC usunięte, system się wyłącza.
             * Jeśli byliśmy w autobusie, driver już nie może nas powiadomić.
             * Wychodzimy bezpiecznie. */
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] IPC usuniete podczas czekania - koniec\n",
                     b, (int)my_pid);
            log_write(ln);
            return;
        }

        /* Sprawdź czy to nasz token */
        if (my_pid > 0 && my_pid < MAX_PID
            && bus->passenger_trip_completed[my_pid]) {
            bus->passenger_trip_completed[my_pid] = 0;
            return; /* Token skonsumowany, flaga wyczyszczona – koniec */
        }

        /* Nie nasz token (spurious wakeup) – odłóż go z powrotem. */
        struct sembuf sb_ret = {6, 1, 0};
        semop(semid, &sb_ret, 1);

        /* Krótka pauza żeby uniknąć busy-waitu gdy wiele pasażerów
         * jednocześnie oddaje nie swój token. */
        sleep(1);
    }
}

int main(void)
{
    /* Połącz z zasobami IPC */
    key_t shm_key     = ftok(SHM_PATH, 'S');
    key_t sem_key     = ftok(SEM_PATH, 'E');
    key_t msg_key     = ftok(MSG_PATH, 'M');
    key_t msg_rpl_key = ftok(MSG_REPLY_PATH, 'R');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1 || msg_rpl_key == -1) {
        perror("ftok passenger");
        return 1;
    }

    shmid      = shmget(shm_key, sizeof(struct BusState), 0600);
    semid      = semget(sem_key, 0, 0600);    /* nsems=0: dostęp do istniejącego zestawu */
    msgid      = msgget(msg_key, 0600);       /* kolejka żądań (wysyłanie rejestracji) */
    msgid_reply= msgget(msg_rpl_key, 0600);   /* kolejka odpowiedzi (odbiór biletu) */

    if (shmid == -1 || semid == -1 || msgid == -1 || msgid_reply == -1) {
        perror("get ipc passenger");
        return 1;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void *)-1) {
        perror("shmat passenger");
        return 1;
    }

    pid_t my_pid = getpid();
    srand((unsigned)(my_pid ^ (unsigned)time(NULL)));

    char b[64];
    char ln[512];
    ts(b, sizeof(b));

    /* ===== KROK 1: Sprawdź czy dworzec jest otwarty ===== */
    sem_lock();
    int sd = bus->shutdown;
    int sblocked = bus->station_blocked;
    sem_unlock();

    if (sd || sblocked) {
        sem_lock();
        bus->total_station_blocked++;
        sem_unlock();

        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Dworzec zamkniety - odchodzi\n", b, (int)my_pid);
        log_write(ln);
        shmdt(bus);
        return 0;
    }

    /* ===== KROK 2: Losuj typ pasażera ===== */
    int r = rand() % 100;
    int is_vip        = (r < 20);                           /* 20% */
    int is_lone_child = (!is_vip && r < 30);                /* 10% */
    int has_bike      = (!is_vip && !is_lone_child && r < 50); /* 20% */
    int is_guardian   = (!is_vip && !is_lone_child && !has_bike && r < 65); /* 15% */
    /* pozostałe ~35% to zwykły pasażer */

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] Start: VIP=%d DZIECKO_SAM=%d ROWER=%d OPIEKUN=%d\n",
             b, (int)my_pid, is_vip, is_lone_child, has_bike, is_guardian);
    log_write(ln);

    /* ===== KROK 3: Aktualizacja liczników typów ===== */
    sem_lock();
    if (is_vip) {
        bus->total_vip++;
    } else {
        bus->total_non_vip++;
        if (has_bike)    bus->total_bikes++;
    }
    sem_unlock();

    /* ===== KROK 4: Dziecko bez opiekuna - odrzucamy ===== */
    if (is_lone_child) {
        sem_lock();
        bus->total_children_without_guardian++;
        sem_unlock();

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Dziecko bez opiekuna - odrzucony (brak opiekuna)\n",
                 b, (int)my_pid);
        log_write(ln);
        log_main(ln);

        shmdt(bus);
        return 0;
    }

    /* ===== KROK 5: Niebędący VIP idzie do kasy ===== */
    if (!is_vip) {
        /* Sprawdź station_blocked tuż przed wysłaniem do kasy */
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
        m.child = is_guardian; /* sygnalizuje że opiekun ma ze sobą dziecko */

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Ide do kasy (ROWER=%d OPIEKUN=%d)\n",
                 b, (int)my_pid, has_bike, is_guardian);
        log_write(ln);

        if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
            if (errno == EIDRM) {
                shmdt(bus);
                return 0;
            }
            perror("msgsnd register");
            shmdt(bus);
            return 0;
        }

        /* Czekaj na bilet z kolejki ODPOWIEDZI (msgid_reply).
         * Typ = MSG_TICKET_REPLY + pid – unikalna wartość per pasażer,
         * więc nie ma ryzyka podebrania biletu cudzego pasażera. */
        long reply_type = MSG_TICKET_REPLY + (long)my_pid;
        ssize_t rcv;
        do {
            rcv = msgrcv(msgid_reply, &m, sizeof(m) - sizeof(long), reply_type, 0);
        } while (rcv < 0 && errno == EINTR);

        if (rcv < 0) {
            /* Kolejka usunięta lub inny błąd - system się wyłącza */
            shmdt(bus);
            return 0;
        }

        if (!m.ticket_ok) {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln),
                     "[%s] [PASAZER %d] Kasa odmowila biletu - odchodzi\n",
                     b, (int)my_pid);
            log_write(ln);
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

    /* ===== KROK 6: Wsiadanie do autobusu ===== */
    /*
     * Pasażer czeka aż autobus będzie na przystanku i będzie wolne miejsce.
     * Sprawdzamy i rezerwujemy miejsce pod mutexem (sem[0]), więc nie ma
     * wyścigu między pasażerami.
     *
     * Opiekun potrzebuje 2 miejsc (dla siebie i dziecka).
     */
    int boarded   = 0;
    int needed    = is_guardian ? 2 : 1;

    while (!boarded) {
        sem_lock();
        sd       = bus->shutdown;
        sblocked = bus->station_blocked;

        if (sd || sblocked) {
            sem_unlock();
            break;
        }

        /* Autobus musi być na przystanku i nie może jeszcze odjeżdżać */
        if (bus->driver_pid != 0 && !bus->departing) {
            int free_seats = bus->P - bus->passengers;
            int bike_ok    = !has_bike || (bus->bikes < bus->R);
            int list_ok    = (bus->passenger_count + needed) <= MAX_BUS_CAPACITY;

            if (free_seats >= needed && bike_ok && list_ok) {
                /* Wsiadamy */
                bus->passengers += needed;
                if (has_bike) bus->bikes++;

                /* Dodaj siebie do listy pasażerów */
                bus->passenger_list[bus->passenger_count++] = my_pid;

                /* Jeśli opiekun: dodaj dziecko jako negatywny PID */
                if (is_guardian) {
                    bus->passenger_list[bus->passenger_count++] = -my_pid;
                    bus->total_children_with_guardian++;
                }

                boarded = 1;

                ts(b, sizeof(b));
                snprintf(ln, sizeof(ln),
                         "[%s] [PASAZER %d] Wsiadt do autobusu "
                         "(miejsca: %d/%d, rowery: %d/%d, opiekun=%d)\n",
                         b, (int)my_pid,
                         bus->passengers, bus->P,
                         bus->bikes, bus->R,
                         is_guardian);
                log_write(ln);
                log_main(ln);

                sem_unlock();
            } else {
                /* Brak miejsca - czekaj */
                sem_unlock();
                sleep(1);
            }
        } else {
            /* Brak autobusu lub właśnie odjeżdża */
            sem_unlock();
            sleep(1);
        }
    }

    if (!boarded) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln),
                 "[%s] [PASAZER %d] Nie wsiadt (shutdown/blokada)\n",
                 b, (int)my_pid);
        log_write(ln);
        shmdt(bus);
        return 0;
    }

    /* ===== KROK 7: Czekanie na powrót autobusu ===== */
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] W autobusie - czeka na powrot\n",
             b, (int)my_pid);
    log_write(ln);

    wait_for_trip_end(my_pid);

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln),
             "[%s] [PASAZER %d] Wrocil z trasy - koniec pracy\n",
             b, (int)my_pid);
    log_write(ln);
    log_main(ln);

    shmdt(bus);
    return 0;
}
