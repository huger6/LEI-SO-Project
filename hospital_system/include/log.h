#ifndef LOG_H
#define LOG_H

#include <time.h>
#include <pthread.h>

// Log severity

typedef enum {
    CRITICAL = 1,
    ERROR = 2,
    WARNING = 3,
    INFO = 4,
    DEBUG = 5
} log_severity_t;

// Critical logs (SHM5)

typedef struct {
    time_t timestamp;
    char event_type[30];
    char component[20];
    char description[256];
    log_severity_t severity;
} critical_event_t;

typedef struct {
    critical_event_t events[1000];
    int event_count;
    int current_index;
    pthread_mutex_t mutex;
} critical_log_shm_t;


int init_logging(const char *filepath);
void close_logging();
void log_event(log_severity_t severity, const char *component, const char *event_type, const char *details);

#endif