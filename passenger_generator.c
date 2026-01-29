/*
 * PASSENGER_GENERATOR.C - Proces Generatora Pasażerów
 * 
 * Ten proces jest odpowiedzialny za ciągłe tworzenie nowych pasażerów.
 * Główne zadania:
 * - Czekanie losowego czasu (1-3 sekundy)
 * - Tworzenie nowego procesu pasażera (fork + exec)
 * - Inkrementacja licznika active_passengers przed utworzeniem pasażera
 * - Monitorowanie flag shutdown i station_blocked
 * - Automatyczne zbieranie zakończonych procesów potomnych
 * 
 * Generator działa w nieskończonej pętli do momentu otrzymania
 * sygnału shutdown lub station_blocked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include "ipc.h"

// Globalne ID zasobów IPC
int shmid, semid;
struct BusState* bus;

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
    struct sembuf sb = { 0, -1, SEM_UNDO };  // Operacja P (wait)
    semop(semid, &sb, 1);
}

/*
 * Funkcja sem_unlock - odblokowuje semafor mutex (sem[0])
 */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };  // Operacja V (signal)
    semop(semid, &sb, 1);
}

/*
 * Handler sygnału SIGCHLD
 * 
 * Automatycznie zbiera zakończone procesy potomne (pasażerów).
 * WNOHANG oznacza że waitpid nie blokuje jeśli nie ma zakończonych procesów.
 * 
 * Ten handler zapobiega powstawaniu procesów zombie - procesy które
 * się zakończyły ale ich wpis w tablicy procesów nie został usunięty
 * bo proces rodzica nie wywołał wait().
 */
void handle_sigchld(int sig) {
    (void)sig;  // Nie używamy parametru
    int saved_errno = errno;  // Zachowaj errno (handler może go zmienić)
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // Pętla zbiera wszystkie zakończone procesy
        // waitpid(-1, ...) czeka na dowolny proces potomny
        // WNOHANG powoduje natychmiastowy powrót jeśli brak zakończonych procesów
    }
    errno = saved_errno;  // Przywróć errno
}

int main(int argc, char** argv) {
    (void)argc;  // Nie używamy argumentów
    (void)argv;

    // === INICJALIZACJA IPC ===
    // Generuj klucze na podstawie ścieżek plików
    key_t shm_key = ftok(SHM_PATH, 'S');  // Klucz pamięci dzielonej
    key_t sem_key = ftok(SEM_PATH, 'E');  // Klucz semaforów

    if (shm_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    // Uzyskaj dostęp do zasobów IPC (bez tworzenia - IPC_CREAT)
    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    semid = semget(sem_key, 4, 0600);

    if (shmid == -1 || semid == -1) {
        perror("get ipc");
        return 1;
    }

    // Podłącz pamięć dzieloną
    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    // === KONFIGURACJA HANDLERA SIGCHLD ===
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));  // Wyzeruj strukturę
    sa_chld.sa_handler = handle_sigchld;  // Ustaw funkcję obsługi
    sigemptyset(&sa_chld.sa_mask);  // Pusta maska sygnałów
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    // SA_RESTART - automatycznie wznawiaj przerwane wywołania systemowe
    // SA_NOCLDSTOP - nie powiadamiaj o zatrzymanych procesach (tylko zakończonych)
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("sigaction SIGCHLD");
    }

    // === LOGOWANIE STARTU ===
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Start - tworzy pasazerow w nieskonczonosc\n", b);
    log_write(ln);

    // === INICJALIZACJA GENERATORA LICZB LOSOWYCH ===
    // Używamy aktualnego czasu jako seed
    srand((unsigned)time(NULL));

    // === GŁÓWNA PĘTLA GENERATORA ===
    for (;;) {
        // === FAZA 1: LOSOWY ODSTĘP ===
        // Losowy odstęp 1-3 sekundy między tworzeniem pasażerów
        int delay = 1 + (rand() % 3);  // [1, 3]
        sleep(delay);

        // === FAZA 2: SPRAWDZENIE SHUTDOWN ===
        // Sprawdź czy system się nie wyłącza
        sem_lock();
        int sd = bus->shutdown;  // Flaga shutdown
        int sb = bus->station_blocked;  // Flaga blokady dworca
        sem_unlock();

        if (sd || sb) {
            break;  // Jeśli system się wyłącza, zakończ generator
        }

        // === FAZA 3: INKREMENTACJA LICZNIKA AKTYWNYCH PASAŻERÓW ===
        // WAŻNE: Zwiększamy licznik PRZED utworzeniem procesu pasażera
        // Dzięki temu main może śledzić ile pasażerów jeszcze działa
        sem_lock();
        bus->active_passengers++;
        sem_unlock();

        // === FAZA 4: TWORZENIE PROCESU PASAŻERA ===
        pid_t p = fork();  // Utwórz nowy proces
        if (p == -1) {
            // Fork się nie powiódł
            perror("fork passenger");
            // Zmniejsz licznik bo pasażer nie został utworzony
            sem_lock();
            bus->active_passengers--;
            sem_unlock();
        }
        else if (p == 0) {
            // === KOD PROCESU POTOMNEGO (PASAŻERA) ===
            // Zastąp proces programem passenger
            execl("./passenger", "passenger", NULL);
            // Jeśli exec się nie powiedzie, wypisz błąd i zakończ
            perror("exec passenger");
            _exit(1);  // Użyj _exit (nie exit) w procesie potomnym
        }
        // === KOD PROCESU RODZICA (GENERATORA) ===
        // Proces rodzica kontynuuje pętlę i tworzy kolejnych pasażerów
        // Zakończone procesy pasażerów są automatycznie zbierane przez handle_sigchld
    }

    // === ZAKOŃCZENIE PRACY ===
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [GENERATOR] Koniec pracy\n", b);
    log_write(ln);

    shmdt(bus);  // Odłącz pamięć dzieloną
    return 0;
}
