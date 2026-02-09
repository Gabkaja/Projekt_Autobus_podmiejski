/*
 * ipc.h
 * 
 * Definicje struktur i stałych dla systemu komunikacji międzyprocesowej.
 * Zawiera wspólne definicje używane przez wszystkie procesy w systemie
 * symulacji dworca autobusowego.
 */

#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

/* Ścieżki do plików kluczy dla ftok() */
#define SHM_PATH "bus_shm.key"
#define SEM_PATH "bus_sem.key"
#define MSG_PATH "bus_msg.key"

/* Typy wiadomości w kolejce komunikatów */
#define MSG_REGISTER 1            /* Rejestracja pasażera w kasie */
#define MSG_TICKET_REPLY 2        /* Odpowiedź z biletem (+ PID pasażera) */
#define MSG_BUS_RETURNED 10000000 /* Powiadomienie o powrocie (+ PID pasażera) */

/* Limity systemu */
#define MAX_PASSENGERS 10         /* Limit aktywnych procesów pasażerów */
#define MAX_BUS_CAPACITY 200      /* Maksymalna pojemność jednego autobusu */

/* 
 * Struktura przechowująca globalny stan systemu w pamięci dzielonej.
 * Współdzielona między wszystkimi procesami, dostęp chroniony semaforami.
 */
struct BusState {
    /* Parametry konfiguracyjne (ustawiane przy starcie, tylko odczyt) */
    int P;                      /* Maksymalna liczba pasażerów w autobusie */
    int R;                      /* Maksymalna liczba rowerów w autobusie */
    int T;                      /* Czas oczekiwania na dworcu (sekundy) */
    int N;                      /* Liczba autobusów */
    
    /* Stan aktualnie stojącego autobusu */
    int passengers;             /* Aktualna liczba pasażerów w autobusie */
    int bikes;                  /* Aktualna liczba rowerów w autobusie */
    int departing;              /* Flaga: autobus odjeżdża (blokada wsiadań) */
    
    /* Stan globalny systemu */
    int station_blocked;        /* Flaga: dworzec zablokowany */
    int active_passengers;      /* Liczba wszystkich aktywnych pasażerów */
    int boarded_passengers;     /* Łączna liczba pasażerów którzy wsiedli */
    pid_t driver_pid;           /* PID kierowcy na dworcu (0 = brak) */
    int shutdown;               /* Flaga: system się wyłącza */
    
    /* Lista pasażerów w aktualnie stojącym autobusie */
    pid_t passenger_list[MAX_BUS_CAPACITY];  /* PID-y (ujemne = dzieci) */
    int passenger_count;        /* Liczba wpisów w liście */
    
    /* Statystyki */
    int generator_count;        /* Liczba aktywnych pasażerów (limit MAX_PASSENGERS) */
};

/*
 * Struktura wiadomości przesyłanej przez kolejkę komunikatów.
 * Pole 'type' służy do routingu - każdy proces odbiera tylko swoje wiadomości.
 */
struct msg {
    long type;           /* Typ wiadomości (routing) */
    pid_t pid;           /* PID pasażera */
    int vip;             /* Flaga: pasażer VIP (ma już bilet) */
    int bike;            /* Flaga: pasażer ma rower */
    int child;           /* Flaga: pasażer ma dziecko (wątek) */
    int ticket_ok;       /* Flaga: bilet wystawiony/zatwierdzony */
    pid_t driver_pid;    /* PID kierowcy (w powiadomieniu o powrocie) */
};

#endif
