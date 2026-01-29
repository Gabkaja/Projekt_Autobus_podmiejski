/*
 * DRIVER.C - Proces Kierowcy Autobusu
 * 
 * Ten proces symuluje kierowcę autobusu. Każdy autobus ma swojego kierowcę.
 * Główne zadania:
 * - Przybywanie na dworzec i czekanie T sekund (lub na sygnał od dyspozytora)
 * - Odbieranie pasażerów (pasażerowie sami wchodzą przez bramki)
 * - Odjazd z pasażerami
 * - Podróż trwająca losowo 3-9 sekund
 * - Powrót na dworzec i powtórzenie cyklu
 * 
 * Synchronizacja:
 * - Semafor gate[3] zapewnia że tylko jeden autobus jest na dworcu
 * - Semafory gate[1] i gate[2] kontrolują dostęp pasażerów
 * - Flaga departing informuje pasażerów że autobus zaraz odjeżdża
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "ipc.h"

// Globalne zmienne
int shmid, semid;  // ID zasobów IPC
struct BusState* bus;  // Wskaźnik do stanu systemu
volatile sig_atomic_t force_flag = 0;  // Flaga wymuszonego odjazdu

/*
 * Funkcja ts (timestamp) - generuje aktualny znacznik czasu
 * Parametry:
 *   buf - bufor na wynik w formacie HH:MM:SS
 *   n - rozmiar bufora
 */
void ts(char* buf, size_t n) {
    time_t t = time(NULL);  // Pobierz aktualny czas systemowy
    struct tm* tm_info = localtime(&t);  // Konwertuj na czas lokalny
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");  // Wartość domyślna w razie błędu
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);  // Formatuj jako HH:MM:SS
}

/*
 * Funkcja log_write - zapisuje wpis do pliku report.txt
 * Parametry:
 *   s - tekst do zapisania
 */
void log_write(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;
    write(fd, s, strlen(s));
    close(fd);
}

/*
 * Funkcja sem_lock - blokuje semafor mutex (sem[0])
 * Używana do zapewnienia wyłącznego dostępu do pamięci dzielonej
 */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };  // Operacja P (wait) na semaforze 0
    semop(semid, &sb, 1);
}

/*
 * Funkcja sem_unlock - odblokowuje semafor mutex (sem[0])
 */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };  // Operacja V (signal) na semaforze 0
    semop(semid, &sb, 1);
}

/*
 * Funkcja gate_lock - blokuje określoną bramkę
 * Parametry:
 *   gate - numer bramki (1 = bez roweru, 2 = z rowerem, 3 = dworzec)
 * 
 * Używana przez kierowcę do:
 * - Zablokowania dworca (gate=3) przed wjazdem
 * - Zablokowania wejść pasażerów (gate=1, gate=2) przed odjazdem
 */
void gate_lock(int gate) {
    struct sembuf sb = { gate, -1, SEM_UNDO };  // Operacja P na semaforze 'gate'
    semop(semid, &sb, 1);
}

/*
 * Funkcja gate_unlock - odblokowuje określoną bramkę
 * Parametry:
 *   gate - numer bramki
 */
void gate_unlock(int gate) {
    struct sembuf sb = { gate, 1, SEM_UNDO };  // Operacja V na semaforze 'gate'
    semop(semid, &sb, 1);
}

/*
 * Handler sygnału SIGUSR1 - wymuszony odjazd
 * Dyspozytor wysyła ten sygnał aby zmusić autobus do natychmiastowego odjazdu
 * (bez czekania pełnych T sekund)
 */
void handle_usr1(int sig) {
    (void)sig;
    force_flag = 1;  // Ustaw flagę wymuszonego odjazdu
}

/*
 * Handler sygnału SIGUSR2 - blokada dworca
 * Dyspozytor wysyła ten sygnał aby zablokować dworzec
 */
void handle_usr2(int sig) {
    (void)sig;
    sem_lock();
    bus->station_blocked = 1;  // Ustaw flagę blokady dworca
    sem_unlock();
}

/*
 * Handler sygnału SIGINT - rozpoczęcie zamykania systemu
 */
void handle_int(int sig) {
    (void)sig;
    sem_lock();
    bus->shutdown = 1;  // Rozpocznij wyłączanie
    bus->station_blocked = 1;  // Zablokuj dworzec
    sem_unlock();
}

