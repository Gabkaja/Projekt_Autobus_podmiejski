#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

#define SHM_PATH "bus_shm.key"
#define SEM_PATH "bus_sem.key"
#define MSG_PATH "bus_msg.key"

#define REGISTER 1

struct BusState {
    int P;
    int R;
    int T;
    int N;
    int passengers;
    int bikes;
    int departing;
    int station_blocked;
    int active_passengers;
    pid_t driver_pid;
    int shutdown;
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