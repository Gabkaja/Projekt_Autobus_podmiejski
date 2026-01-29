/*
 * MAIN.C - Proces Główny Systemu Autobusowego
 * 
 * Ten proces jest punktem startowym całego systemu.
 * Główne zadania:
 * - Parsowanie parametrów wiersza poleceń (N, P, R, T)
 * - Tworzenie zasobów IPC (pamięć dzielona, semafory, kolejka komunikatów)
 * - Inicjalizacja struktury BusState
 * - Uruchamianie wszystkich procesów potomnych:
 *   * N kierowców (driver)
 *   * 1 kasjer (cashier)
 *   * 1 dyspozytor (dispatcher)
 *   * 1 generator pasażerów (passenger_generator)
 * - Oczekiwanie na zakończenie wszystkich procesów
 * - Sprzątanie zasobów IPC po zakończeniu
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include "ipc.h"

// Globalne zmienne potrzebne do cleanup i obsługi sygnałów
int shmid, semid, msgid;  // ID zasobów IPC
struct BusState* bus;  // Wskaźnik do pamięci dzielonej
pid_t dispatcher_pid = 0;  // PID dyspozytora (do wysyłania sygnałów)

/*
 * Funkcja log_write - zapisuje wpis do pliku report.txt
 * Parametry:
 *   s - tekst do zapisania
 */
void log_write(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) {
        perror("open report");
        return;
    }
    if (write(fd, s, strlen(s)) == -1) {
        perror("write report");
    }
    if (close(fd) == -1) {
        perror("close report");
    }
}

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
 * Funkcja cleanup - usuwa wszystkie zasoby IPC
 * 
 * Wywoływana na końcu programu aby wyczyścić:
 * - Pamięć dzieloną (shmctl IPC_RMID)
 * - Semafory (semctl IPC_RMID)
 * - Kolejkę komunikatów (msgctl IPC_RMID)
 * - Pliki kluczy (unlink)
 */
void cleanup() {
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl IPC_RMID");
    }
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("semctl IPC_RMID");
    }
    if (msgctl(msgid, IPC_RMID, NULL) == -1) {
        perror("msgctl IPC_RMID");
    }
    // Usuń pliki kluczy
    unlink(SHM_PATH);
    unlink(SEM_PATH);
    unlink(MSG_PATH);
}

/*
 * Handler sygnału SIGINT (Ctrl+C)
 * 
 * Inicjuje kontrolowane zamknięcie systemu:
 * 1. Ustawia flagi shutdown i station_blocked w pamięci dzielonej
 * 2. Wysyła SIGINT do dyspozytora
 * 3. Loguje rozpoczęcie zamykania
 */
void handle_sigint(int sig) {
    (void)sig;
    if (bus) {
        bus->shutdown = 1;  // Rozpocznij wyłączanie systemu
        bus->station_blocked = 1;  // Zablokuj dworzec
    }
    
    if (dispatcher_pid > 0) {
        kill(dispatcher_pid, SIGINT);  // Powiadom dyspozytora
    }
    
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [MAIN] Shutdown initiated\n", b);
    log_write(ln);
}

/*
 * Handler sygnału SIGCHLD
 * 
 * Automatycznie zbiera zakończone procesy potomne używając waitpid z WNOHANG.
 * To zapobiega powstawaniu procesów zombie.
 * SA_NOCLDSTOP oznacza że nie chcemy być powiadamiani o zatrzymanych procesach.
 */
void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;  // Zachowaj errno (handler może go zmienić)
    while (waitpid(-1, NULL, WNOHANG) > 0);  // Zbierz wszystkie zakończone procesy
    errno = saved_errno;  // Przywróć errno
}

