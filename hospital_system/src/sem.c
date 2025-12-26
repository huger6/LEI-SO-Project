#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>

#include "../include/sem.h"
#include "../include/log.h"
#include "../include/config.h"
#include "../include/manager_utils.h"

#define SEM_PERMS       0644

#define VAL_BO          1
#define VAL_TEAMS       2 
#define VAL_LAB         1
#define VAL_PHARMACY    4

sem_t *sem_bo1 = NULL;
sem_t *sem_bo2 = NULL;
sem_t *sem_bo3 = NULL;
sem_t *sem_medical_teams = NULL;
sem_t *sem_lab1 = NULL;
sem_t *sem_lab2 = NULL;
sem_t *sem_pharmacy = NULL;


// Helper function to handle the O_CREAT | O_EXCL logic for a single semaphore
static sem_t* _init_single_sem(const char *name, int value) {
    sem_t *sem_ptr;

    // Attempt to create the semaphore exclusively
    sem_ptr = sem_open(name, O_CREAT | O_EXCL, SEM_PERMS, value);

    if (sem_ptr != SEM_FAILED) {
        return sem_ptr;
    }

    // Check if failure was because it already exists
    if (errno == EEXIST) {
        // Reopen existing semaphore
        sem_ptr = sem_open(name, 0);
        if (sem_ptr == SEM_FAILED) {
            log_event(ERROR, "IPC", "SEM_FAIL", "Failed to open existing semaphore");
            return SEM_FAILED;
        }
        return sem_ptr;
    }

    // Generic creation failure
    log_event(ERROR, "IPC", "SEM_FAIL", "Failed to create semaphore");
    return SEM_FAILED;
}

int init_all_semaphores(void) {

    #ifdef DEBUG
        log_event(DEBUG_LOG, "IPC", "SEM_INIT", "Initializing named semaphores");
    #endif
    
    // 1. Surgery Block Operating Rooms
    if ((sem_bo1 = _init_single_sem(SEM_NAME_BO1, VAL_BO)) == SEM_FAILED) return -1;
    if ((sem_bo2 = _init_single_sem(SEM_NAME_BO2, VAL_BO)) == SEM_FAILED) return -1;
    if ((sem_bo3 = _init_single_sem(SEM_NAME_BO3, VAL_BO)) == SEM_FAILED) return -1;

    #ifdef DEBUG
        {
            char dbg[200];
            snprintf(dbg, sizeof(dbg), "Opened semaphores: BO1=%p BO2=%p BO3=%p", (void*)sem_bo1, (void*)sem_bo2, (void*)sem_bo3);
            log_event(DEBUG_LOG, "IPC", "SEM_OPEN", dbg);
        }
    #endif

    // 2. Medical Teams (Shared Resource)
    if ((sem_medical_teams = _init_single_sem(SEM_NAME_TEAMS, VAL_TEAMS)) == SEM_FAILED) return -1;

    #ifdef DEBUG
        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "Opened semaphore: TEAMS=%p", (void*)sem_medical_teams);
            log_event(DEBUG_LOG, "IPC", "SEM_OPEN", dbg);
        }
    #endif

    // 3. Laboratory Equipment (LAB1, LAB2)
    if ((sem_lab1 = _init_single_sem(SEM_NAME_LAB1, VAL_LAB)) == SEM_FAILED) return -1;
    if ((sem_lab2 = _init_single_sem(SEM_NAME_LAB2, VAL_LAB)) == SEM_FAILED) return -1;

    #ifdef DEBUG
        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "Opened semaphores: LAB1=%p LAB2=%p", (void*)sem_lab1, (void*)sem_lab2);
            log_event(DEBUG_LOG, "IPC", "SEM_OPEN", dbg);
        }
    #endif

    // 4. Pharmacy Access
    if ((sem_pharmacy = _init_single_sem(SEM_NAME_PHARMACY, VAL_PHARMACY)) == SEM_FAILED) return -1;

    #ifdef DEBUG
        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "Opened semaphore: PHARMACY=%p", (void*)sem_pharmacy);
            log_event(DEBUG_LOG, "IPC", "SEM_OPEN", dbg);
        }
    #endif

    return 0;
}

// Helper to safely close a single semaphore
static void _close_single_sem(sem_t **sem_ptr, const char *name) {
    (void)name; // unused now
    if (*sem_ptr != NULL && *sem_ptr != SEM_FAILED) {
        sem_close(*sem_ptr);
        *sem_ptr = NULL;
    }
}

/**
 * Closes all opened semaphore handles for the calling process.
 * Does NOT unlink the semaphores.
 */
