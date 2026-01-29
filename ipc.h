/*
 * IPC.H - Plik nagłówkowy zawierający definicje wspólne dla wszystkich procesów
 * 
 * Ten plik definiuje:
 * - Ścieżki do plików kluczy IPC
 * - Typy komunikatów
 * - Strukturę stanu systemu (BusState)
 * - Strukturę komunikatów (msg)
 */

#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

// === ŚCIEŻKI DO PLIKÓW KLUCZY IPC ===
// Te pliki są używane przez ftok() do generowania kluczy IPC
// Muszą istnieć w systemie plików (tworzone przez main.c)
#define SHM_PATH "bus_shm.key"  // Plik klucza dla pamięci dzielonej
#define SEM_PATH "bus_sem.key"  // Plik klucza dla semaforów
#define MSG_PATH "bus_msg.key"  // Plik klucza dla kolejki komunikatów

// === TYPY KOMUNIKATÓW ===
// Komunikaty w kolejce używają pola 'type' do identyfikacji
#define MSG_REGISTER 1          // Typ: rejestracja pasażera w kasie
#define MSG_TICKET_REPLY 2      // Bazowy typ dla odpowiedzi z biletem
                                // Rzeczywisty typ to MSG_TICKET_REPLY + PID pasażera
                                // Dzięki temu każdy pasażer odbiera tylko swój bilet

/*
 * Struktura BusState - Stan Systemu Autobusowego
 * 
 * Ta struktura jest przechowywana w pamięci dzielonej i zawiera
 * wszystkie informacje o stanie systemu dostępne dla wszystkich procesów.
 * Dostęp do tej struktury jest chroniony semaforem mutex (sem[0]).
 */
struct BusState {
    // === PARAMETRY KONFIGURACYJNE (stałe podczas działania) ===
    int P;                      // Maksymalna liczba pasażerów w autobusie
    int R;                      // Maksymalna liczba rowerów w autobusie
    int T;                      // Czas oczekiwania autobusu na dworcu (w sekundach)
    int N;                      // Liczba autobusów w systemie
    
    // === STAN AKTUALNEGO AUTOBUSU NA DWORCU ===
    int passengers;             // Aktualna liczba pasażerów w autobusie na dworcu
    int bikes;                  // Aktualna liczba rowerów w autobusie na dworcu
    int departing;              // Flaga: 1 = autobus odjeżdża (pasażerowie nie mogą już wsiadać)
    
    // === STAN SYSTEMU ===
    int station_blocked;        // Flaga: 1 = dworzec zablokowany (nowi pasażerowie nie mogą przyjść)
    int active_passengers;      // Liczba aktywnych procesów pasażerów w systemie
    int boarded_passengers;     // Całkowita liczba pasażerów, którzy wsiedli do autobusów
    
    // === KOMUNIKACJA Z KIEROWCĄ ===
    pid_t driver_pid;           // PID kierowcy aktualnie stojącego na dworcu
                                // Używane przez dyspozytora do wysyłania sygnałów
    
    // === FLAGA WYŁĄCZANIA ===
    int shutdown;               // Flaga: 1 = system się wyłącza (wszystkie procesy kończą pracę)
};

/*
 * Struktura msg - Komunikat w Kolejce Komunikatów
 * 
 * Używana do komunikacji między pasażerami a kasjerem:
 * 1. Pasażer -> Kasjer: rejestracja (type = MSG_REGISTER)
 * 2. Kasjer -> Pasażer: bilet (type = MSG_TICKET_REPLY + PID pasażera)
 */
struct msg {
    long type;          // Typ komunikatu (wymagane przez msgrcv/msgsnd)
                        // Dla rejestracji: MSG_REGISTER
                        // Dla biletu: MSG_TICKET_REPLY + PID
    
    // === INFORMACJE O PASAŻERZE ===
    pid_t pid;          // PID procesu pasażera
    int vip;            // 1 = pasażer VIP (nie płaci, ma priorytet)
    int bike;           // 1 = pasażer ma rower
    int child;          // 1 = to dziecko (idzie z rodzicem)
    
    // === INFORMACJA O BILECIE ===
    int ticket_ok;      // 1 = bilet OK (używane w odpowiedzi od kasjera)
};

#endif
