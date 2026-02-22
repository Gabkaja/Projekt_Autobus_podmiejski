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

int shmid, semid, msgid;
struct BusState* bus;

/* Flaga ustawiana przez handler SIGUSR1 (wymuszony odjazd od dyspozytora).
 * volatile sig_atomic_t gwarantuje bezpieczny zapis w handlerze sygnału
 * i odczyt w pętli głównej bez wyścigu. */
volatile sig_atomic_t force_flag = 0;

/* Formatuje aktualny czas jako HH:MM:SS. */
void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

/* Dopisuje do prywatnego logu kierowcy. */
void log_write(const char* s) {
    int fd = open("driver.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Dopisuje do wspólnego raportu symulacji. */
void log_main(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/* Blokujące P(sem[0]) – zajmuje mutex ogólny.
 * Pętla ponawia przy EINTR, kończy gdy IPC zostaje usunięte. */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

/* V(sem[0]) – zwalnia mutex ogólny. */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Blokujące P(sem[gate]) – zajmuje wskazany semafor bramki lub dworca.
 * SEM_UNDO gwarantuje zwolnienie semafora nawet gdy proces kierowcy
 * zostanie niespodziewanie zabity, co zapobiega trwałemu zablokowaniu. */
void gate_lock(int gate) {
    struct sembuf sb = { (unsigned short)gate, -1, SEM_UNDO };
    while (semop(semid, &sb, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

/* V(sem[gate]) – zwalnia wskazany semafor bramki lub dworca. */
void gate_unlock(int gate) {
    struct sembuf sb = { (unsigned short)gate, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/* Handler SIGUSR1 – wymuszony odjazd wysyłany przez dyspozytora.
 * Ustawia tylko flagę; faktyczne odjeżdżanie dzieje się w pętli głównej. */
void handle_usr1(int sig) {
    (void)sig;
    force_flag = 1;
}

/* Handler SIGUSR2 – zablokowanie dworca od dyspozytora.
 * Ustawia station_blocked pod mutexem żeby pasażerowie zobaczyli
 * spójny stan shared memory. */
void handle_usr2(int sig) {
    (void)sig;
    sem_lock();
    bus->station_blocked = 1;
    sem_unlock();
}

/* Handler SIGINT – globalny shutdown.
 * Ustawia obie flagi pod mutexem. */
void handle_int(int sig) {
    (void)sig;
    sem_lock();
    bus->shutdown = 1;
    bus->station_blocked = 1;
    sem_unlock();
}

int main() {
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1) {
        perror("ftok");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 7, 0600);
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

    /* SA_RESTART żeby przerwane przez sygnał sleep() wznawiały się poprawnie. */
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

    /* XOR pid z czasem daje unikalne ziarno dla każdego z N procesów kierowców,
     * żeby czasy tras były niezależne od siebie. */
    srand((unsigned)(getpid() ^ time(NULL)));

    char b[64];
    ts(b, sizeof(b));
    char ln[2048];
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Start pracy\n", b, getpid());
    log_write(ln);
    log_main(ln);

    for (;;) {
        /* Sprawdzamy pod mutexem czy inny kierowca nie zajął już dworca.
         * Kolejność blokowania: najpierw mutex, potem gate – musi być spójna
         * z pasażerami którzy blokują gate a potem mutex. Odwrotna kolejność
         * groziłaby zakleszczeniem. */
        sem_lock();
        
        if (bus->driver_pid != 0 && bus->driver_pid != getpid()) {
            int sd_tmp = bus->shutdown;
            int sb_tmp = bus->station_blocked;
            sem_unlock();
            
            if (sd_tmp || sb_tmp) {
                break;
            }
            
            sleep(1);
            continue;
        }
        
        int sb = bus->station_blocked;
        int sd = bus->shutdown;
        sem_unlock();

        if (sd || sb) {
            break;
        }

        /* Bierzemy semafor dworca (sem[3]) – gwarantuje że tylko jeden
         * autobus stoi na dworcu jednocześnie. Pozostałe autobusy blokują
         * się tutaj i czekają na swoją kolej. */
        gate_lock(3);
        
        /* Po wzięciu gate[3] ponownie sprawdzamy driver_pid – inny kierowca
         * mógł wejść na dworzec między pierwszym sprawdzeniem a gate_lock. */
        sem_lock();
        if (bus->driver_pid != 0 && bus->driver_pid != getpid()) {
            sem_unlock();
            gate_unlock(3);
            sleep(1);
            continue;
        }
        
        /* Rejestrujemy siebie jako aktualnego kierowcę na dworcu. */
        bus->driver_pid = getpid();
        bus->departing = 0;
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

        /* Czekamy T sekund na zapełnienie autobusu lub na sygnał SIGUSR1
         * od dyspozytora (force_flag). Co sekundę sprawdzamy też flagi
         * shutdown i station_blocked żeby nie czekać w nieskończoność. */
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

        /* Jeśli system się wyłącza i autobus jest pusty – nie ma sensu jechać,
         * kończymy bez odjeżdżania. Jeśli są pasażerowie, musimy ich odwieźć
         * nawet przy shutdown – nie można ich zostawić w autobusie. */
        if ((sd || sb) && current_passengers == 0) {
            gate_unlock(3);
            break;
        }

        force_flag = 0;

        /* Zamykamy drzwi: blokujemy obie bramki pasażerskie zanim
         * ustawimy flagę departing i skopiujemy listę pasażerów.
         * Kolejność gate[1] → gate[2] → mutex jest spójna z pasażerem
         * który robi gate[X] → mutex, więc deadlock jest niemożliwy.
         * Bez blokowania bramek pasażer mógłby wsiąść już po skopiowaniu listy
         * i nigdy nie zostałby powiadomiony o powrocie autobusu. */
        gate_lock(1);
        gate_lock(2);

        sem_lock();
        bus->departing = 1;
        int p = bus->passengers;
        int r = bus->bikes;
        int pcount = bus->passenger_count;
        bus->boarded_passengers += p;

        /* Kopiujemy listę pasażerów lokalnie, bo za chwilę wyzerujemy
         * pola shared memory żeby następny autobus mógł przyjechać. */
        pid_t plist[MAX_BUS_CAPACITY];
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            plist[i] = bus->passenger_list[i];
        }

        /* Zerujemy liczniki – dworzec wolny dla następnego autobusu. */
        bus->passengers = 0;
        bus->bikes = 0;
        bus->passenger_count = 0;
        bus->driver_pid = 0;
        sem_unlock();

        /* Zwalniamy bramki pasażerskie i semafor dworca jednocześnie –
         * pasażerowie i kolejny kierowca mogą teraz wejść na dworzec. */
        gate_unlock(1);
        gate_unlock(2);
        gate_unlock(3);

        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Odjazd: %d pasazerow, %d rowerow\n",
                 b, getpid(), p, r);
        log_write(ln);
        log_main(ln);

        /* Czas trasy: losowy z przedziału 3–9 sekund.
         * Jeśli autobus jest załadowany, blokujemy SIGINT na czas jazdy –
         * nie można przerwać trasy z pasażerami w środku.
         * Jeśli jedzie pusty, można przerwać wcześniej gdy nadejdzie shutdown. */
        int Ti = (rand() % 7) + 3;
        
        if (p > 0) {
            sigset_t sigset, oldset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGINT);
            sigprocmask(SIG_BLOCK, &sigset, &oldset);
            
            sleep(Ti);
            
            sigprocmask(SIG_SETMASK, &oldset, NULL);
        } else {
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

        /* Budujemy czytelną listę PID-ów pasażerów do logu.
         * Ujemne PID-y to dzieci – wyświetlamy je jako "dziecko_PID". */
        if (pcount > 0) {
            char plist_str[1024] = "[";
            for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
                char tmp[64];
                pid_t pid = plist[i];
                if (pid < 0) {
                    snprintf(tmp, sizeof(tmp), "dziecko_%d%s", -pid, (i < pcount - 1) ? ", " : "");
                } else {
                    snprintf(tmp, sizeof(tmp), "%d%s", pid, (i < pcount - 1) ? ", " : "");
                }
                strncat(plist_str, tmp, sizeof(plist_str) - strlen(plist_str) - 1);
            }
            strncat(plist_str, "]", sizeof(plist_str) - strlen(plist_str) - 1);
            
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Rozwieziono pasazerow: %s\n", 
                     b, getpid(), plist_str);
            log_write(ln);
            log_main(ln);
        }

        /* Powiadamianie pasażerów o powrocie autobusu odbywa się przez
         * kombinację flagi w tablicy i semafora sem[6].
         * Najpierw pod mutexem ustawiamy passenger_trip_completed[pid] = 1
         * dla każdego dorosłego pasażera (dzieci z ujemnym PID pomijamy,
         * bo wątek dziecka czeka na sygnał od opiekuna, nie na semafor).
         * Potem poza mutexem podnosimy sem[6] raz na pasażera – każde
         * podniesienie obudzi dokładnie jednego czekającego pasażera.
         * Pasażer sprawdza czy token jest jego (po flagi) i jeśli tak – kończy. */
        sem_lock();
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            pid_t passenger_pid = plist[i];
            
            if (passenger_pid < 0) {
                continue; /* dziecko – pomijamy, opiekun je powiadomi */
            }
            
            if (passenger_pid > 0 && passenger_pid < MAX_PID) {
                bus->passenger_trip_completed[passenger_pid] = 1;
            }
        }
        sem_unlock();
        
        /* Podnosimy sem[6] po jednym razie dla każdego dorosłego pasażera.
         * Bez SEM_UNDO – celowo: jeśli kierowca padnie po tym punkcie,
         * tokeny pozostają w semaforze i pasażerowie mogą się wybudzić. */
        for (int i = 0; i < pcount && i < MAX_BUS_CAPACITY; i++) {
            pid_t passenger_pid = plist[i];
            
            if (passenger_pid < 0) {
                continue;
            }
            
            struct sembuf sb = { 6, 1, 0 };
            if (semop(semid, &sb, 1) == -1) {
                if (errno == EIDRM || errno == EINVAL) {
                    break; /* semafory usunięte – system się kończy */
                }
            }
        }

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
