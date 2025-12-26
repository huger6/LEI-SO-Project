#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
// #include <stdarg.h>

#include "../include/log.h"
#include "../include/safe_threads.h"


// Global log file pointer
static FILE *log_file = NULL;

// SHM
static critical_log_shm_t *global_log_shm = NULL;

// Mutex
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Map severity enum to string
const char* get_severity_str(log_severity_t severity) {
    switch(severity) {
        case CRITICAL: return "CRITICAL";
        case ERROR:    return "ERROR";
        case WARNING:  return "WARNING";
        case INFO:     return "INFO";
        case DEBUG_LOG:    return "DEBUG_LOG";
        default:       return "UNKNOWN";
    }
}

// Set logger ptr
void set_critical_log_shm_ptr(critical_log_shm_t *shm_ptr) {
    global_log_shm = shm_ptr;
}

// Initialize logging system
int init_logging(const char *filepath) {
    pthread_mutex_lock(&log_mutex);

    // Open in append mode
    log_file = fopen(filepath, "a");
    if (!log_file) {
        perror("Failed to open log file");
        pthread_mutex_unlock(&log_mutex);
        return -1;
    }
    pthread_mutex_unlock(&log_mutex);
    return 0;
}

// Close logging system
void close_logging() {
    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    global_log_shm = NULL;
    pthread_mutex_unlock(&log_mutex);
}

// Log events 
void log_event(log_severity_t severity, const char *component, const char *event_type, const char *details) {
    // Prepare Timestamp
    time_t now;
    struct tm *timeinfo;
    char timestamp[25];
    time(&now);
    timeinfo = localtime(&now);

    if (timeinfo != NULL) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    } else {
        strncpy(timestamp, "UNKNOWN", sizeof(timestamp));
    }

    // Lock Local Mutex
    pthread_mutex_lock(&log_mutex);

    if (!log_file) {
        // Fallback if file is closed/failed
        fprintf(stderr, "[LOG ERROR] System not initialized. Event: %s | %s\n", event_type, details);
    } else {
        // Format: [TIMESTAMP] [COMPONENT] [SEVERITY] [EVENT_TYPE] details
        fprintf(log_file, "[%s] [%s] [%s] [%s] %s\n", 
                timestamp, 
                component, 
                get_severity_str(severity), 
                event_type, 
                details);
        
        // Flush immediately to ensure data is written
        fflush(log_file);
    }

    // Write to Shared Memory (Critical Logic)
    // Only write to SHM if severity is high or it's a shutdown event
    if (severity <= ERROR || (event_type && strcmp(event_type, "SHUTDOWN") == 0)) {
        if (global_log_shm != NULL) {
            // Lock SHM Mutex
            safe_pthread_mutex_lock(&global_log_shm->mutex);
            
            int idx = global_log_shm->current_index;
            
            // Write Data to SHM Struct
            
            // Timestamp 
            global_log_shm->events[idx].timestamp = now;
            
            // Severity
            global_log_shm->events[idx].severity = severity;
            
            // Strings
            snprintf(global_log_shm->events[idx].event_type, 
                     sizeof(global_log_shm->events[idx].event_type), 
                     "%s", event_type);

            snprintf(global_log_shm->events[idx].component, 
                     sizeof(global_log_shm->events[idx].component), 
                     "%s", component);

            snprintf(global_log_shm->events[idx].description, 
                     sizeof(global_log_shm->events[idx].description), 
                     "%s", details);

            // --- Circular Buffer Update ---
            
            // Move index forward, wrapping around at 1000
            global_log_shm->current_index = (idx + 1) % 1000;

            // Track total count up to the maximum capacity (stops at 1000)
            if (global_log_shm->event_count < 1000) {
                global_log_shm->event_count++;
            }
            
            safe_pthread_mutex_unlock(&global_log_shm->mutex);
        }
    }

    // Console Output for Debugging
    #ifdef DEBUG
        printf("[%s] [%s] [%s] [%s] %s\n", 
            timestamp, component, get_severity_str(severity), event_type, details);
    #endif

    pthread_mutex_unlock(&log_mutex);
}

