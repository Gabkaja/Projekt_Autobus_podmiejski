/*
 * PASSENGER.C - Proces Pasażera
 * 
 * Ten proces symuluje pojedynczego pasażera próbującego wsiąść do autobusu.
 * 
 * Charakterystyka pasażera (losowana):
 * - Wiek (0-79 lat)
 * - VIP status (1% szans)
 * - Czy ma rower (50% szans)
 * - Czy jest z dzieckiem (20% szans dla dorosłych)
 * 
 * Przepływ procesu:
 * 1. Generowanie losowych cech pasażera
 * 2. Sprawdzenie czy dworzec jest otwarty
 * 3. Odrzucenie dzieci bez opiekuna (wiek < 8)
 * 4. Rejestracja w kasie
 * 5. Oczekiwanie na bilet (jeśli nie VIP)
 * 6. Obsługa pasażera z dzieckiem (fork procesu dziecka)
 * 7. Próby wsiadania do autobusu (pętla z czekaniem)
 * 
 * Synchronizacja:
 * - Semafory gate[1] i gate[2] kontrolują dostęp do autobusu
 * - Atomowe operacje sprawdzania miejsca i wsiadania
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ipc.h"

// Globalne ID zasobów IPC
int shmid, semid, msgid;
struct BusState* bus;

/*
 * Funkcja ts (timestamp) - generuje aktualny znacznik czasu
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
 * Funkcja log_write - zapisuje wpis do pliku report.txt
 */
