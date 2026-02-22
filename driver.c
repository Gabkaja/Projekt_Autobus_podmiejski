/*
 * driver.c – Proces kierowcy autobusu.
 *
 * Każdy kierowca reprezentuje jeden autobus. W systemie może działać jednocześnie
 * N procesów kierowcy (N = parametr uruchomienia), lecz tylko jeden może
 * przebywać na dworcu w danym momencie – gwarantuje to semafor sem[3] (mutex dworca).
 *
 * Cykl jednego kursu:
 *   1. Sprawdzenie flag shutdown/station_blocked – jeśli ustawione, kończenie pracy.
 *   2. Wzięcie semafora dworca (sem[3]) – czekanie na zwolnienie stacji przez inny autobus.
 *   3. Rejestracja własnego PID w bus->driver_pid – pasażerowie wiedzą, że autobus stoi.
 *   4. Oczekiwanie przez T sekund lub do wymuszenia odjazdu przez SIGUSR1.
 *   5. Zamknięcie bramek pasażerskich (sem[1], sem[2]) – blokada wsiadania.
 *   6. Skopiowanie listy pasażerów z pamięci dzielonej i wyzerowanie liczników.
 *   7. Zwolnienie bramek i semafora dworca.
 *   8. Symulacja jazdy (rand() % 7 + 3 sekund).
 *   9. Powiadomienie pasażerów o powrocie przez flagę passenger_trip_completed[pid]
 *      i podniesienie sem[5] dla każdego pasażera.
 *  10. Powrót do kroku 1.
 *
 * Obsługiwane sygnały:
 *   SIGUSR1 – Wymuszony odjazd: ustawia force_flag, kierowca wyjeżdża natychmiast.
 *   SIGUSR2 – Blokada dworca: ustawia station_blocked w pamięci dzielonej.
 *   SIGINT  – Shutdown: ustawia shutdown i station_blocked.
 *
 * Kolejność blokowania semaforów (zapobieganie zakleszczeniom):
 *   Zawsze: mutex ogólny (sem[0]) PRZED bramką/muteksem dworca,
 *           lub bramka (sem[1]/sem[2]) PRZED mutexem (sem[0]).
 *   Nigdy odwrotnie.
 */

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

/* Globalne identyfikatory IPC */
int shmid, semid, msgid;
struct BusState* bus;

/* Flaga wymuszenia odjazdu – ustawiana przez handler SIGUSR1 */
volatile sig_atomic_t force_flag = 0;

/* =========================================================
 * Funkcje pomocnicze: timestamp i logowanie
 * ========================================================= */

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

/* =========================================================
 * Operacje na semaforach
 * ========================================================= */

/*
 * sem_lock / sem_unlock – Mutex ogólny (sem[0]).
 * Chroni dostęp do wszystkich pól BusState.
 * Pętla przy EINTR zapewnia, że mutex zostanie wzięty
 * mimo przerwania przez sygnał.
 */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR)              continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/*
 * gate_lock / gate_unlock – Semafory bramek pasażerskich i dworca.
 * Parametr gate: 1 = bramka bez roweru, 2 = bramka z rowerem, 3 = mutex dworca.
 * SEM_UNDO gwarantuje zwolnienie semafora, nawet jeśli kierowca zakończy
 * się awaryjnie przed jawnym wywołaniem gate_unlock.
 */
