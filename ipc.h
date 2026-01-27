#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

#define SHM_PATH "bus_shm.key"
#define SEM_PATH "bus_sem.key"
#define MSG_PATH "bus_msg.key"

#define MSG_REGISTER 1
#define MSG_TICKET_REPLY 2

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
};

struct msg {
    long type;
    pid_t pid;
    int vip;
    int bike;
    int child;
    int ticket_ok;
};

#endif