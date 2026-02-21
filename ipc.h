#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

#define SHM_PATH      "bus_shm.key"
#define SEM_PATH      "bus_sem.key"
#define MSG_PATH      "bus_msg.key"       /* kolejka żądań:  pasażer → kasjer  */
#define MSG_REPLY_PATH "bus_msg_reply.key" /* kolejka odpow.: kasjer  → pasażer */

/* Typy wiadomości w kolejce żądań (MSG_PATH) */
#define MSG_REGISTER      1   /* pasażer rejestruje się u kasjera            */

/* Typy wiadomości w kolejce odpowiedzi (MSG_REPLY_PATH).
 * Każdy pasażer czeka na typ = MSG_TICKET_BASE + own_pid,
 * więc nie może przypadkowo odebrać biletu innego pasażera. */
#define MSG_TICKET_BASE   1   /* kasjer używa type = MSG_TICKET_BASE + pid    */

/* Stara makrodefinicja zachowana dla kompatybilności z ewentualnymi
 * fragmentami kodu, które jeszcze z niej korzystają. */
#define MSG_TICKET_REPLY  MSG_TICKET_BASE
#define MSG_BUS_RETURNED  10000000

#define MAX_PASSENGERS 5000
// MAX_BUS_CAPACITY = maksymalna pojemność JEDNEGO autobusu
// (górny limit dla parametru P)
#define MAX_BUS_CAPACITY 500

// MAX_PID - maksymalny obsługiwany PID (10 milionów)
// Nowoczesne Linuxy mają PID-y w milionach, więc potrzebujemy dużej tablicy
// 10MB shared memory to akceptowalne dla stabilności
#define MAX_PID 10000000

struct BusState {
    int P;                      // Maksymalna liczba pasażerów
    int R;                      // Maksymalna liczba rowerów
    int T;                      // Czas oczekiwania na dworcu
    int N;                      // Liczba autobusów
    int passengers;             // Aktualna liczba pasażerów w autobusie
    int bikes;                  // Aktualna liczba rowerów w autobusie
    int departing;              // Flaga: autobus odjeżdża
    int station_blocked;        // Flaga: dworzec zablokowany
    int active_passengers;      // Liczba aktywnych pasażerów w systemie
    int boarded_passengers;     // Liczba pasażerów, którzy weszli do autobusu
    pid_t driver_pid;           // PID aktualnego kierowcy na dworcu
    int shutdown;               // Flaga: system się wyłącza
    pid_t passenger_list[MAX_BUS_CAPACITY];  // Lista PID-ów pasażerów w busie
    int passenger_count;        // Liczba pasażerów na liście
    int generator_count;        // Liczba pasażerów aktywnych według generatora (limit 100)
    volatile char passenger_trip_completed[MAX_PID];  // Flagi zakończenia podróży (indeks = PID, 10MB)
    
    // ========== NOWE LICZNIKI STATYSTYCZNE ==========
    int total_bikes;                        // Całkowita liczba rowerów
    int total_children_with_guardian;       // Dzieci z opiekunem (wsiedli)
    int total_children_without_guardian;    // Dzieci bez opiekuna (odrzucone)
    int total_vip;                          // Pasażerowie VIP
    int total_non_vip;                      // Pasażerowie nie-VIP
    int cashier_processed;                  // Liczba pasażerów obsłużonych przez kasę
    int generator_created;                  // Liczba pasażerów stworzonych przez generator
    int total_station_blocked;             // Pasażerowie odrzuceni przez station_blocked (nie dotarli do kasy)
    int total_sent_to_cashier;             // Pasażerowie faktycznie wysłani do kasy (nie-VIP przed msgsnd)
};

struct msg {
    long type;
    pid_t pid;
    int vip;
    int bike;
    int child;
    int ticket_ok;
    pid_t driver_pid;           // PID kierowcy który wrócił
};

#endif