void log_write(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/*
 * Funkcja sem_lock - blokuje semafor mutex (sem[0])
 */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/*
 * Funkcja sem_unlock - odblokowuje semafor mutex (sem[0])
 */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/*
 * Funkcja gate_lock - blokuje określoną bramkę
 * Parametry:
 *   gate - numer bramki (1 = bez roweru, 2 = z rowerem)
 */
void gate_lock(int gate) {
    struct sembuf sb = { gate, -1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/*
 * Funkcja gate_unlock - odblokowuje określoną bramkę
 */
void gate_unlock(int gate) {
    struct sembuf sb = { gate, 1, SEM_UNDO };
    semop(semid, &sb, 1);
}

/*
 * Funkcja try_board - próba wejścia do autobusu
 * 
 * Parametry:
 *   bike - czy pasażer ma rower (1 = tak, 0 = nie)
 *   with_child - czy pasażer jest z dzieckiem (1 = tak, 0 = nie)
 *   vip - czy pasażer jest VIP (obecnie nieużywane)
 * 
 * Zwraca:
 *   1 - sukces (wsiadł)
 *   0 - system się wyłącza (koniec procesu)
 *  -1 - brak miejsca (spróbuj ponownie)
 * 
 * WAŻNE: Ta funkcja wykonuje ATOMOWĄ operację:
 * 1. Zablokuj bramkę (gate_lock)
 * 2. Sprawdź warunki (shutdown, departing, miejsce)
 * 3. Jeśli OK, zwiększ liczniki (passengers, bikes)
 * 4. Odblokuj bramkę (gate_unlock)
 * 
 * Dzięki temu nie może być sytuacji race condition gdzie dwóch
 * pasażerów jednocześnie sprawdza miejsce i obaj wchodzą przekraczając limit.
 */
int try_board(int bike, int with_child, int vip) {
    (void)vip;  // Parametr vip obecnie nieużywany
    int gate = bike ? 2 : 1;  // Wybierz bramkę: 2 jeśli rower, 1 jeśli bez
    
    // ATOMOWA operacja: lock gate -> sprawdź warunki -> wsiądź/odrzuć -> unlock
    gate_lock(gate);

    // Odczytaj stan systemu (chronione mutexem)
    sem_lock();
    int sd = bus->shutdown;  // Czy system się wyłącza?
    int sb = bus->station_blocked;  // Czy dworzec zablokowany?
    int dep = bus->departing;  // Czy autobus odjeżdża?
    int pass = bus->passengers;  // Ile pasażerów już w autobusie?
    int bks = bus->bikes;  // Ile rowerów już w autobusie?
    int P = bus->P;  // Limit pasażerów
    int R = bus->R;  // Limit rowerów

    // Nie możemy wsiąść - system zamknięty lub autobus odjeżdża
    if (sd || sb ) {
        sem_unlock();
        gate_unlock(gate);
        return 0;  // System się wyłącza - kończymy proces
    }

    // Sprawdzamy czy jest miejsce
    int needed_seats = with_child ? 2 : 1;  // Rodzic + dziecko = 2 miejsca
    int needed_bikes = bike ? 1 : 0;

    if (pass + needed_seats > P || bks + needed_bikes > R || dep) {
        // Brak miejsca w autobusie
        sem_unlock();
        gate_unlock(gate);
        return -1;  // Brak miejsca - czekamy na następny autobus
    }

    // Wchodzimy - ATOMOWO zwiększamy liczniki
    bus->passengers += needed_seats;
    bus->bikes += needed_bikes;
    sem_unlock();

    gate_unlock(gate);
    return 1;  // Sukces - wsiedliśmy
}

int main() {
    // === INICJALIZACJA IPC ===
    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1) {
        perror("ftok");
        return 1;
    }

    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 4, 0600);
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

    // === GENEROWANIE LOSOWYCH CECH PASAŻERA ===
    // Inicjalizacja generatora liczb losowych (unikalny seed dla każdego pasażera)
    srand((unsigned)(getpid() ^ time(NULL)));

    int vip = (rand() % 100 == 0);  // 1% szans na VIP
    int bike = rand() % 2;  // 50% szans na rower
    int age = rand() % 80;  // Wiek 0-79
    int with_child = (age >= 18 && rand() % 5 == 0);  // 20% dorosłych ma dziecko

    // Bufory na logi
    char b[64];
    char ln[256];

    // === LOGOWANIE PRZYBYCIA ===
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Przybycie (VIP=%d wiek=%d rower=%d dziecko=%d)\n", 
             b, getpid(), vip, age, bike, with_child);
    log_write(ln);

    // === SPRAWDZENIE CZY DWORZEC JEST OTWARTY ===
    sem_lock();
    int sb = bus->station_blocked;
    int sd = bus->shutdown;
    sem_unlock();

    if (sb || sd) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Dworzec zamkniety\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;  // Zmniejsz licznik aktywnych pasażerów
        sem_unlock();
        shmdt(bus);
        return 0;
    }

    // === ODRZUCENIE DZIECI BEZ OPIEKUNA ===
    // Dzieci poniżej 8 lat nie mogą podróżować same
    if (age < 8) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [DZIECKO %d] Bez opiekuna - odmowa\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        shmdt(bus);
        return 0;
    }

    // === REJESTRACJA W KASIE ===
    struct msg m;
    m.type = MSG_REGISTER;  // Typ komunikatu: rejestracja
    m.pid = getpid();  // Nasz PID
    m.vip = vip;  // Status VIP
    m.bike = bike;  // Czy mamy rower
    m.child = 0;  // To nie jest dziecko (dzieci < 8 już odrzucone)
    m.ticket_ok = vip ? 1 : 0;  // VIP automatycznie ma OK

    // Sprawdź shutdown przed wysłaniem
    sem_lock();
    sd = bus->shutdown;
    sb = bus->station_blocked;
    sem_unlock();

    if (sd || sb) {
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Dworzec zamkniety przed rejestracją\n", b, getpid());
        log_write(ln);
        sem_lock();
        bus->active_passengers--;
        sem_unlock();
        shmdt(bus);
        return 0;
    }

    // Wysłanie komunikatu rejestracyjnego do kasjera
    if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
        perror("msgsnd register");
    }

    // === CZEKANIE NA BILET (JEŚLI NIE VIP) ===
    if (!vip) {
        int got_ticket = 0;  // Flaga otrzymania biletu
        int no_msg_count = 0;  // Licznik pustych prób

        // Pętla oczekiwania na bilet
        for (;;) {
            long ticket_type = MSG_TICKET_REPLY + getpid();  // Unikalny typ dla naszego biletu
            ssize_t rr = msgrcv(msgid, &m, sizeof(m) - sizeof(long), ticket_type, IPC_NOWAIT);
            
            if (rr >= 0) {
                // Otrzymaliśmy bilet
                got_ticket = 1;
                break;
            }

            if (errno != ENOMSG) {
                // Błąd inny niż "brak wiadomości"
                perror("msgrcv ticket");
                break;
            }

            // Sprawdź czy system się nie wyłącza
            sem_lock();
            sd = bus->shutdown;
            sb = bus->station_blocked;
            sem_unlock();

            if (sd || sb) break;  // Jeśli shutdown, przerwij czekanie

            // Kontrola częstotliwości sprawdzania
            no_msg_count++;
            if (no_msg_count > 100) {
                sleep(1);  // Zaśnij jeśli długo czekamy
                no_msg_count = 0;
            }
        }

        // Jeśli nie dostaliśmy biletu, kończymy
        if (!got_ticket || !m.ticket_ok) {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Brak biletu\n", b, getpid());
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }
    }

    // === OBSŁUGA PASAŻERA Z DZIECKIEM ===
    if (with_child) {
        // Tworzymy pipe do synchronizacji z procesem dziecka
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            perror("pipe");
        }

        // Zwiększ licznik aktywnych pasażerów (dla dziecka)
        sem_lock();
        bus->active_passengers++;
        sem_unlock();

        // Fork - tworzenie procesu dziecka
        pid_t cpid = fork();
        if (cpid == -1) {
            perror("fork child");
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
        }
        else if (cpid == 0) {
            // === KOD PROCESU DZIECKA ===
            // Proces dziecka - NIE rejestruje się w kasie, tylko czeka na rodzica
            close(pipefd[1]);  // Zamknij koniec do zapisu

            // Czekaj na sygnał od rodzica
            char bufc;
            read(pipefd[0], &bufc, 1);  // Blokuje do otrzymania danych
            close(pipefd[0]);

            // Dziecko wchodzi przez bramkę bez roweru (synchronicznie z rodzicem)
            // Tylko po to żeby przejść przez gate - liczniki już zwiększone przez rodzica
            gate_lock(1);
            gate_unlock(1);

            // Zmniejsz licznik aktywnych pasażerów
            sem_lock();
            bus->active_passengers--;
            sem_unlock();

            shmdt(bus);
            return 0;  // Koniec procesu dziecka
        }
        else {
            // === KOD PROCESU RODZICA ===
            close(pipefd[0]);  // Zamknij koniec do odczytu

            // Próbujemy wsiąść (rodzic + dziecko razem)
            for (;;) {
                int result = try_board(bike, 1, vip);  // with_child = 1

                if (result == 0) {
                    // System się wyłącza
                    close(pipefd[1]);
                    waitpid(cpid, NULL, 0);  // Poczekaj na dziecko
                    sem_lock();
                    bus->active_passengers -= 2;  // Rodzic + dziecko
                    sem_unlock();
                    shmdt(bus);
                    return 0;
                }

                if (result == 1) {
                    // Sukces - wsiedliśmy!
                    write(pipefd[1], "X", 1);  // Powiadom dziecko
                    close(pipefd[1]);
                    waitpid(cpid, NULL, 0);  // Poczekaj aż dziecko przejdzie przez gate

                    ts(b, sizeof(b));
                    snprintf(ln, sizeof(ln), "[%s] [DOROSLY+DZIECKO %d] Wsiadl (VIP=%d rower=%d)\n", 
                             b, getpid(), vip, bike);
                    log_write(ln);

                    sem_lock();
                    bus->active_passengers -= 2;  // Zmniejsz licznik o 2
                    sem_unlock();
                    shmdt(bus);
                    return 0;
                }

                // Brak miejsca (result == -1) - czekamy na następny autobus
                sleep(1);

                // Sprawdź shutdown
                sem_lock();
                sd = bus->shutdown;
                sb = bus->station_blocked;
                sem_unlock();

                if (sd || sb) {
                    close(pipefd[1]);
                    waitpid(cpid, NULL, 0);
                    sem_lock();
                    bus->active_passengers -= 2;
                    sem_unlock();
                    shmdt(bus);
                    return 0;
                }
            }
        }
    }

    // === ZWYKŁY PASAŻER - PRÓBUJEMY WSIĄŚĆ ===
    for (;;) {
        int result = try_board(bike, 0, vip);  // with_child = 0

        if (result == 0) {
            // System się wyłącza
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] System zamkniety\n", b, getpid());
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }

        if (result == 1) {
            // Sukces - wsiedliśmy
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Wsiadl (VIP=%d rower=%d)\n", 
                     b, getpid(), vip, bike);
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }

        // Brak miejsca (result == -1) - czekamy na następny autobus
        sleep(1);

        // Sprawdź shutdown podczas oczekiwania
        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            ts(b, sizeof(b));
            snprintf(ln, sizeof(ln), "[%s] [PASAZER %d] Dworzec zamkniety podczas oczekiwania\n", 
                     b, getpid());
            log_write(ln);
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
            shmdt(bus);
            return 0;
        }
    }

    return 0;
}