void close_all_semaphores(void) {
    #ifdef DEBUG
        log_event(DEBUG_LOG, "IPC", "SEM_CLOSE", "Closing semaphore handles for this process");
    #endif
    // 1. Close Surgery Block Semaphores
    _close_single_sem(&sem_bo1, "BO1");
    _close_single_sem(&sem_bo2, "BO2");
    _close_single_sem(&sem_bo3, "BO3");

    // 2. Close Medical Teams Semaphore
    _close_single_sem(&sem_medical_teams, "Medical Teams");

    // 3. Close Laboratory Semaphores
    _close_single_sem(&sem_lab1, "LAB1");
    _close_single_sem(&sem_lab2, "LAB2");

    // 4. Close Pharmacy Semaphore
    _close_single_sem(&sem_pharmacy, "Pharmacy");
}

/**
 * Helper to safely unlink a single semaphore.
 * Returns -1 on failure, 0 on success.
 */
static int _unlink_single_sem(const char *name) {
    if (sem_unlink(name) != 0 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

/**
 * Removes all POSIX named semaphores from the system.
 * * Returns:
 * 0: All semaphores removed successfully.
 * -1: At least one semaphore failed to be removed.
 */
int unlink_all_semaphores(void) {
    int final_status = 0;

    #ifdef DEBUG
        log_event(DEBUG_LOG, "IPC", "SEM_UNLINK", "Unlinking named semaphores");
    #endif

    // 1. Unlink Surgery Block Semaphores
    if (_unlink_single_sem(SEM_NAME_BO1) != 0) final_status = -1;
    if (_unlink_single_sem(SEM_NAME_BO2) != 0) final_status = -1;
    if (_unlink_single_sem(SEM_NAME_BO3) != 0) final_status = -1;

    // 2. Unlink Medical Teams Semaphore
    if (_unlink_single_sem(SEM_NAME_TEAMS) != 0) final_status = -1;

    // 3. Unlink Laboratory Semaphores
    if (_unlink_single_sem(SEM_NAME_LAB1) != 0) final_status = -1;
    if (_unlink_single_sem(SEM_NAME_LAB2) != 0) final_status = -1;

    // 4. Unlink Pharmacy Semaphore
    if (_unlink_single_sem(SEM_NAME_PHARMACY) != 0) final_status = -1;

    #ifdef DEBUG
        char dbg[80];
        snprintf(dbg, sizeof(dbg), "unlink_all_semaphores() -> %d", final_status);
        log_event(DEBUG_LOG, "IPC", "SEM_UNLINK_DONE", dbg);
    #endif

    return final_status;
}

/**
 * Wrapper for sem_wait that handles EINTR, checks for shutdown, and uses a timeout
 * @param sem Pointer to the semaphore to wait on
 * @param sem_name Name of the semaphore (for logging purposes)
 * @return 0 on success, -1 on failure or shutdown
 */
int sem_wait_safe(sem_t *sem, const char *sem_name) {
    char log_buffer[256];
    // Safety check for null pointers
    if (sem == NULL) {
        snprintf(log_buffer, sizeof(log_buffer), "Attempted to wait on NULL semaphore: %s", sem_name ? sem_name : "UNKNOWN");
        log_event(ERROR, "SEMAPHORE", "SEM_WAIT_NULL", log_buffer);
        return -1;
    }

    while (1) {
        // Check for shutdown before waiting
        if (check_shutdown()) {
            return -1;
        }
        
        // Use sem_timedwait with a short timeout to allow periodic shutdown checks
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000; // 100ms timeout
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        
        if (sem_timedwait(sem, &ts) == 0) {
            return 0; // Success: Resource acquired
        }

        // Handle specific errors
        if (errno == EINTR) {
            // The call was interrupted by a signal handler
            continue;
        } else if (errno == ETIMEDOUT) {
            // Timeout expired - loop back and check shutdown again
            continue;
        } else {
            snprintf(log_buffer, sizeof(log_buffer), "sem_wait failed on %s. Errno: %d", sem_name ? sem_name : "UNKNOWN", errno);
            log_event(ERROR, "SEMAPHORE", "SEM_WAIT_FAIL", log_buffer);
            return -1;
        }
    }
}

/**
 * Wrapper for sem_post that logs errors
 * @param sem Pointer to the semaphore to post to
 * @param sem_name Name of the semaphore (for logging purposes)
 * @return 0 on success, -1 on failure
 */
int sem_post_safe(sem_t *sem, const char *sem_name) {
    char log_buffer[256];
    if (sem == NULL) {
        snprintf(log_buffer, sizeof(log_buffer), "Attempted to post to NULL semaphore: %s", sem_name ? sem_name : "UNKNOWN");
        log_event(ERROR, "SEMAPHORE", "SEM_POST_NULL", log_buffer);
        return -1;
    }

    if (sem_post(sem) != 0) {
        snprintf(log_buffer, sizeof(log_buffer), "sem_post failed on %s. Errno: %d", sem_name ? sem_name : "UNKNOWN", errno);
        log_event(ERROR, "SEMAPHORE", "SEM_POST_FAIL", log_buffer);
        return -1;
    }

    return 0;
}
