/*
 * CASHIER.C - Proces Kasjera
 * 
 * Ten proces odpowiada za rejestrację pasażerów i wydawanie biletów.
 * Główne zadania:
 * - Odbieranie zgłoszeń rejestracyjnych od pasażerów (przez kolejkę komunikatów)
 * - Wydawanie biletów pasażerom niebędącym VIP i nie będącym dziećmi
 * - Logowanie wszystkich rejestracji do pliku report.txt
 * - Monitorowanie flagi shutdown, aby wiedzieć kiedy zakończyć pracę
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ipc.h"

// Globalne ID zasobów IPC
int shmid, msgid, semid;
struct BusState* bus;  // Wskaźnik do pamięci dzielonej

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
 * Funkcja sem_lock - blokuje semafor mutex (sem[0])
 * Używana do zapewnienia wyłącznego dostępu do pamięci dzielonej
 */
void sem_lock() {
    struct sembuf sb = { 0, -1, SEM_UNDO };  // Operacja P (wait) na semaforze 0
    semop(semid, &sb, 1);  // Wykonaj operację semafora
}

/*
 * Funkcja sem_unlock - odblokowuje semafor mutex (sem[0])
 * Zwalnia dostęp do pamięci dzielonej
 */
void sem_unlock() {
    struct sembuf sb = { 0, 1, SEM_UNDO };  // Operacja V (signal) na semaforze 0
    semop(semid, &sb, 1);  // Wykonaj operację semafora
}

int main() {
    // === INICJALIZACJA KLUCZY IPC ===
    // Generowanie kluczy na podstawie ścieżek plików i liter identyfikujących
    key_t shm_key = ftok(SHM_PATH, 'S');  // Klucz dla pamięci dzielonej
    key_t msg_key = ftok(MSG_PATH, 'M');  // Klucz dla kolejki komunikatów
    key_t sem_key = ftok(SEM_PATH, 'E');  // Klucz dla semaforów

    if (shm_key == -1 || msg_key == -1 || sem_key == -1) {
        perror("ftok");
        return 1;
    }

    // === UZYSKANIE DOSTĘPU DO ZASOBÓW IPC ===
    // Kasjer NIE tworzy zasobów (bez IPC_CREAT), tylko się do nich podłącza
    shmid = shmget(shm_key, sizeof(struct BusState), 0600);  // Pamięć dzielona
    msgid = msgget(msg_key, 0600);  // Kolejka komunikatów
    semid = semget(sem_key, 4, 0600);  // Zestaw 4 semaforów

    if (shmid == -1 || msgid == -1 || semid == -1) {
        perror("get ipc");
        return 1;
    }

    // === PODŁĄCZENIE DO PAMIĘCI DZIELONEJ ===
    bus = shmat(shmid, NULL, 0);  // Przyłącz segment pamięci do przestrzeni adresowej procesu
    if (bus == (void*)-1) {
        perror("shmat");
        return 1;
    }

    // === LOGOWANIE STARTU ===
    char b[64];  // Bufor na znacznik czasu
    ts(b, sizeof(b));
    char ln[128];  // Bufor na linię logu
    snprintf(ln, sizeof(ln), "[%s] [KASA] Start pracy\n", b);
    log_write(ln);

    // === GŁÓWNA PĘTLA KASJERA ===
    int no_msg_count = 0;  // Licznik pustych prób odbierania wiadomości
    for (;;) {
        // NAJPIERW sprawdzamy shutdown PRZED odbieraniem wiadomości
        // To zapewnia szybkie zakończenie gdy system się wyłącza
        sem_lock();  // Zablokuj dostęp do pamięci dzielonej
        int sd = bus->shutdown;  // Odczytaj flagę shutdown
        sem_unlock();  // Odblokuj dostęp

        if (sd) {
            break;  // Jeśli shutdown=1, kończymy pracę
        }

        // === ODBIERANIE ZGŁOSZENIA REJESTRACYJNEGO ===
        struct msg m;  // Struktura na komunikat
        // msgrcv odbiera komunikat typu MSG_REGISTER
        // IPC_NOWAIT - nie czekaj jeśli brak komunikatów (tryb nieblokujący)
        ssize_t r = msgrcv(msgid, &m, sizeof(m) - sizeof(long), MSG_REGISTER, IPC_NOWAIT);

        if (r < 0) {
            if (errno == ENOMSG) {
                // Brak komunikatów w kolejce - to normalna sytuacja
                no_msg_count++;

                // Jeśli przez dłuższy czas brak komunikatów, zaśnij na chwilę
                // To zmniejsza zużycie CPU podczas bezczynności
                if (no_msg_count > 100) {
                    sleep(1);
                    no_msg_count = 0;
                }
                continue;  // Sprawdź ponownie
            }
            else {
                // Inny błąd niż ENOMSG
                perror("msgrcv");
                break;
            }
        }

        // === ODEBRALIŚMY KOMUNIKAT ===
        no_msg_count = 0;  // Resetuj licznik

        // Loguj rejestrację pasażera
        ts(b, sizeof(b));
        snprintf(ln, sizeof(ln), "[%s] [KASA] Rejestracja PID=%d VIP=%d DZIECKO=%d\n",
                 b, m.pid, m.vip, m.child);
        log_write(ln);

        // === WYDAWANIE BILETÓW ===
        // Wysyłamy bilet tylko dla nie-VIP dorosłych (nie dzieci)
        // VIP nie potrzebują biletów (darmowy przejazd)
        // Dzieci wchodzą z rodzicem i nie dostają osobnych biletów
        if (!m.vip && !m.child) {
            m.ticket_ok = 1;  // Ustaw flagę "bilet OK"
            m.type = MSG_TICKET_REPLY + m.pid;  // Typ wiadomości = MSG_TICKET_REPLY + PID pasażera
            // Wysyłanie biletu do pasażera
            if (msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0) == -1) {
                perror("msgsnd reply");
            }
        }
        // VIP i dzieci nie dostają osobnych biletów (dzieci wchodzą z rodzicem)
    }

    // === ZAKOŃCZENIE PRACY ===
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [KASA] Koniec pracy\n", b);
    log_write(ln);

    shmdt(bus);  // Odłącz pamięć dzieloną
    return 0;
}