void gate_lock(int gate) {
    struct sembuf sb = { (unsigned short)gate, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR)              continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

void gate_unlock(int gate) {
    struct sembuf sb = { (unsigned short)gate, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* =========================================================
 * Handlery sygnałów
 * ========================================================= */

/*
 * handle_usr1 – Wymuszenie natychmiastowego odjazdu przez dyspozytora.
 * Ustawia force_flag; pętla oczekiwania T sekund sprawdza tę flagę.
 */
void handle_usr1(int sig) {
    (void)sig;
    force_flag = 1;
}

/*
 * handle_usr2 – Blokada dworca przez dyspozytora.
 * Ustawia station_blocked pod mutexem, aby zmiana była widoczna
 * dla pasażerów sprawdzających tę flagę.
 */
void handle_usr2(int sig) {
    (void)sig;
    sem_lock();
    bus->station_blocked = 1;
    sem_unlock();
}

/*
 * handle_int – Shutdown systemu.
 * Ustawia zarówno shutdown, jak i station_blocked, co powoduje,
 * że wszystkie pętle sprawdzające te flagi zakończą się.
 */
void handle_int(int sig) {
    (void)sig;
    sem_lock();
    bus->shutdown = 1;
    bus->station_blocked = 1;
    sem_unlock();
}

/* =========================================================
 * Funkcja główna kierowcy
 * ========================================================= */

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1) {
        perror("ftok");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 6, 0600);
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

    /* Rejestracja handlerów sygnałów z SA_RESTART */
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
    char ln[2048];
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Start pracy\n", b, getpid());
    log_write(ln);
    log_main(ln);

    /* =========================================================
     * Pętla kursów
     * ========================================================= */
    for (;;) {
        /*
         * Sprawdzamy, czy inny kierowca już obsługuje stację.
         * Mutex ogólny bierzemy przed semaforem dworca (sem[3]),
         * aby zachować spójną kolejność blokowania.
         */
        sem_lock();

        if (bus->driver_pid != 0 && bus->driver_pid != getpid()) {
            int sd_tmp = bus->shutdown;
            int sb_tmp = bus->station_blocked;
            sem_unlock();

            /* Przy shutdown/blokadzie natychmiast przerywamy */
            if (sd_tmp || sb_tmp) break;

            sleep(1);
            continue;
        }

        int sb = bus->station_blocked;
        int sd = bus->shutdown;
        sem_unlock();

        if (sd || sb) break;

        /* Bierzemy mutex dworca – tylko jeden autobus jednocześnie na stacji */
        gate_lock(3);

        /* Ponowna kontrola po wzięciu semafora dworca – stan mógł się zmienić */
        sem_lock();
        if (bus->driver_pid != 0 && bus->driver_pid != getpid()) {
            sem_unlock();
            gate_unlock(3);
            sleep(1);
            continue;
        }

        /* Rejestracja kierowcy i odczyt parametrów */
        bus->driver_pid = getpid();
        bus->departing  = 0;
        sb = bus->station_blocked;
        sd = bus->shutdown;
        int wait_time = bus->T;
        sem_unlock();

        if (sd || sb) {
            gate_unlock(3);
            break;
        }

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Autobus na dworcu\n", b, getpid());
        log_write(ln);
        log_main(ln);

        /*
         * Oczekiwanie na pasażerów przez T sekund.
         * Pętla sprawdza co sekundę flagę force_flag (wymuszony odjazd)
         * oraz flagi shutdown/station_blocked.
         */
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

        /*
         * Przy shutdown bez pasażerów autobus nie wyjeżdża – kończy pracę.
         * Jeśli jednak są pasażerowie, kierowca musi dokończyć kurs,
         * aby odpowiedzialnie odwieźć osoby już w autobusie.
         */
        if ((sd || sb) && current_passengers == 0) {
            gate_unlock(3);
            break;
        }

        force_flag = 0;

        /*
         * Zamknięcie bramek wsiadania przed odczytem listy pasażerów.
         * Kolejność: sem[1] → sem[2] → sem[0].
         * Musi być spójna z kolejnością w passenger.c (gate → mutex),
         * by nie dopuścić do zakleszczenia.
         */
        gate_lock(1);   /* blokada bramki dla pasażerów bez roweru */
        gate_lock(2);   /* blokada bramki dla pasażerów z rowerem  */

        sem_lock();
        bus->departing = 1;
        int p      = bus->passengers;
        int r      = bus->bikes;
        int pcount = bus->passenger_count;
        bus->boarded_passengers += p;

        /* Lokalna kopia listy PID-ów pasażerów – potrzebna po zwolnieniu mutex */
        pid_t plist[MAX_BUS_CAPACITY];
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++)
            plist[i] = bus->passenger_list[i];

        /* Reset liczników w pamięci dzielonej przed zwolnieniem mutexu */
        bus->passengers      = 0;
        bus->bikes           = 0;
        bus->passenger_count = 0;
        bus->driver_pid      = 0;
        sem_unlock();

        /* Otwieramy bramki i zwalniamy dworzec – inny kierowca może teraz wjechać */
        gate_unlock(1);
        gate_unlock(2);
        gate_unlock(3);

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Odjazd: %d pasazerow, %d rowerow\n",
                 b, getpid(), p, r);
        log_write(ln);
        log_main(ln);

        /* Symulacja czasu jazdy: 3–9 sekund (losowo) */
        int Ti = (rand() % 7) + 3;

        if (p > 0) {
            /*
             * Przy pasażerach blokujemy SIGINT podczas jazdy.
             * Bez tego: Ctrl+C mógłby przerwać sleep i pasażerowie
             * nigdy nie doczekaliby się sygnału powrotu.
             */
            sigset_t sigset, oldset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGINT);
            sigprocmask(SIG_BLOCK, &sigset, &oldset);

            sleep(Ti);

            sigprocmask(SIG_SETMASK, &oldset, NULL);
        } else {
            /* Pusty autobus może przerwać jazdę wcześniej przy shutdown */
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

        /* Budowanie i logowanie listy PID-ów pasażerów kursu */
        if (pcount > 0) {
            char plist_str[1024] = "[";
            for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
                char tmp[64];
                pid_t pid = plist[i];
                /* Ujemny PID oznacza dziecko podróżujące z opiekunem */
                if (pid < 0)
                    snprintf(tmp, sizeof(tmp), "dziecko_%d%s", -pid, (i < pcount - 1) ? ", " : "");
                else
                    snprintf(tmp, sizeof(tmp), "%d%s", pid, (i < pcount - 1) ? ", " : "");
                strncat(plist_str, tmp, sizeof(plist_str) - strlen(plist_str) - 1);
            }
            strncat(plist_str, "]", sizeof(plist_str) - strlen(plist_str) - 1);

            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Rozwieziono pasazerow: %s\n",
                     b, getpid(), plist_str);
            log_write(ln);
            log_main(ln);
        }

        /*
         * Powiadamianie pasażerów o zakończeniu kursu.
         * Pod mutexem ustawiamy flagę passenger_trip_completed[pid] = 1
         * dla każdego dorosłego pasażera, następnie podnosimy sem[5] raz
         * dla każdego z nich, budząc tych, którzy czekają na semop(-1).
         *
         * Dzieci (ujemne PID-y) nie mają własnego procesu oczekującego –
         * są obsługiwane przez wątek w procesie opiekuna.
         */
        sem_lock();
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            pid_t passenger_pid = plist[i];
            if (passenger_pid < 0) continue;   /* pomijamy wpisy dzieci */
            if (passenger_pid > 0 && passenger_pid < MAX_PID)
                bus->passenger_trip_completed[passenger_pid] = 1;
        }
        sem_unlock();

        /* Podniesienie semafora sem[5] po jednym tokenie na pasażera.
         * Bez SEM_UNDO – token musi pozostać do skonsumowania przez pasażera. */
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            pid_t passenger_pid = plist[i];
            if (passenger_pid < 0) continue;

            struct sembuf sb_post = { 5, 1, 0 };
            if (semop(semid, &sb_post, 1) == -1) {
                if (errno == EIDRM || errno == EINVAL) break;  /* IPC usunięte */
            }
        }

        /* Sprawdzenie flag po powrocie do stacji */
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