int main(int argc, char** argv) {
    // === PARSOWANIE ARGUMENTÓW WIERSZA POLECEŃ ===
    if (argc < 5) {
        fprintf(stderr, "Uzycie: %s N P R T\n", argv[0]);
        fprintf(stderr, "  N - liczba autobusow\n");
        fprintf(stderr, "  P - maksymalna liczba pasazerow w autobusie\n");
        fprintf(stderr, "  R - maksymalna liczba rowerow w autobusie\n");
        fprintf(stderr, "  T - czas oczekiwania na dworcu (sekundy)\n");
        return EXIT_FAILURE;
    }

    // Konwersja argumentów na liczby całkowite
    int N = atoi(argv[1]);  // Liczba autobusów
    int P = atoi(argv[2]);  // Maksymalna liczba pasażerów
    int R = atoi(argv[3]);  // Maksymalna liczba rowerów
    int T = atoi(argv[4]);  // Czas oczekiwania na dworcu

    // Walidacja parametrów
    if (N <= 0 || P <= 0 || R < 0 || T <= 0) {
        fprintf(stderr, "Niepoprawne parametry\n");
        return EXIT_FAILURE;
    }

    // === TWORZENIE PLIKU RAPORTU ===
    // Tworzymy pusty plik report.txt (lub czyścimy istniejący)
    int fdrep = creat("report.txt", 0600);
    if (fdrep == -1) {
        perror("creat report");
        return EXIT_FAILURE;
    }
    close(fdrep);

    // === TWORZENIE PLIKÓW KLUCZY IPC ===
    // Te pliki są potrzebne przez ftok() do generowania kluczy
    creat(SHM_PATH, 0600);
    creat(SEM_PATH, 0600);
    creat(MSG_PATH, 0600);

    // === GENEROWANIE KLUCZY IPC ===
    // ftok() generuje unikalny klucz na podstawie ścieżki pliku i znaku
    key_t shm_key = ftok(SHM_PATH, 'S');  // Klucz dla pamięci dzielonej
    key_t sem_key = ftok(SEM_PATH, 'E');  // Klucz dla semaforów
    key_t msg_key = ftok(MSG_PATH, 'M');  // Klucz dla kolejki komunikatów

    if (shm_key == -1 || sem_key == -1 || msg_key == -1) {
        perror("ftok");
        cleanup();
        return EXIT_FAILURE;
    }

    // === TWORZENIE PAMIĘCI DZIELONEJ ===
    // IPC_CREAT tworzy nowy segment jeśli nie istnieje
    shmid = shmget(shm_key, sizeof(struct BusState), IPC_CREAT | 0600);
    if (shmid == -1) {
        perror("shmget");
        cleanup();
        return EXIT_FAILURE;
    }

    // Podłączenie pamięci dzielonej do przestrzeni adresowej procesu
    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        cleanup();
        return EXIT_FAILURE;
    }

    // === TWORZENIE SEMAFORÓW ===
    // Tworzymy zestaw 4 semaforów:
    // [0] - mutex do ochrony pamięci dzielonej
    // [1] - gate bez roweru (kontroluje wejście pasażerów bez rowerów)
    // [2] - gate z rowerem (kontroluje wejście pasażerów z rowerami)
    // [3] - dworzec (zapewnia że tylko jeden autobus jest na dworcu)
    semid = semget(sem_key, 4, IPC_CREAT | 0600);
    if (semid == -1) {
        perror("semget");
        cleanup();
        return EXIT_FAILURE;
    }

    // Inicjalizacja wartości semaforów (wszystkie = 1, czyli odblokowane)
    semctl(semid, 0, SETVAL, 1);  // mutex
    semctl(semid, 1, SETVAL, 1);  // gate bez roweru
    semctl(semid, 2, SETVAL, 1);  // gate z rowerem
    semctl(semid, 3, SETVAL, 1);  // dworzec (tylko jeden autobus)

    // === TWORZENIE KOLEJKI KOMUNIKATÓW ===
    // Używana do komunikacji pasażer <-> kasjer
    msgid = msgget(msg_key, IPC_CREAT | 0600);
    if (msgid == -1) {
        perror("msgget");
        cleanup();
        return EXIT_FAILURE;
    }

    // === INICJALIZACJA STANU SYSTEMU ===
    // Ustawiamy wszystkie wartości w pamięci dzielonej
    bus->P = P;  // Maksymalna liczba pasażerów
    bus->R = R;  // Maksymalna liczba rowerów
    bus->T = T;  // Czas oczekiwania
    bus->N = N;  // Liczba autobusów
    bus->passengers = 0;  // Obecnie brak pasażerów w autobusie
    bus->bikes = 0;  // Obecnie brak rowerów w autobusie
    bus->departing = 0;  // Autobus nie odjeżdża
    bus->station_blocked = 0;  // Dworzec otwarty
    bus->active_passengers = 0;  // Brak aktywnych pasażerów
    bus->boarded_passengers = 0;  // Nikt jeszcze nie wsiadł
    bus->driver_pid = 0;  // Brak kierowcy na dworcu
    bus->shutdown = 0;  // System włączony

    // === KONFIGURACJA OBSŁUGI SYGNAŁÓW ===
    
    // Handler SIGINT (Ctrl+C)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    // Handler SIGCHLD (automatyczne zbieranie procesów zombie)
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;  // SA_NOCLDSTOP = ignoruj zatrzymane procesy
    sigaction(SIGCHLD, &sa_chld, NULL);

    // === LOGOWANIE STARTU SYSTEMU ===
    char b[64];
    ts(b, sizeof(b));
    char ln[256];
    snprintf(ln, sizeof(ln), "[%s] [MAIN] Start systemu: N=%d P=%d R=%d T=%d\n", 
             b, N, P, R, T);
    log_write(ln);

    // === TWORZENIE KIEROWCÓW (N AUTOBUSÓW) ===
    for (int i = 0; i < N; i++) {
        pid_t p = fork();  // Utwórz proces potomny
        if (p == -1) {
            perror("fork driver");
        }
        else if (p == 0) {
            // Kod procesu potomnego
            execl("./driver", "driver", NULL);  // Zastąp proces programem driver
            perror("exec driver");  // Jeśli exec się nie powiedzie
            _exit(1);  // _exit (nie exit) w procesie potomnym
        }
        // Proces rodzica kontynuuje pętlę
    }

    // === TWORZENIE KASJERA ===
    pid_t p1 = fork();
    if (p1 == -1) {
        perror("fork cashier");
    }
    else if (p1 == 0) {
        execl("./cashier", "cashier", NULL);
        perror("exec cashier");
        _exit(1);
    }

    // === TWORZENIE DYSPOZYTORA ===
    pid_t p2 = fork();
    if (p2 == -1) {
        perror("fork dispatcher");
    }
    else if (p2 == 0) {
        execl("./dispatcher", "dispatcher", NULL);
        perror("exec dispatcher");
        _exit(1);
    }
    dispatcher_pid = p2;  // Zapisz PID dyspozytora (potrzebne do wysyłania sygnałów)

    // === TWORZENIE GENERATORA PASAŻERÓW ===
    pid_t p3 = fork();
    if (p3 == -1) {
        perror("fork generator");
    }
    else if (p3 == 0) {
        execl("./passenger_generator", "passenger_generator", NULL);
        perror("exec passenger_generator");
        _exit(1);
    }

    // === OCZEKIWANIE NA ZAKOŃCZENIE WSZYSTKICH PROCESÓW ===
    // wait(NULL) czeka na zakończenie dowolnego procesu potomnego
    // Pętla kontynuuje dopóki są jakieś procesy potomne
    while (wait(NULL) > 0);

    // === LOGOWANIE ZAKOŃCZENIA ===
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [MAIN] System zakonczony\n", b);
    log_write(ln);

    // === ODŁĄCZENIE PAMIĘCI DZIELONEJ ===
    if (shmdt(bus) == -1) {
        perror("shmdt");
    }

    // === SPRZĄTANIE ZASOBÓW IPC ===
    cleanup();
    return 0;
}
