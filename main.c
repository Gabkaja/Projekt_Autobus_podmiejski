#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include "ipc.h"

/* Globalne identyfikatory IPC trzymane jako zmienne globalne, żeby handler
 * sygnału SIGINT mógł z nich korzystać bez przekazywania przez argument. */
int shmid, semid, msgid, msgid_reply;
struct BusState* bus;
pid_t dispatcher_pid = 0;

/* Otwiera report.txt w trybie dopisywania i zapisuje linię tekstu.
 * Każde wywołanie otwiera i zamyka plik – nieinwazyjne wobec innych procesów. */
void log_write(const char* s) {
    int fd = open("report.txt", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd == -1) {
        perror("open report");
        return;
    }
    if (write(fd, s, strlen(s)) == -1) {
        perror("write report");
    }
    if (close(fd) == -1) {
        perror("close report");
    }
}

/* Formatuje aktualny czas jako HH:MM:SS i wpisuje do bufora buf o długości n. */
void ts(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(buf, n, "00:00:00");
        return;
    }
    strftime(buf, n, "%H:%M:%S", tm_info);
}

/* Usuwa wszystkie zasoby IPC (shared memory, semafory, kolejki komunikatów)
 * oraz pliki kluczy z dysku. Wywoływana po zakończeniu wszystkich procesów
 * potomnych, żeby nie zostawiać zombie-zasobów w systemie. */
void cleanup() {
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        if (errno != EINVAL && errno != EIDRM) {
            perror("shmctl IPC_RMID");
        }
    }
    if (semctl(semid, 0, IPC_RMID) == -1) {
        if (errno != EINVAL && errno != EIDRM) {
            perror("semctl IPC_RMID");
        }
    }
    if (msgctl(msgid, IPC_RMID, NULL) == -1) {
        if (errno != EINVAL && errno != EIDRM) {
            perror("msgctl IPC_RMID");
        }
    }
    if (msgctl(msgid_reply, IPC_RMID, NULL) == -1) {
        if (errno != EINVAL && errno != EIDRM) {
            perror("msgctl reply IPC_RMID");
        }
    }
    unlink(SHM_PATH);
    unlink(SEM_PATH);
    unlink(MSG_PATH);
    unlink(MSG_REPLY_PATH);
}

/* Handler SIGINT i SIGUSR2 – uruchamiany gdy użytkownik wciśnie Ctrl+C
 * lub gdy dyspozytor wyśle SIGUSR2 z powodu blokady dworca.
 *
 * Kolejność działań jest ważna:
 *  1. Ustawia flagi shutdown i station_blocked w shared memory tak,
 *     żeby wszystkie procesy zobaczyły je przy następnym sprawdzeniu.
 *  2. Wysyła SIGINT do dyspozytora, żeby ten też zareagował na shutdown.
 *  3. Wysyła do kasjera wake-up message (pid=0), bo kasjer blokuje się
 *     na msgrcv i bez tej wiadomości nigdy nie sprawdziłby flagi shutdown.
 *  4. Podnosi sem[6] wielokrotnie, żeby obudzić pasażerów czekających
 *     na powrót autobusu po tym jak system zostaje zamknięty. */
void handle_sigint(int sig) {
    (void)sig;
    if (bus) {
        bus->shutdown = 1;
        bus->station_blocked = 1;
    }
    
    if (dispatcher_pid > 0) {
        kill(dispatcher_pid, SIGINT);
    }
    
    if (msgid != -1) {
        struct msg wake_msg;
        memset(&wake_msg, 0, sizeof(wake_msg));
        wake_msg.type = MSG_REGISTER;
        wake_msg.pid  = 0; /* pid=0 to konwencja wake-up, nie prawdziwy pasażer */
        msgsnd(msgid, &wake_msg, sizeof(wake_msg) - sizeof(long), IPC_NOWAIT);
    }
    
    /* Podnosimy sem[6] MAX_PASSENGERS razy, żeby każdy pasażer czekający
     * na zakończenie podróży mógł się odblokować i zakończyć proces. */
    if (semid != -1) {
        for (int i = 0; i < MAX_PASSENGERS; i++) {
            struct sembuf sb = { 6, 1, IPC_NOWAIT };
            if (semop(semid, &sb, 1) == -1) break;
        }
    }
    
    char b[64];
    ts(b, sizeof(b));
    char ln[128];
    snprintf(ln, sizeof(ln), "[%s] [MAIN] Shutdown initiated\n", b);
    log_write(ln);
}

