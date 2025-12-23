#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../include/safe_threads.h"
#include "../include/log.h"

#define LOG_COMPONENT "SAFE_THREADS"

// --- Threads ---

int safe_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
    int status = pthread_create(thread, attr, start_routine, arg);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_create failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "THREAD_CREATE_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_join(pthread_t thread, void **retval) {
    int status = pthread_join(thread, retval);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_join failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "THREAD_JOIN_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_detach(pthread_t thread) {
    int status = pthread_detach(thread);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_detach failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "THREAD_DETACH_FAIL", error_msg);
    }
    return status;
}

// --- Mutexes ---

int safe_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    int status = pthread_mutex_init(mutex, attr);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_mutex_init failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "MUTEX_INIT_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_mutex_lock(pthread_mutex_t *mutex) {
    int status = pthread_mutex_lock(mutex);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_mutex_lock failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "MUTEX_LOCK_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    int status = pthread_mutex_unlock(mutex);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_mutex_unlock failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "MUTEX_UNLOCK_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_mutex_destroy(pthread_mutex_t *mutex) {
    int status = pthread_mutex_destroy(mutex);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_mutex_destroy failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "MUTEX_DESTROY_FAIL", error_msg);
    }
    return status;
}

// --- Condition Variables ---

int safe_pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    int status = pthread_cond_init(cond, attr);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_cond_init failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "COND_INIT_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int status = pthread_cond_wait(cond, mutex);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_cond_wait failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "COND_WAIT_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) {
    int status = pthread_cond_timedwait(cond, mutex, abstime);
    
    if (status != 0) {
        // Caller decides
        if (status != ETIMEDOUT) {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "pthread_cond_timedwait failed: %s", strerror(status));
            log_event(ERROR, LOG_COMPONENT, "COND_TIMEDWAIT_FAIL", error_msg);
        }
    }
    return status;
}

int safe_pthread_cond_signal(pthread_cond_t *cond) {
    int status = pthread_cond_signal(cond);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_cond_signal failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "COND_SIGNAL_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_cond_broadcast(pthread_cond_t *cond) {
    int status = pthread_cond_broadcast(cond);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_cond_broadcast failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "COND_BROADCAST_FAIL", error_msg);
    }
    return status;
}

int safe_pthread_cond_destroy(pthread_cond_t *cond) {
    int status = pthread_cond_destroy(cond);
    if (status != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "pthread_cond_destroy failed: %s", strerror(status));
        log_event(ERROR, LOG_COMPONENT, "COND_DESTROY_FAIL", error_msg);
    }
    return status;
}

