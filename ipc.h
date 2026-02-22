/*
 * ipc.h – Wspólne definicje mechanizmów IPC dla całego projektu.
 *
 * Plik nagłówkowy zawiera:
 *   - Ścieżki plików używanych do generowania kluczy IPC (ftok).
 *   - Typy wiadomości dla kolejek komunikatów.
 *   - Stałe ograniczeń systemu.
 *   - Strukturę BusState przechowywaną w pamięci dzielonej.
 *   - Strukturę wiadomości przekazywanych między procesami.
 */

#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

/*
 * Pliki-tokeny używane przez ftok() do generowania kluczy IPC.
 * Każdy zasób IPC (pamięć dzielona, semafory, kolejki) musi mieć
 * unikalny klucz, dlatego każdy plik ma przypisaną inną literę projektu.
 */
#define SHM_PATH       "bus_shm.key"       /* Klucz pamięci dzielonej          */
#define SEM_PATH       "bus_sem.key"       /* Klucz zestawu semaforów          */
#define MSG_PATH       "bus_msg.key"       /* Klucz kolejki żądań (pasażer → kasjer) */
#define MSG_REPLY_PATH "bus_msg_reply.key" /* Klucz kolejki odpowiedzi (kasjer → pasażer) */

/*
 * Typy wiadomości w kolejce żądań (MSG_PATH).
 * Pasażer wysyła wiadomość MSG_REGISTER, aby zarejestrować się u kasjera.
 */
#define MSG_REGISTER     1

/*
 * Typy wiadomości w kolejce odpowiedzi (MSG_REPLY_PATH).
 * Kasjer odsyła bilet z typem MSG_TICKET_BASE + PID pasażera,
 * dzięki czemu każdy pasażer odbiera wyłącznie własną odpowiedź
 * i nie koliduje z odpowiedziami dla innych procesów.
 */
#define MSG_TICKET_BASE  1
#define MSG_TICKET_REPLY MSG_TICKET_BASE   /* Alias zachowany dla kompatybilności */
#define MSG_BUS_RETURNED 10000000

/*
 * Maksymalna liczba pasażerów jednocześnie aktywnych w systemie.
 * Semafor sem[4] ogranicza liczbę współbieżnych procesów pasażerów
 * do tej wartości, zapobiegając przeciążeniu systemu.
 */
#define MAX_PASSENGERS 5000

/*
 * Maksymalna pojemność jednego autobusu.
 * Wyznacza górną granicę parametru P podawanego przy uruchomieniu.
 * Wartość ta ogranicza także rozmiar tablicy passenger_list w BusState.
 */
#define MAX_BUS_CAPACITY 500

/*
 * Górna granica wartości PID obsługiwanych przez system.
 * Nowoczesne jądra Linux mogą przydzielać PID-y rzędu milionów,
 * dlatego tablica passenger_trip_completed musi być odpowiednio duża.
 * Rozmiar tablicy: 10 MB – akceptowalny koszt dla zapewnienia poprawności.
 */
#define MAX_PID 10000000

/*
 * BusState – Główna struktura stanu systemu, przechowywana w pamięci dzielonej.
 *
 * Dostęp do wszystkich pól musi odbywać się pod ochroną semafora sem[0] (mutex),
 * chyba że pole jest oznaczone jako volatile i aktualizowane atomowo przez jeden proces.
 *
 * Semafory zestawu semid:
 *   sem[0] – Mutex ogólny chroniący odczyty i zapisy do tej struktury.
 *   sem[1] – Bramka dla pasażerów bez roweru (serializuje wsiadanie).
 *   sem[2] – Bramka dla pasażerów z rowerem (serializuje wsiadanie).
 *   sem[3] – Mutex dworca (tylko jeden autobus jednocześnie na dworcu).
 *   sem[4] – Licznik wolnych slotów pasażerów (max MAX_PASSENGERS jednocześnie).
 *   sem[5] – Sygnał powrotu z trasy (kierowca postuje +1 dla każdego pasażera).
 */
