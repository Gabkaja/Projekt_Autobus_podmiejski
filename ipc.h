#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

#define SHM_KEY 0x1234
#define SEM_KEY 0x2345
#define MSG_KEY 0x3456

#define MAX_P 50
#define MAX_R 10
#define TOTAL_PASSENGERS 20

struct BusState {
    int passengers;
    int bikes;
    int blocked;
    int active_passengers;
    pid_t driver_pid;
};

struct msg {
    long type;
    pid_t pid;
};

#endif