int main() {
    // === INICJALIZACJA KLUCZY IPC ===
    key_t shm_key = ftok(SHM_PATH, 'S');  // Klucz pamięci dzielonej
    key_t sem_key = ftok(SEM_PATH, 'E');  // Klucz semaforów

    if (shm_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    // === UZYSKANIE DOSTĘPU DO ZASOBÓW IPC ===
    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 4, 0600);

    if (shmid == -1 || semid == -1) {
        perror("get ipc");
        return 1;
    }

    // === PODŁĄCZENIE DO PAMIĘCI DZIELONEJ ===
    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    // === KONFIGURACJA HANDLERÓW SYGNAŁÓW ===
    
    // Handler SIGUSR1 (wymuszony odjazd)
    struct sigaction sa1;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = handle_usr1;
    sigemptyset(&sa1.sa_mask);
    sa1.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa1, NULL);

    // Handler SIGUSR2 (blokada dworca)
    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = handle_usr2;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa2, NULL);

    // Handler SIGINT (Ctrl+C)
    struct sigaction sai;
    memset(&sai, 0, sizeof(sai));
    sai.sa_handler = handle_int;
    sigemptyset(&sai.sa_mask);
    sai.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sai, NULL);

    // === INICJALIZACJA GENERATORA LICZB LOSOWYCH ===
    // Używamy PID i czasu aby każdy kierowca miał inne losowe czasy podróży
    srand((unsigned)(getpid() ^ time(NULL)));

    // === LOGOWANIE STARTU ===
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Start pracy\n", b, getpid());
    log_write(ln);

    // === GŁÓWNA PĘTLA KIEROWCY ===
    for (;;) {
        // === FAZA 1: PRZYBYCIE NA DWORZEC ===
        // Tylko jeden autobus na dworcu - gate[3]
        // Semafor gate[3] zapewnia że tylko jeden autobus może być na dworcu
        gate_lock(3);

        // Zapisz swój PID jako aktualny kierowca i zresetuj flagę departing
        sem_lock();
        bus->driver_pid = getpid();  // Zapisz PID kierowcy
        bus->departing = 0;  // Autobus jeszcze nie odjeżdża
        int sb = bus->station_blocked;  // Odczytaj flagę blokady
        int sd = bus->shutdown;  // Odczytaj flagę shutdown
        int wait_time = bus->T;  // Odczytaj czas oczekiwania
        sem_unlock();

        // Kończymy TYLKO gdy shutdown lub station_blocked
        if (sd || sb) {
            gate_unlock(3);  // Zwolnij dworzec
            break;  // Zakończ pracę
        }

        // Loguj przybycie
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Autobus na dworcu\n", b, getpid());
        log_write(ln);

        // === FAZA 2: OCZEKIWANIE NA PASAŻERÓW ===
        // Czekamy T sekund lub na sygnał od dyspozytora (SIGUSR1)
        int waited = 0;  // Licznik oczekiwanych sekund
        while (!force_flag && waited < wait_time) {
            sleep(1);  // Czekaj 1 sekundę
            waited++;

            // Sprawdź czy system się nie wyłącza
            sem_lock();
            sd = bus->shutdown;
            sb = bus->station_blocked;
            sem_unlock();

            if (sd || sb) break;  // Jeśli shutdown, przerwij czekanie
        }

        // Ponownie sprawdź shutdown po zakończeniu czekania
        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) {
            gate_unlock(3);  // Zwolnij dworzec
            break;  // Zakończ pracę
        }

        force_flag = 0;  // Zresetuj flagę wymuszonego odjazdu

        // === FAZA 3: PRZYGOTOWANIE DO ODJAZDU ===
        // Blokujemy wejścia - pasażerowie nie mogą już wchodzić
        gate_lock(1);  // Zablokuj bramkę bez roweru
        gate_lock(2);  // Zablokuj bramkę z rowerem

        // Ustaw flagę departing i odczytaj liczbę pasażerów/rowerów
        sem_lock();
        bus->departing = 1;  // Autobus odjeżdża
        int p = bus->passengers;  // Liczba pasażerów
        int r = bus->bikes;  // Liczba rowerów
        bus->boarded_passengers += p;  // Zwiększ całkowitą liczbę przewiezionych
        sem_unlock();

        // Loguj odjazd
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Odjazd: %d pasazerow, %d rowerow\n", 
                 b, getpid(), p, r);
        log_write(ln);

        // === FAZA 4: RESET LICZNIKÓW ===
        // Reset liczników - autobus opuszcza dworzec pusty dla następnego cyklu
        sem_lock();
        bus->passengers = 0;  // Wyzeruj pasażerów
        bus->bikes = 0;  // Wyzeruj rowery
        sem_unlock();

        // Odblokowujemy wejścia i dworzec - teraz może przyjechać następny autobus
        gate_unlock(1);  // Odblokuj bramkę bez roweru
        gate_unlock(2);  // Odblokuj bramkę z rowerem
        gate_unlock(3);  // Zwolnij dworzec dla następnego autobusu

        // === FAZA 5: PODRÓŻ ===
        // Jazda (losowy czas 3-9s) - symulacja przewożenia pasażerów
        int Ti = (rand() % 7) + 3;  // Losowy czas z zakresu [3, 9]
        sleep(Ti);  // Symuluj jazdę

        // Loguj powrót
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Powrot po %ds\n", b, getpid(), Ti);
        log_write(ln);

        // === FAZA 6: SPRAWDZENIE SHUTDOWN PO POWROCIE ===
        sem_lock();
        sd = bus->shutdown;
        sb = bus->station_blocked;
        sem_unlock();

        if (sd || sb) break;  // Jeśli shutdown, nie wracaj na dworzec

        // Jeśli nie ma shutdown, pętla się powtarza - autobus wraca na dworzec
    }

    // === ZAKOŃCZENIE PRACY ===
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [KIEROWCA %d] Koniec pracy\n", b, getpid());
    log_write(ln);

    shmdt(bus);  // Odłącz pamięć dzieloną
    return 0;
}
