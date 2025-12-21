#include <stdio.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <errno.h>
#include "ipc.h"

int main() {
    int msgid = msgget(MSG_KEY, 0);
    int shmid = shmget(SHM_KEY, sizeof(struct BusState), 0);
    struct BusState* bus = shmat(shmid, NULL, 0);

    struct msg m;

    while (1) {
        if (msgrcv(msgid, &m, sizeof(pid_t), 1, IPC_NOWAIT) < 0) {
            if (errno == ENOMSG) {
                if (bus->active_passengers == 0)
                    break;
                usleep(100000);
                continue;
            }
        }

        printf("[KASA] Bilet dla PID %d\n", m.pid);
        m.type = m.pid;
        msgsnd(msgid, &m, sizeof(pid_t), 0);
    }

    return 0;
}