struct BusState {
    /* Parametry konfiguracyjne (ustawiane przez main, tylko do odczytu przez pozostałe procesy) */
    int P;                      /* Maksymalna liczba pasażerów w jednym autobusie         */
    int R;                      /* Maksymalna liczba rowerów w jednym autobusie           */
    int T;                      /* Czas oczekiwania kierowcy na pasażerów (sekundy)       */
    int N;                      /* Liczba autobusów (procesów kierowcy) w systemie        */

    /* Stan dynamiczny autobusu */
    int passengers;             /* Aktualna liczba pasażerów w autobusie na dworcu        */
    int bikes;                  /* Aktualna liczba rowerów w autobusie na dworcu          */
    int departing;              /* Flaga: kierowca ogłosił odjazd, wsiadanie zablokowane  */
    int station_blocked;        /* Flaga: dworzec zamknięty, nowi pasażerowie odrzucani   */
    int active_passengers;      /* Liczba aktywnych procesów pasażerów w systemie         */
    int boarded_passengers;     /* Łączna liczba pasażerów, którzy odbyli podróż          */
    pid_t driver_pid;           /* PID kierowcy aktualnie obsługującego dworzec           */
    int shutdown;               /* Flaga: system w trakcie wyłączania                    */

    /* Lista pasażerów w bieżącym kursie */
    pid_t passenger_list[MAX_BUS_CAPACITY]; /* PID-y pasażerów; ujemny PID oznacza dziecko */
    int   passenger_count;      /* Liczba wpisów w passenger_list                        */

    /* Licznik aktywnych pasażerów według generatora */
    int generator_count;        /* Używany przez generator do śledzenia żywych procesów  */

    /*
     * Tablica flag zakończenia podróży, indeksowana PID-em pasażera.
     * Kierowca po powrocie ustawia passenger_trip_completed[pid] = 1,
     * następnie sygnalizuje semafor sem[5], by obudzić oczekującego pasażera.
     * Tablica jest volatile, gdyż może być modyfikowana przez inny proces w dowolnym momencie.
     */
    volatile char passenger_trip_completed[MAX_PID];

    /* Liczniki statystyczne – aktualizowane atomowo pod mutexem sem[0] */
    int total_bikes;                     /* Łączna liczba pasażerów z rowerami            */
    int total_children_with_guardian;    /* Dzieci, które wsiadły z opiekunem             */
    int total_children_without_guardian; /* Dzieci odrzucone z powodu braku opiekuna      */
    int total_vip;                       /* Łączna liczba pasażerów VIP                   */
    int total_non_vip;                   /* Łączna liczba pasażerów nie-VIP               */
    int cashier_processed;               /* Liczba pasażerów obsłużonych przez kasjera    */
    int generator_created;               /* Liczba pasażerów utworzonych przez generator  */
    int total_station_blocked;           /* Pasażerowie odrzuceni z powodu zamkniętego dworca */
    int total_sent_to_cashier;           /* Pasażerowie, którzy faktycznie wysłali żądanie do kasjera */
};

/*
 * msg – Struktura wiadomości przesyłanej między pasażerem a kasjerem.
 *
 * Pole `type` jest wymagane przez interfejs msgrcv/msgsnd jako pierwszy element.
 * W kolejce żądań (MSG_PATH) type = MSG_REGISTER.
 * W kolejce odpowiedzi (MSG_REPLY_PATH) type = MSG_TICKET_REPLY + pid,
 * co pozwala każdemu pasażerowi odebrać wyłącznie własny bilet.
 */
struct msg {
    long  type;        /* Typ wiadomości – wymagany przez System V IPC     */
    pid_t pid;         /* PID pasażera wysyłającego żądanie                */
    int   vip;         /* 1 jeśli pasażer jest VIP (omija kasę)            */
    int   bike;        /* 1 jeśli pasażer posiada rower                    */
    int   child;       /* 1 jeśli pasażer jest opiekunem dziecka           */
    int   ticket_ok;   /* 1 jeśli kasjer wystawił bilet pomyślnie          */
    pid_t driver_pid;  /* PID kierowcy (używany w wiadomościach o powrocie) */
};

#endif /* IPC_H */