/* Handler SIGCHLD – zbiera statusy wyjścia zakończonych procesów potomnych
 * metodą nieblokującą (WNOHANG), żeby żaden z nich nie pozostał zombie.
 * errno jest zapisywane i przywracane, bo waitpid może je nadpisać. */
void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "Uzycie: %s N P R T\n", argv[0]);
        fprintf(stderr, "  N - liczba autobusow\n");
        fprintf(stderr, "  P - maksymalna liczba pasazerow w autobusie\n");
        fprintf(stderr, "  R - maksymalna liczba rowerow w autobusie\n");
        fprintf(stderr, "  T - czas oczekiwania na dworcu (sekundy)\n");
        return EXIT_FAILURE;
    }

    int N = atoi(argv[1]);
    int P = atoi(argv[2]);
    int R = atoi(argv[3]);
    int T = atoi(argv[4]);

    if (N <= 0 || P <= 0 || R < 0 || T <= 0) {
        fprintf(stderr, "Niepoprawne parametry\n");
        return EXIT_FAILURE;
    }

    /* Tworzymy puste pliki logów przed forkiem, żeby każdy proces potomny
     * mógł je otworzyć w trybie O_APPEND bez martwienia się o ich istnienie. */
    creat("report.txt", 0600);
    creat("driver.log", 0600);
    creat("passenger.log", 0600);
    creat("cashier.log", 0600);
    creat("dispatcher.log", 0600);
    creat("generator.log", 0600);

    /* Pliki kluczy IPC muszą istnieć fizycznie na dysku, zanim ftok()
     * wygeneruje z nich klucze. Każdy plik daje inny klucz dzięki
     * różnym literom id ('S', 'E', 'M', 'R'). */
    creat(SHM_PATH, 0600);
    creat(SEM_PATH, 0600);
    creat(MSG_PATH, 0600);
    creat(MSG_REPLY_PATH, 0600);

    key_t shm_key = ftok(SHM_PATH, 'S');
    key_t sem_key = ftok(SEM_PATH, 'E');
    key_t msg_key = ftok(MSG_PATH, 'M');
    key_t msg_reply_key = ftok(MSG_REPLY_PATH, 'R');

    if (shm_key == -1 || sem_key == -1 || msg_key == -1 || msg_reply_key == -1) {
        perror("ftok");
        cleanup();
        return EXIT_FAILURE;
    }

    /* Tworzymy segment pamięci dzielonej o rozmiarze struktury BusState.
     * Wszystkie procesy potomne uzyskują do niego dostęp przez shmget+shmat. */
    shmid = shmget(shm_key, sizeof(struct BusState), IPC_CREAT | 0600);
    if (shmid == -1) {
        perror("shmget");
        cleanup();
        return EXIT_FAILURE;
    }

    bus = shmat(shmid, NULL, 0);
    if (bus == (void*)-1) {
        perror("shmat");
        cleanup();
        return EXIT_FAILURE;
    }

    /* Tworzymy 7 semaforów:
     *   sem[0] – mutex ogólny chroniący pola BusState
     *   sem[1] – bramka dla pasażerów BEZ roweru (serializuje wejście)
     *   sem[2] – bramka dla pasażerów Z rowerem
     *   sem[3] – blokada dworca: tylko jeden autobus może stać jednocześnie
     *   sem[4] – nieużywany aktualnie (zarezerwowany)
     *   sem[5] – limit generatora: ile procesów pasażerów może istnieć jednocześnie
     *   sem[6] – sygnał powrotu autobusu: kierowca postuje +1 per pasażer po kursie */
    semid = semget(sem_key, 7, IPC_CREAT | 0600);
    if (semid == -1) {
        perror("semget");
        cleanup();
        return EXIT_FAILURE;
    }

    semctl(semid, 0, SETVAL, 1);              /* mutex: zaczyna odblokowany */
    semctl(semid, 1, SETVAL, 1);              /* bramka bez roweru: otwarta */
    semctl(semid, 2, SETVAL, 1);              /* bramka z rowerem: otwarta  */
    semctl(semid, 3, SETVAL, 1);              /* dworzec: wolny             */
    semctl(semid, 4, SETVAL, 0);              /* nieużywany                 */
    semctl(semid, 5, SETVAL, MAX_PASSENGERS); /* generator: MAX_PASSENGERS wolnych slotów */
    semctl(semid, 6, SETVAL, 0);              /* trip_completed: brak sygnałów na starcie */

    /* Dwie oddzielne kolejki komunikatów zapobiegają zapychaniu:
     *   msgid       – pasażer → kasjer (żądania rejestracji)
     *   msgid_reply – kasjer → pasażer (bilety z odpowiedzią)
     * Gdyby użyć jednej kolejki, nagromadzone żądania blokowałyby bilety. */
    msgid = msgget(msg_key, IPC_CREAT | 0600);
    if (msgid == -1) {
        perror("msgget req");
        cleanup();
        return EXIT_FAILURE;
    }

    msgid_reply = msgget(msg_reply_key, IPC_CREAT | 0600);
    if (msgid_reply == -1) {
        perror("msgget reply");
        cleanup();
        return EXIT_FAILURE;
    }

    /* Inicjalizacja wszystkich pól BusState do wartości startowych.
     * Musi się odbyć przed uruchomieniem procesów potomnych, żeby nie
     * odczytały niezainicjowanych danych. */
    bus->P = P;
    bus->R = R;
    bus->T = T;
    bus->N = N;
    bus->passengers = 0;
    bus->bikes = 0;
    bus->departing = 0;
    bus->station_blocked = 0;
    bus->active_passengers = 0;
    bus->boarded_passengers = 0;
    bus->driver_pid = 0;
    bus->shutdown = 0;
    bus->passenger_count = 0;
    bus->generator_count = 0;
    for (int i = 0; i < MAX_BUS_CAPACITY; i++) {
        bus->passenger_list[i] = 0;
    }
    /* Tablica 10MB – wyzerowanie przez memset jest szybsze niż pętla. */
    memset((void*)bus->passenger_trip_completed, 0, MAX_PID);

    bus->total_bikes = 0;
    bus->total_children_with_guardian = 0;
    bus->total_children_without_guardian = 0;
    bus->total_vip = 0;
    bus->total_non_vip = 0;
    bus->cashier_processed = 0;
    bus->generator_created = 0;
    bus->total_station_blocked = 0;
    bus->total_sent_to_cashier = 0;

    /* SIGINT i SIGUSR2 obsługuje ten sam handler – oba powodują shutdown.
     * SA_RESTART sprawia że przerwane wywołania systemowe wznawiają się
     * automatycznie zamiast zwracać EINTR. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    /* SIGCHLD z SA_NOCLDSTOP budzi handler tylko przy zakończeniu procesu,
     * nie przy zatrzymaniu sygnałem SIGSTOP. */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    char b[64];
    ts(b, sizeof(b));
    char ln[512];
    snprintf(ln, sizeof(ln), "[%s] [MAIN] Start systemu: N=%d P=%d R=%d T=%d\n", 
             b, N, P, R, T);
    log_write(ln);

    /* Uruchamiamy N procesów kierowców. Każdy z nich wykona execl("./driver").
     * Kierowcy rywalizują między sobą o bramkę dworca (sem[3]). */
    for (int i = 0; i < N; i++) {
        pid_t p = fork();
        if (p == -1) {
            perror("fork driver");
        }
        else if (p == 0) {
            execl("./driver", "driver", NULL);
            perror("exec driver");
            _exit(1);
        }
    }

    /* Jeden kasjer obsługuje całą kolejkę żądań. */
    pid_t p1 = fork();
    if (p1 == -1) {
        perror("fork cashier");
    }
    else if (p1 == 0) {
        execl("./cashier", "cashier", NULL);
        perror("exec cashier");
        _exit(1);
    }

    /* Dyspozytor reaguje na sygnały zewnętrzne (SIGUSR1 = wymuś odjazd,
     * SIGUSR2 = zablokuj dworzec, SIGINT = shutdown). */
    pid_t p2 = fork();
    if (p2 == -1) {
        perror("fork dispatcher");
    }
    else if (p2 == 0) {
        execl("./dispatcher", "dispatcher", NULL);
        perror("exec dispatcher");
        _exit(1);
    }
    dispatcher_pid = p2; /* zapamiętujemy PID by wysłać mu SIGINT przy shutdown */

    /* Generator co jakiś czas tworzy nowe procesy pasażerów (max 100 jednocześnie). */
    pid_t p3 = fork();
    if (p3 == -1) {
        perror("fork generator");
    }
    else if (p3 == 0) {
        execl("./passenger_generator", "passenger_generator", NULL);
        perror("exec passenger_generator");
        _exit(1);
    }

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [MAIN] Czekam na zakonczenie wszystkich procesow...\n", b);
    log_write(ln);
    
    /* Blokujące wait() zbiera wszystkich bezpośrednich potomków jeden po jednym.
     * Pasażerowie są wnukami (dzieci generatora), więc nie są tutaj zbierani –
     * generator czeka na nich sam zanim zakończy pracę. */
    int count = 0;
    while (wait(NULL) > 0) {
        count++;
    }
    
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [MAIN] Zakonczono %d procesow\n", b, count);
    log_write(ln);

    /* Po zakończeniu wszystkich procesów odczytujemy statystyki z shared memory.
     * Jest to bezpieczne bo żaden inny proces już do niej nie pisze. */
    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "\n");
    log_write(ln);
    snprintf(ln, sizeof(ln), "========================================\n");
    log_write(ln);
    snprintf(ln, sizeof(ln), "[%s] PODSUMOWANIE SYMULACJI\n", b);
    log_write(ln);
    snprintf(ln, sizeof(ln), "========================================\n");
    log_write(ln);
    
    snprintf(ln, sizeof(ln), "Parametry systemu:\n");
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Liczba autobusow (N): %d\n", N);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Pojemnosc autobusu (P): %d\n", P);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Max rowerow w autobusie (R): %d\n", R);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Czas oczekiwania na dworcu (T): %d s\n", T);
    log_write(ln);
    snprintf(ln, sizeof(ln), "\n");
    log_write(ln);
    
    snprintf(ln, sizeof(ln), "Statystyki pasazerow:\n");
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Pasazerow utworzonych przez generator: %d\n", bus->generator_created);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Pasazerow obsluzonych przez kase: %d\n", bus->cashier_processed);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Pasazerow przewiezionych autobusem: %d\n", bus->boarded_passengers);
    log_write(ln);
    snprintf(ln, sizeof(ln), "\n");
    log_write(ln);
    
    snprintf(ln, sizeof(ln), "Typy pasazerow:\n");
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - VIP (bez kasy): %d\n", bus->total_vip);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Zwykli (przez kase): %d\n", bus->total_non_vip- bus->total_children_without_guardian);
    log_write(ln);
    snprintf(ln, sizeof(ln), "\n");
    log_write(ln);
    
    snprintf(ln, sizeof(ln), "Rowery:\n");
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Pasazerow z rowerami: %d\n", bus->total_bikes);
    log_write(ln);
    snprintf(ln, sizeof(ln), "\n");
    log_write(ln);
    
    snprintf(ln, sizeof(ln), "Dzieci:\n");
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Dzieci z opiekunem (wsiedli): %d\n", bus->total_children_with_guardian);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Dzieci bez opiekuna (odrzucone): %d\n", bus->total_children_without_guardian);
    log_write(ln);
    snprintf(ln, sizeof(ln), "\n");
    log_write(ln);
    
    snprintf(ln, sizeof(ln), "Weryfikacja spójności:\n");
    log_write(ln);
    int total_passengers_calc = bus->total_vip + bus->total_non_vip;
    int expected_cashier = bus->total_sent_to_cashier;
    snprintf(ln, sizeof(ln), "  - VIP + Zwykli + Odrzucone dzieci = %d\n", total_passengers_calc);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Pasazerow wyslanych do kasy: %d\n", bus->total_sent_to_cashier);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Pasazerow zablokowanych przez dworzec: %d\n", bus->total_station_blocked);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Generator utworzyl: %d\n", bus->generator_created);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Kasa obsluzyla: %d (powinno byc = wyslanych = %d)\n",
             bus->cashier_processed, expected_cashier);
    log_write(ln);
    snprintf(ln, sizeof(ln), "  - Autobusy przewiozly: %d\n", bus->boarded_passengers);
    log_write(ln);
    
    /* Sprawdzenie spójności: liczba obsłużonych przez kasę powinna dokładnie
     * równać się liczbie pasażerów którzy faktycznie wysłali msgsnd do kasjera. */
    if (bus->cashier_processed == expected_cashier) {
        snprintf(ln, sizeof(ln), "  OK Kasa obsluzyla poprawna liczbe pasazerow\n");
    } else {
        snprintf(ln, sizeof(ln), "  NIEZGODNOSC: Kasa vs wyslani (%d != %d)\n", 
                 bus->cashier_processed, expected_cashier);
    }
    log_write(ln);
    
    snprintf(ln, sizeof(ln), "========================================\n");
    log_write(ln);

    ts(b, sizeof(b));
    snprintf(ln, sizeof(ln), "[%s] [MAIN] System zakonczony\n", b);
    log_write(ln);
    
    printf("\n");
    printf("========================================\n");
    printf("PODSUMOWANIE SYMULACJI\n");
    printf("========================================\n");
    printf("Parametry: N=%d P=%d R=%d T=%ds\n", N, P, R, T);
    printf("\n");
    printf("Pasazerowie:\n");
    printf("  Generator utworzyl:     %d\n", bus->generator_created);
    printf("  Kasa obsluzyla:         %d\n", bus->cashier_processed);
    printf("  Autobusy przewiozly:    %d\n", bus->boarded_passengers);
    printf("\n");
    printf("Typy:\n");
    printf("  VIP:                    %d\n", bus->total_vip);
    printf("  Zwykli (przez kase):    %d\n", bus->total_sent_to_cashier);
    printf("  Z rowerami:             %d\n", bus->total_bikes);
    printf("  Dzieci z opiekunem:     %d\n", bus->total_children_with_guardian);
    printf("  Dzieci bez opiekuna:    %d\n", bus->total_children_without_guardian);
    printf("  Zablokowane (shutdown): %d\n", bus->total_station_blocked);
    printf("\n");
    printf("Weryfikacja:\n");
    printf("  Wyslanych do kasy:      %d\n", bus->total_sent_to_cashier);
    printf("  Kasa obsluzyla:         %d\n", bus->cashier_processed);
    printf("  Kasa == Wyslani? %s\n", 
           bus->cashier_processed == expected_cashier ? "TAK" : "NIE");
    printf("========================================\n");
    printf("\nSzczegoly w pliku report.txt\n");
    
    if (shmdt(bus) == -1) {
        perror("shmdt");
    }
    
    cleanup();
    return 0;
}
