#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

/* Ścieżki do plików używanych przez ftok() do generowania kluczy IPC.
 * Każdy z tych plików musi fizycznie istnieć na dysku zanim zostanie
 * wywołany ftok(). Tworzone są przez main przed uruchomieniem procesów. */
#define SHM_PATH      "bus_shm.key"
#define SEM_PATH      "bus_sem.key"
#define MSG_PATH      "bus_msg.key"        /* kolejka żądań:  pasażer → kasjer  */
#define MSG_REPLY_PATH "bus_msg_reply.key" /* kolejka odpow.: kasjer  → pasażer */

/* Typ wiadomości wysyłanej przez pasażera do kolejki żądań.
 * Kasjer blokuje się na msgrcv z tym typem i obsługuje kolejno przychodzących. */
#define MSG_REGISTER      1

/* Podstawa typów wiadomości w kolejce odpowiedzi.
 * Kasjer wysyła bilet z typem = MSG_TICKET_BASE + pid_pasażera.
 * Dzięki temu pasażer odbiera tylko swój bilet, a nie cudzy –
 * nawet jeśli wielu pasażerów czeka jednocześnie na tej samej kolejce. */
#define MSG_TICKET_BASE   1

/* Alias zachowany dla kompatybilności ze starszymi fragmentami kodu. */
#define MSG_TICKET_REPLY  MSG_TICKET_BASE
#define MSG_BUS_RETURNED  10000000

/* Łączna maksymalna liczba pasażerów obsługiwanych w całej symulacji.
 * Używana jako górna granica licznika aktywnych procesów oraz
 * jako wielkość podnoszonego semafora sem[6] podczas shutdownu. */
#define MAX_PASSENGERS 5000

/* Maksymalna pojemność jednego autobusu (górna granica parametru P).
 * Wyznacza też rozmiar tablicy passenger_list w strukturze BusState. */
#define MAX_BUS_CAPACITY 500

/* Maksymalny PID obsługiwany przez tablicę passenger_trip_completed.
 * Nowoczesne jądra Linuksa mogą przydzielać PID-y rzędu milionów,
 * dlatego tablica ma 10 milionów wpisów i zajmuje 10 MB w shared memory.
 * Indeksowanie po PID eliminuje potrzebę przeszukiwania listy. */
#define MAX_PID 10000000

/* Główna struktura stanu systemu trzymana w pamięci dzielonej.
 * Dostęp do wszystkich pól (poza flagą shutdown w handlerach sygnałów)
 * musi odbywać się pod mutexem sem[0]. */
struct BusState {
    int P;                      /* maksymalna liczba pasażerów w autobusie (parametr) */
    int R;                      /* maksymalna liczba rowerów w autobusie (parametr) */
    int T;                      /* czas oczekiwania kierowcy na dworcu w sekundach */
    int N;                      /* liczba autobusów / procesów kierowców */
    int passengers;             /* aktualna liczba pasażerów w autobusie stojącym na dworcu */
    int bikes;                  /* aktualna liczba rowerów w autobusie stojącym na dworcu */
    int departing;              /* 1 gdy autobus właśnie odjeżdża i zamknął drzwi */
    int station_blocked;        /* 1 gdy dworzec jest zablokowany i nie można wsiadać */
    int active_passengers;      /* liczba procesów pasażerów aktualnie działających w systemie */
    int boarded_passengers;     /* łączna liczba pasażerów którzy dotychczas wsiedli do autobusów */
    pid_t driver_pid;           /* PID kierowcy aktualnie stojącego na dworcu, 0 jeśli brak */
    int shutdown;               /* 1 gdy system jest w trakcie wyłączania */
    pid_t passenger_list[MAX_BUS_CAPACITY];  /* PID-y pasażerów w aktualnym kursie; ujemne PID = dziecko */
    int passenger_count;        /* liczba wpisów w passenger_list */
    int generator_count;        /* liczba aktualnie żyjących procesów pasażerów (limit 100 jednocześnie) */
    volatile char passenger_trip_completed[MAX_PID];  /* tablica flag: indeks = PID, wartość 1 = pasażer może wyjść */

    /* Liczniki statystyczne zbierane przez cały czas trwania symulacji.
     * Odczytywane przez main po zakończeniu wszystkich procesów potomnych. */
    int total_bikes;                        /* łączna liczba pasażerów z rowerami */
    int total_children_with_guardian;       /* dzieci które wsiadły z opiekunem */
    int total_children_without_guardian;    /* dzieci odrzucone z powodu braku opiekuna */
    int total_vip;                          /* pasażerowie VIP którzy ominęli kasę */
    int total_non_vip;                      /* pasażerowie zwykli (łącznie z odrzuconymi dziećmi) */
    int cashier_processed;                  /* ile razy kasjer wysłał bilet (powinno = total_sent_to_cashier) */
    int generator_created;                  /* ile procesów pasażerów stworzył generator */
    int total_station_blocked;             /* pasażerowie którzy odeszli bo dworzec był zamknięty */
    int total_sent_to_cashier;             /* pasażerowie faktycznie wysłani do kasy (msgsnd wykonany) */
};

/* Struktura wiadomości przesyłanej między pasażerem a kasjerem.
 * Pole type (wymagane przez system kolejek) musi być pierwszym polem i
 * musi mieć typ long – taki jest wymóg msgrcv/msgsnd. */
struct msg {
    long type;       /* MSG_REGISTER przy żądaniu; MSG_TICKET_BASE+pid przy odpowiedzi */
    pid_t pid;       /* PID pasażera; 0 oznacza wiadomość wake-up wysyłaną przez main */
    int vip;         /* 1 jeśli pasażer jest VIP-em (kasjer go ignoruje) */
    int bike;        /* 1 jeśli pasażer ma rower */
    int child;       /* 1 jeśli pasażer jest opiekunem (rezerwuje 2 miejsca) */
    int ticket_ok;   /* ustawiane przez kasjera na 1 gdy bilet wystawiony poprawnie */
    pid_t driver_pid; /* PID kierowcy – pole historyczne, aktualnie nieużywane */
};

#endif
