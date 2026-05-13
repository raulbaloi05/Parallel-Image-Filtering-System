#ifndef DATATYPES_H
#define DATATYPES_H

#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#define REQUEST_CLIENTS 0
#define SEND_CLIENTS 1
#define REQUEST_FILTERS 2
#define SEND_FILTERS 3
#define REQUEST_LOGS 4
#define SEND_LOGS 5
#define KILL_CLIENT 6
#define DISCONNECT_ALL 7
#define CHANGE_CONECTIONS 8
#define SET_MAX_CLIENTS 9
#define REQUEST_SERVER_CONFIGURATION 10
#define SEND_SERVER_CONFIGURATION 11
#define REQUEST_SYS_INFO 12
#define SEND_SYS_INFO 13
#define BAN_CLIENT 14
#define ADMIN_TIMEOUT_DISCONNECT 15

#define UNIXSOCKET "/tmp/unixds"
#define MAX_LOGS 256

#define FILTERNR 5

#define STATUS_LEN    8
#define IP_LEN        16
#define PROCESS_COUNT 4
#define NAME_LEN      64
#define MESSAGE_LEN   256
#define MAX_CLIENTS   100
#define UPTIME_LEN    64

/* Headerul protocolului */
typedef struct {
    size_t type;
    size_t size;
    int count;
} Header;

/* Informatiile celor 4 procese care prelucreaza imaginea */
typedef struct {
    int cpu;
    int ram;
    pid_t pid;
    char status[STATUS_LEN];
} ProcessInfo;

/* Informatiile clientului conectat */
typedef struct {
    char ip[IP_LEN];
    int job_id;
    ProcessInfo P[PROCESS_COUNT];
} ClientInfo;

/* Clientul si procesele acestuia pe care dorim sa le terminam */
typedef struct {
    int job_id;
    pid_t childrens[PROCESS_COUNT];
} toKill;

/* Informatiile unui filtru */
typedef struct {
    char name[NAME_LEN];
    unsigned int uses;
} Filter;

typedef struct {
    char status[STATUS_LEN];
    unsigned int max_clients_number;
} ServerConfiguration;

typedef struct {
    char message[MESSAGE_LEN];
} LogEntry;

typedef struct {
    int CPU_usage;
    long RAM_usage;
    long RAM_max;
    char uptime[UPTIME_LEN];
} SysInfo;

/* Global server state structure (replaces shared memory) */
typedef struct {
    int active_clients_count;
    ClientInfo clients[MAX_CLIENTS];
    Filter filters[FILTERNR];
    ServerConfiguration config;
    time_t start_time;
    LogEntry logs[MAX_LOGS];
    int log_count;
} ServerState;

// Thread function declarations
void* unix_main(void* arg);
void* soap_main(void* arg);

#endif