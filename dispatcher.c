/*
 * DISPATCHER.C - Proces Dyspozytora
 * 
 * Ten proces odpowiada za zarządzanie ruchem autobusów poprzez obsługę sygnałów.
 * Główne zadania:
 * - Wymuszanie odjazdu autobusu (sygnał SIGUSR1)
 * - Blokowanie dworca i zamykanie systemu (sygnał SIGUSR2)
 * - Obsługa przerwania SIGINT (Ctrl+C)
 * 
 * Dyspozytor nie wykonuje aktywnych operacji - działa reaktywnie,
 * reagując tylko na otrzymane sygnały.
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include "ipc.h"

// Globalne zmienne
int shmid;  // ID pamięci dzielonej
struct BusState* bus;  // Wskaźnik do stanu systemu
volatile sig_atomic_t should_exit = 0;  // Flaga zakończenia (volatile - może być zmieniana w handlerze)

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
 * 
 * Otwiera plik w trybie append, więc nie nadpisuje poprzednich wpisów
 */
void log_write(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) return;  // Jeśli nie można otworzyć pliku, po prostu wyjdź
    write(fd, s, strlen(s));  // Zapisz tekst
    close(fd);  // Zamknij plik
}

/*
 * Handler sygnału SIGINT (Ctrl+C)
 * Ustawia flagę should_exit aby zakończyć proces w kontrolowany sposób
 */
void handle_int(int sig) {
    (void)sig;  // Nie używamy parametru (unikamy ostrzeżenia kompilatora)
    should_exit = 1;  // Ustaw flagę zakończenia
}

/*
 * Handler sygnału SIGUSR1 - wymuszenie odjazdu autobusu
 * 
 * Gdy dyspozytor otrzyma SIGUSR1:
 * 1. Sprawdza czy jakiś kierowca jest aktualnie na dworcu (bus->driver_pid > 0)
 * 2. Jeśli tak, wysyła do niego sygnał SIGUSR1
 * 3. Kierowca otrzymując ten sygnał natychmiast odjeżdża (nie czekając T sekund)
 */
void handle_usr1(int sig) {
    (void)sig;  // Nie używamy parametru
    if (bus && bus->driver_pid > 0) {
        kill(bus->driver_pid, SIGUSR1);  // Wyślij SIGUSR1 do kierowcy
        char b[64];
        ts(b, sizeof(b));
        char ln[128];
        snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Wymuszenie odjazdu\n", b);
        log_write(ln);
    }
}

/*
 * Handler sygnału SIGUSR2 - blokada dworca i zamknięcie systemu
 * 
 * Gdy dyspozytor otrzyma SIGUSR2:
 * 1. Ustawia flagę station_blocked (nowi pasażerowie nie mogą wejść)
 * 2. Ustawia flagę shutdown (cały system zaczyna się wyłączać)
 * 3. Wysyła SIGUSR2 do kierowcy (jeśli jest na dworcu)
 * 4. Ustawia flagę should_exit aby zakończyć proces dyspozytora
 */
void handle_usr2(int sig) {
    (void)sig;  // Nie używamy parametru
    if (bus) {
        bus->station_blocked = 1;  // Zablokuj dworzec
        bus->shutdown = 1;  // Rozpocznij wyłączanie systemu
        if (bus->driver_pid > 0) {
            kill(bus->driver_pid, SIGUSR2);  // Powiadom kierowcę
        }
        char b[64];
        ts(b, sizeof(b));
        char ln[128];
        snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Blokada dworca\n", b);
        log_write(ln);
    }
    should_exit = 1;  // Zakończ proces dyspozytora
}

int main() {
    // === INICJALIZACJA KLUCZA IPC ===
    key_t shm_key = ftok(SHM_PATH, 'S');  // Generuj klucz dla pamięci dzielonej
    if (shm_key == -1) {
        perror("ftok shm");
        return 1;
    }

    // === UZYSKANIE DOSTĘPU DO PAMIĘCI DZIELONEJ ===
    // Dyspozytor NIE tworzy pamięci (bez IPC_CREAT), tylko się podłącza
    shmid = shmget(shm_key, sizeof(struct BusState), 0600);
    if (shmid == -1) {
        perror("shmget");
        return 1;
    }

    // === PODŁĄCZENIE DO PAMIĘCI DZIELONEJ ===
    bus = shmat(shmid, NULL, 0);  // Przyłącz segment pamięci do przestrzeni adresowej
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    // === LOGOWANIE STARTU ===
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Start pracy\n", b);
    log_write(ln);

    // === KONFIGURACJA HANDLERA SIGINT ===
    struct sigaction sai;
    memset(&sai, 0, sizeof(sai));  // Wyzeruj strukturę
    sai.sa_handler = handle_int;  // Ustaw funkcję obsługi
    sigemptyset(&sai.sa_mask);  // Pusta maska sygnałów (nie blokuj innych sygnałów)
    sai.sa_flags = SA_RESTART;  // Automatycznie wznawiaj przerwane wywołania systemowe
    sigaction(SIGINT, &sai, NULL);  // Zarejestruj handler

    // === KONFIGURACJA HANDLERA SIGUSR1 ===
    struct sigaction sa1;
    memset(&sa1, 0, sizeof(sa1));  // Wyzeruj strukturę
    sa1.sa_handler = handle_usr1;  // Ustaw funkcję obsługi
    sigemptyset(&sa1.sa_mask);  // Pusta maska sygnałów
    sa1.sa_flags = SA_RESTART;  // Automatycznie wznawiaj przerwane wywołania systemowe
    sigaction(SIGUSR1, &sa1, NULL);  // Zarejestruj handler

    // === KONFIGURACJA HANDLERA SIGUSR2 ===
    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));  // Wyzeruj strukturę
    sa2.sa_handler = handle_usr2;  // Ustaw funkcję obsługi
    sigemptyset(&sa2.sa_mask);  // Pusta maska sygnałów
    sa2.sa_flags = SA_RESTART;  // Automatycznie wznawiaj przerwane wywołania systemowe
    sigaction(SIGUSR2, &sa2, NULL);  // Zarejestruj handler

    // === GŁÓWNA PĘTLA DYSPOZYTORA ===
    // Proces czeka na sygnały używając pause()
    // pause() zawiesza proces do otrzymania sygnału
    while (!should_exit) {
        pause();  // Czekaj na sygnał (bardzo wydajne - nie zużywa CPU)
    }

    // === ZAKOŃCZENIE PRACY ===
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [DYSPOZYTOR] Koniec pracy\n", b);
    log_write(ln);

    shmdt(bus);  // Odłącz pamięć dzieloną
    return 0;
}
