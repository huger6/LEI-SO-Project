#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
// #include <stdarg.h>

#include "../include/log.h"


// Global log file pointer
static FILE *log_file = NULL;

// Mutex
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Map severity enum to string
const char* get_severity_str(log_severity_t severity) {
    switch(severity) {
        case CRITICAL: return "CRITICAL";
        case ERROR:    return "ERROR";
        case WARNING:  return "WARNING";
        case INFO:     return "INFO";
        case DEBUG:    return "DEBUG";
        default:       return "UNKNOWN";
    }
}

// Initialize logging system
int init_logging(const char *filepath) {
    // Open in append mode
    log_file = fopen(filepath, "a");
    if (!log_file) {
        perror("Failed to open log file");
        return -1;
    }
    return 0;
}

// Close logging system
void close_logging() {
    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}

// Log events 
void log_event(log_severity_t severity, const char *component, const char *event_type, const char *details) {
    pthread_mutex_lock(&log_mutex);

    if (!log_file) {
        fprintf(stderr, "LOG SYSTEM NOT INITIALIZED: %s\n", details);
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    // Get current timestamp
    time_t now;
    struct tm *timeinfo;
    char timestamp[25];
    time(&now);
    timeinfo = localtime(&now);

    if (timeinfo != NULL) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    } 
    else {
        strncpy(timestamp, "UNKNOWN TIME", sizeof(timestamp));
        timestamp[sizeof(timestamp) - 1] = '\0';
    }

    // Format: [TIMESTAMP] [COMPONENT] [SEVERITY] [EVENT_TYPE] [DETAILS]
    fprintf(log_file, "[%s] [%s] [%s] [%s] %s\n", 
            timestamp, 
            component, 
            get_severity_str(severity), 
            event_type, 
            details);
    
    // Make sure data is written immediately
    fflush(log_file);

    // Also print to console for real-time monitoring (optional but helpful)
    #ifdef DEBUG
        printf("[%s] [%s] [%s] [%s] %s\n", 
            timestamp, component, get_severity_str(severity), event_type, details);
    #endif

    pthread_mutex_unlock(&log_mutex);
}
