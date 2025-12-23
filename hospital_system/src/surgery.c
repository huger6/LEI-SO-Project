#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/msg.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "../include/hospital.h"
#include "../include/config.h"
#include "../include/mq.h"
#include "../include/shm.h"
#include "../include/sem.h"
#include "../include/log.h"
#include "../include/stats.h"
#include "../include/safe_threads.h"
#include "../include/time_simulation.h"
#include "../include/manager_utils.h"

// --- Constants ---
#define ROOM_FREE       0
#define ROOM_OCCUPIED   1
#define ROOM_CLEANING   2

#define SURGERY_CARDIO  0   // BO1
#define SURGERY_ORTHO   1   // BO2
#define SURGERY_NEURO   2   // BO3

#define MAX_CONCURRENT_SURGERIES 10

// --- Active Surgery Control Structure ---
// Each worker thread gets one of these for synchronization with the dispatcher
typedef struct ActiveSurgery {
    int surgery_id;
    char patient_id[20];
    int surgery_type;
    int urgency;
    int scheduled_time;
    int estimated_duration;
    int tests_count;
    int tests_id[5];
    int meds_count;
    int meds_id[5];
    
    // Synchronization - per-worker
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    
    // Status flags (protected by mutex)
    int tests_done;         // 1 = lab results received
    int meds_ok;            // 1 = pharmacy confirmed
    int needs_tests;        // 1 = requires lab tests
    int needs_meds;         // 1 = requires medications
    int active;             // 1 = still in workflow, 0 = completed/cancelled
    
    // Thread handle
    pthread_t thread;
    
    // Linked list for active surgeries registry
    struct ActiveSurgery *next;
} active_surgery_t;

// --- Globals ---

// Active surgeries registry (linked list)
static active_surgery_t *active_surgeries_head = NULL;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// Condition variable for medical teams
static pthread_cond_t teams_available_cond = PTHREAD_COND_INITIALIZER;

// --- Helper Functions ---

static int get_surgery_duration(int surgery_type) {
    int min_duration, max_duration;
    
    switch (surgery_type) {
        case SURGERY_CARDIO:
            min_duration = config->bo1_min_duration;
            max_duration = config->bo1_max_duration;
            break;
        case SURGERY_ORTHO:
            min_duration = config->bo2_min_duration;
            max_duration = config->bo2_max_duration;
            break;
        case SURGERY_NEURO:
            min_duration = config->bo3_min_duration;
            max_duration = config->bo3_max_duration;
            break;
        default:
            min_duration = 30;
            max_duration = 60;
            break;
    }
    
    return min_duration + (rand() % (max_duration - min_duration + 1));
}

static int get_cleanup_duration(void) {
    int min_clean = config->cleanup_min_time;
    int max_clean = config->cleanup_max_time;
    return min_clean + (rand() % (max_clean - min_clean + 1));
}

static const char* get_room_name(int surgery_type) {
    switch (surgery_type) {
        case SURGERY_CARDIO: return "BO1";
        case SURGERY_ORTHO:  return "BO2";
        case SURGERY_NEURO:  return "BO3";
        default: return "UNKNOWN";
    }
}

static sem_t* get_room_semaphore(int surgery_type) {
    switch (surgery_type) {
        case SURGERY_CARDIO: return sem_bo1;
        case SURGERY_ORTHO:  return sem_bo2;
        case SURGERY_NEURO:  return sem_bo3;
        default: return NULL;
    }
}

static const char* get_room_sem_name(int surgery_type) {
    switch (surgery_type) {
        case SURGERY_CARDIO: return SEM_NAME_BO1;
        case SURGERY_ORTHO:  return SEM_NAME_BO2;
        case SURGERY_NEURO:  return SEM_NAME_BO3;
        default: return "UNKNOWN";
    }
}

// --- Registry Management ---

static void register_surgery(active_surgery_t *surgery) {
    safe_pthread_mutex_lock(&registry_mutex);
    surgery->next = active_surgeries_head;
    active_surgeries_head = surgery;
    safe_pthread_mutex_unlock(&registry_mutex);
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Surgery %d registered for %s", 
             surgery->surgery_id, surgery->patient_id);
    log_event(INFO, "SURGERY", "REGISTERED", log_msg);
}

static void unregister_surgery(active_surgery_t *surgery) {
    safe_pthread_mutex_lock(&registry_mutex);
    
    active_surgery_t **curr = &active_surgeries_head;
    while (*curr) {
        if (*curr == surgery) {
            *curr = surgery->next;
            break;
        }
        curr = &(*curr)->next;
    }
    
    safe_pthread_mutex_unlock(&registry_mutex);
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Surgery %d unregistered for %s", 
             surgery->surgery_id, surgery->patient_id);
    log_event(INFO, "SURGERY", "UNREGISTERED", log_msg);
}

static active_surgery_t* find_surgery_by_id(int surgery_id) {
    // Caller must hold registry_mutex
    active_surgery_t *curr = active_surgeries_head;
    while (curr) {
        if (curr->surgery_id == surgery_id) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

// --- Async Request Senders (Non-blocking) ---

// Maximum tests that msg_lab_request_t can hold
#define MAX_LAB_TESTS_IN_MSG 5

static int send_lab_request_async(active_surgery_t *surgery) {
    if (surgery->tests_count <= 0) {
        surgery->needs_tests = 0;
        surgery->tests_done = 1;
        return 0;
    }
    
    surgery->needs_tests = 1;
    surgery->tests_done = 0;
    
    msg_lab_request_t req;
    memset(&req, 0, sizeof(msg_lab_request_t));
    
    req.hdr.mtype = MSG_LAB_REQUEST;
    req.hdr.kind = MSG_LAB_REQUEST;
    strncpy(req.hdr.patient_id, surgery->patient_id, sizeof(req.hdr.patient_id) - 1);
    req.hdr.operation_id = surgery->surgery_id;
    req.hdr.timestamp = time(NULL);
    
    // Clamp tests_count to message struct limit to prevent overflow
    int tests_to_copy = surgery->tests_count;
    if (tests_to_copy > MAX_LAB_TESTS_IN_MSG) {
        char warn_msg[128];
        snprintf(warn_msg, sizeof(warn_msg), "Truncating tests from %d to %d for %s",
                 surgery->tests_count, MAX_LAB_TESTS_IN_MSG, surgery->patient_id);
        log_event(WARNING, "SURGERY", "TESTS_TRUNCATED", warn_msg);
        tests_to_copy = MAX_LAB_TESTS_IN_MSG;
    }
    
    req.tests_count = tests_to_copy;
    for (int i = 0; i < tests_to_copy; i++) {
        req.tests_id[i] = surgery->tests_id[i];
    }
    
    if (send_generic_message(mq_lab_id, &req, sizeof(msg_lab_request_t)) != 0) {
        log_event(ERROR, "SURGERY", "LAB_REQUEST_FAIL", surgery->patient_id);
        return -1;
    }
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Async lab request sent for %s (surgery %d)", 
             surgery->patient_id, surgery->surgery_id);
    log_event(INFO, "SURGERY", "LAB_REQUEST", log_msg);
    
    return 0;
}

// Maximum meds that msg_pharmacy_request_t can hold
#define MAX_MEDS_IN_MSG 8
// Maximum meds in active_surgery_t source structure
#define MAX_MEDS_IN_SURGERY 5

// Timeout for waiting on Lab/Pharmacy responses (in simulation time units)
#define DEPENDENCY_TIMEOUT_UNITS 50

static int send_pharmacy_request_async(active_surgery_t *surgery) {
    if (surgery->meds_count <= 0) {
        surgery->needs_meds = 0;
        surgery->meds_ok = 1;
        return 0;
    }
    
    surgery->needs_meds = 1;
    surgery->meds_ok = 0;
    
    msg_pharmacy_request_t req;
    memset(&req, 0, sizeof(msg_pharmacy_request_t));
    
    req.hdr.mtype = MSG_PHARMACY_REQUEST;
    req.hdr.kind = MSG_PHARMACY_REQUEST;
    strncpy(req.hdr.patient_id, surgery->patient_id, sizeof(req.hdr.patient_id) - 1);
    req.hdr.operation_id = surgery->surgery_id;
    req.hdr.timestamp = time(NULL);
    
    // Safe copy: clamp to both source and destination limits
    int meds_to_copy = surgery->meds_count;
    if (meds_to_copy > MAX_MEDS_IN_SURGERY) {
        meds_to_copy = MAX_MEDS_IN_SURGERY;  // Don't read past source array
    }
    if (meds_to_copy > MAX_MEDS_IN_MSG) {
        meds_to_copy = MAX_MEDS_IN_MSG;  // Don't write past destination array
    }
    
    req.meds_count = meds_to_copy;
    for (int i = 0; i < meds_to_copy; i++) {
        req.meds_id[i] = surgery->meds_id[i];
        req.meds_qty[i] = 1;  // Default quantity
    }
    
    if (send_generic_message(mq_pharmacy_id, &req, sizeof(msg_pharmacy_request_t)) != 0) {
        log_event(ERROR, "SURGERY", "PHARM_REQUEST_FAIL", surgery->patient_id);
        return -1;
    }
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Async pharmacy request sent for %s (surgery %d)", 
             surgery->patient_id, surgery->surgery_id);
    log_event(INFO, "SURGERY", "PHARM_REQUEST", log_msg);
    
    return 0;
}

// --- Wait for Dependencies (Worker waits on its own cond var with timeout) ---

static int wait_for_dependencies(active_surgery_t *surgery) {
    struct timespec timeout;
    int rc;
    
    safe_pthread_mutex_lock(&surgery->mutex);
    
    while (!check_shutdown()) {
        // Check if both dependencies are satisfied
        int tests_satisfied = !surgery->needs_tests || surgery->tests_done;
        int meds_satisfied = !surgery->needs_meds || surgery->meds_ok;
        
        if (tests_satisfied && meds_satisfied) {
            safe_pthread_mutex_unlock(&surgery->mutex);
            log_event(INFO, "SURGERY", "DEPS_READY", surgery->patient_id);
            return 0;
        }
        
        // Calculate absolute timeout based on simulation time units
        // total_ms = DEPENDENCY_TIMEOUT_UNITS * config->time_unit_ms
        if (clock_gettime(CLOCK_REALTIME, &timeout) != 0) {
            log_event(ERROR, "SURGERY", "CLOCK_FAIL", "clock_gettime failed");
            safe_pthread_mutex_unlock(&surgery->mutex);
            return -1;
        }
        
        long total_ms = (long)DEPENDENCY_TIMEOUT_UNITS * config->time_unit_ms;
        long add_sec = total_ms / 1000;
        long add_nsec = (total_ms % 1000) * 1000000L;
        
        timeout.tv_sec += add_sec;
        timeout.tv_nsec += add_nsec;
        
        // Handle nanosecond overflow (tv_nsec must be < 1 billion)
        if (timeout.tv_nsec >= 1000000000L) {
            timeout.tv_sec += timeout.tv_nsec / 1000000000L;
            timeout.tv_nsec = timeout.tv_nsec % 1000000000L;
        }
        
        // Wait for dispatcher to signal us (with timeout)
        rc = safe_pthread_cond_timedwait(&surgery->cond, &surgery->mutex, &timeout);
        
        if (rc == ETIMEDOUT) {
            // Timeout expired - check one more time if dependencies were satisfied
            int tests_ok = !surgery->needs_tests || surgery->tests_done;
            int meds_ok_flag = !surgery->needs_meds || surgery->meds_ok;
            
            if (tests_ok && meds_ok_flag) {
                // Race condition: response arrived just as we timed out
                safe_pthread_mutex_unlock(&surgery->mutex);
                log_event(INFO, "SURGERY", "DEPS_READY", surgery->patient_id);
                return 0;
            }
            
            // Genuine timeout - log which dependency is missing
            char err_msg[150];
            snprintf(err_msg, sizeof(err_msg), 
                     "TIMEOUT waiting for Lab/Pharmacy response for %s (tests_done=%d, meds_ok=%d)",
                     surgery->patient_id, surgery->tests_done, surgery->meds_ok);
            log_event(ERROR, "SURGERY", "TIMEOUT", err_msg);
            
            safe_pthread_mutex_unlock(&surgery->mutex);
            return -1; // Timeout failure
        } else if (rc != 0 && rc != EINTR) {
            // Unexpected error
            char err_msg[100];
            snprintf(err_msg, sizeof(err_msg), "pthread_cond_timedwait failed: %d", rc);
            log_event(ERROR, "SURGERY", "COND_WAIT_FAIL", err_msg);
            safe_pthread_mutex_unlock(&surgery->mutex);
            return -1;
        }
        // rc == 0 means we were signaled, loop back and check conditions
        // rc == EINTR means interrupted, loop back and try again
    }
    
    safe_pthread_mutex_unlock(&surgery->mutex);
    return -1; // Shutdown
}

// --- Wait for Scheduled Time ---

static void wait_for_scheduled_time(active_surgery_t *surgery) {
    while (!check_shutdown()) {
        int current_time = get_simulation_time();
        if (current_time >= surgery->scheduled_time) {
            break;
        }
        wait_time_units(1);
    }
}

// --- Resource Acquisition ---

static int acquire_medical_team(active_surgery_t *surgery) {
    safe_pthread_mutex_lock(&shm_hospital->shm_surg->teams_mutex);
    
    while (shm_hospital->shm_surg->medical_teams_available <= 0 && !check_shutdown()) {
        safe_pthread_cond_wait(&teams_available_cond, &shm_hospital->shm_surg->teams_mutex);
    }
    
    if (check_shutdown()) {
        safe_pthread_mutex_unlock(&shm_hospital->shm_surg->teams_mutex);
        return -1;
    }
    
    shm_hospital->shm_surg->medical_teams_available--;
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Team acquired for %s (teams left: %d)", 
             surgery->patient_id, shm_hospital->shm_surg->medical_teams_available);
    log_event(INFO, "SURGERY", "TEAM_ACQUIRED", log_msg);
    
    safe_pthread_mutex_unlock(&shm_hospital->shm_surg->teams_mutex);
    return 0;
}

static void release_medical_team(active_surgery_t *surgery) {
    safe_pthread_mutex_lock(&shm_hospital->shm_surg->teams_mutex);
    
    shm_hospital->shm_surg->medical_teams_available++;
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Team released for %s (teams available: %d)", 
             surgery->patient_id, shm_hospital->shm_surg->medical_teams_available);
    log_event(INFO, "SURGERY", "TEAM_RELEASED", log_msg);
    
    safe_pthread_cond_signal(&teams_available_cond);
    safe_pthread_mutex_unlock(&shm_hospital->shm_surg->teams_mutex);
}

static int acquire_room(active_surgery_t *surgery) {
    sem_t *room_sem = get_room_semaphore(surgery->surgery_type);
    const char *sem_name = get_room_sem_name(surgery->surgery_type);
    
    if (!room_sem) {
        log_event(ERROR, "SURGERY", "INVALID_ROOM", surgery->patient_id);
        return -1;
    }
    
    // Wait for room to be free
    if (sem_wait_safe(room_sem, sem_name) != 0) {
        log_event(ERROR, "SURGERY", "ROOM_ACQUIRE_FAIL", surgery->patient_id);
        return -1;
    }
    
    if (check_shutdown()) {
        sem_post_safe(room_sem, sem_name);
        return -1;
    }
    
    // Update room status in shared memory
    surgery_room_t *room = &shm_hospital->shm_surg->rooms[surgery->surgery_type];
    safe_pthread_mutex_lock(&room->mutex);
    
    room->status = ROOM_OCCUPIED;
    strncpy(room->current_patient, surgery->patient_id, sizeof(room->current_patient) - 1);
    room->surgery_start_time = get_simulation_time();
    room->estimated_end_time = room->surgery_start_time + surgery->estimated_duration;
    
    safe_pthread_mutex_unlock(&room->mutex);
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Room %s acquired for %s", 
             get_room_name(surgery->surgery_type), surgery->patient_id);
    log_event(INFO, "SURGERY", "ROOM_ACQUIRED", log_msg);
    
    return 0;
}

static void release_room(active_surgery_t *surgery) {
    surgery_room_t *room = &shm_hospital->shm_surg->rooms[surgery->surgery_type];
    
    safe_pthread_mutex_lock(&room->mutex);
    room->status = ROOM_FREE;
    room->current_patient[0] = '\0';
    room->surgery_start_time = 0;
    room->estimated_end_time = 0;
    safe_pthread_mutex_unlock(&room->mutex);
    
    // Release semaphore
    sem_t *room_sem = get_room_semaphore(surgery->surgery_type);
    const char *sem_name = get_room_sem_name(surgery->surgery_type);
    sem_post_safe(room_sem, sem_name);
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Room %s released after %s", 
             get_room_name(surgery->surgery_type), surgery->patient_id);
    log_event(INFO, "SURGERY", "ROOM_RELEASED", log_msg);
}

// --- Surgery Execution ---

static void perform_surgery(active_surgery_t *surgery) {
    int duration = get_surgery_duration(surgery->surgery_type);
    
    char log_msg[150];
    snprintf(log_msg, sizeof(log_msg), "Surgery started for %s in %s (duration: %d units)", 
             surgery->patient_id, get_room_name(surgery->surgery_type), duration);
    log_event(INFO, "SURGERY", "SURGERY_START", log_msg);
    
    // Simulate surgery
    wait_time_units(duration);
    
    // Update statistics
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    switch (surgery->surgery_type) {
        case SURGERY_CARDIO:
            shm_hospital->shm_stats->total_surgeries_bo1++;
            shm_hospital->shm_stats->bo1_utilization_time += duration;
            break;
        case SURGERY_ORTHO:
            shm_hospital->shm_stats->total_surgeries_bo2++;
            shm_hospital->shm_stats->bo2_utilization_time += duration;
            break;
        case SURGERY_NEURO:
            shm_hospital->shm_stats->total_surgeries_bo3++;
            shm_hospital->shm_stats->bo3_utilization_time += duration;
            break;
    }
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    snprintf(log_msg, sizeof(log_msg), "Surgery completed for %s in %s", 
             surgery->patient_id, get_room_name(surgery->surgery_type));
    log_event(INFO, "SURGERY", "SURGERY_COMPLETE", log_msg);
}

static void cleanup_room(active_surgery_t *surgery) {
    surgery_room_t *room = &shm_hospital->shm_surg->rooms[surgery->surgery_type];
    
    // Set room to cleaning
    safe_pthread_mutex_lock(&room->mutex);
    room->status = ROOM_CLEANING;
    safe_pthread_mutex_unlock(&room->mutex);
    
    int cleanup_duration = get_cleanup_duration();
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Cleaning %s (duration: %d units)", 
             get_room_name(surgery->surgery_type), cleanup_duration);
    log_event(INFO, "SURGERY", "ROOM_CLEANING", log_msg);
    
    wait_time_units(cleanup_duration);
}

// --- Surgery Worker Thread ---

static void *surgery_worker(void *arg) {
    active_surgery_t *surgery = (active_surgery_t *)arg;
    
    char log_msg[150];
    snprintf(log_msg, sizeof(log_msg), "Surgery thread started for %s (type: %s, scheduled: %d)", 
             surgery->patient_id, get_room_name(surgery->surgery_type), surgery->scheduled_time);
    log_event(INFO, "SURGERY", "THREAD_START", log_msg);
    
    // ==========================================
    // Step 1: Registration (already done before thread creation)
    // ==========================================
    
    // ==========================================
    // Step 2: Send Async Requests
    // ==========================================
    
    safe_pthread_mutex_lock(&surgery->mutex);
    
    if (send_lab_request_async(surgery) != 0) {
        surgery->active = 0;
        safe_pthread_mutex_unlock(&surgery->mutex);
        goto surgery_cancelled;
    }
    
    if (send_pharmacy_request_async(surgery) != 0) {
        surgery->active = 0;
        safe_pthread_mutex_unlock(&surgery->mutex);
        goto surgery_cancelled;
    }
    
    safe_pthread_mutex_unlock(&surgery->mutex);
    
    // ==========================================
    // Step 3: Wait for Dependencies
    // ==========================================
    
    if (wait_for_dependencies(surgery) != 0) {
        goto surgery_cancelled;
    }
    
    if (check_shutdown()) goto surgery_cancelled;
    
    // ==========================================
    // Step 4: Wait for Scheduled Time
    // ==========================================
    
    wait_for_scheduled_time(surgery);
    
    if (check_shutdown()) goto surgery_cancelled;
    
    // ==========================================
    // Step 5: Resource Acquisition
    // ==========================================
    
    // 5a. Acquire room
    if (acquire_room(surgery) != 0) {
        goto surgery_cancelled;
    }
    
    // 5b. Acquire medical team
    if (acquire_medical_team(surgery) != 0) {
        release_room(surgery);
        goto surgery_cancelled;
    }
    
    if (check_shutdown()) {
        release_medical_team(surgery);
        release_room(surgery);
        goto surgery_cancelled;
    }
    
    // ==========================================
    // Step 6: Execution
    // ==========================================
    
    // 6a. Perform surgery
    perform_surgery(surgery);
    
    // 6b. Release medical team immediately
    release_medical_team(surgery);
    
    // 6c. Room cleanup
    cleanup_room(surgery);
    
    // 6d. Release room
    release_room(surgery);
    
    // 6e. Update statistics
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    shm_hospital->shm_stats->completed_surgeries++;
    shm_hospital->shm_stats->total_operations++;
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    snprintf(log_msg, sizeof(log_msg), "Surgery workflow complete for %s", surgery->patient_id);
    log_event(INFO, "SURGERY", "WORKFLOW_COMPLETE", log_msg);
    
    goto cleanup;

surgery_cancelled:
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    shm_hospital->shm_stats->cancelled_surgeries++;
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    snprintf(log_msg, sizeof(log_msg), "Surgery cancelled for %s", surgery->patient_id);
    log_event(WARNING, "SURGERY", "SURGERY_CANCELLED", log_msg);

cleanup:
    // Mark as inactive
    safe_pthread_mutex_lock(&surgery->mutex);
    surgery->active = 0;
    safe_pthread_mutex_unlock(&surgery->mutex);
    
    // Unregister from active surgeries
    unregister_surgery(surgery);
    
    // Cleanup synchronization primitives
    safe_pthread_mutex_destroy(&surgery->mutex);
    safe_pthread_cond_destroy(&surgery->cond);
    
    free(surgery);
    return NULL;
}

// --- Dispatcher: Handle Lab Results Response ---

static void handle_lab_response(int surgery_id) {
    safe_pthread_mutex_lock(&registry_mutex);
    
    active_surgery_t *surgery = find_surgery_by_id(surgery_id);
    if (surgery) {
        safe_pthread_mutex_lock(&surgery->mutex);
        surgery->tests_done = 1;
        
        char log_msg[100];
        snprintf(log_msg, sizeof(log_msg), "Lab results received for surgery %d (%s)", 
                 surgery_id, surgery->patient_id);
        log_event(INFO, "SURGERY", "LAB_RESPONSE", log_msg);
        
        safe_pthread_cond_signal(&surgery->cond);
        safe_pthread_mutex_unlock(&surgery->mutex);
    } else {
        char log_msg[100];
        snprintf(log_msg, sizeof(log_msg), "Lab response for unknown surgery %d", surgery_id);
        log_event(WARNING, "SURGERY", "ORPHAN_RESPONSE", log_msg);
    }
    
    safe_pthread_mutex_unlock(&registry_mutex);
}

// --- Dispatcher: Handle Pharmacy Response ---

static void handle_pharmacy_response(int surgery_id) {
    safe_pthread_mutex_lock(&registry_mutex);
    
    active_surgery_t *surgery = find_surgery_by_id(surgery_id);
    if (surgery) {
        safe_pthread_mutex_lock(&surgery->mutex);
        surgery->meds_ok = 1;
        
        char log_msg[100];
        snprintf(log_msg, sizeof(log_msg), "Pharmacy confirmation for surgery %d (%s)", 
                 surgery_id, surgery->patient_id);
        log_event(INFO, "SURGERY", "PHARM_RESPONSE", log_msg);
        
        safe_pthread_cond_signal(&surgery->cond);
        safe_pthread_mutex_unlock(&surgery->mutex);
    } else {
        char log_msg[100];
        snprintf(log_msg, sizeof(log_msg), "Pharmacy response for unknown surgery %d", surgery_id);
        log_event(WARNING, "SURGERY", "ORPHAN_RESPONSE", log_msg);
    }
    
    safe_pthread_mutex_unlock(&registry_mutex);
}

// --- Dispatcher: Broadcast Shutdown to All Workers ---

static void broadcast_shutdown_to_workers(void) {
    safe_pthread_mutex_lock(&registry_mutex);
    
    active_surgery_t *curr = active_surgeries_head;
    while (curr) {
        safe_pthread_mutex_lock(&curr->mutex);
        safe_pthread_cond_broadcast(&curr->cond);
        safe_pthread_mutex_unlock(&curr->mutex);
        curr = curr->next;
    }
    
    safe_pthread_mutex_unlock(&registry_mutex);
    
    // Also wake up anyone waiting for medical teams
    safe_pthread_mutex_lock(&shm_hospital->shm_surg->teams_mutex);
    safe_pthread_cond_broadcast(&teams_available_cond);
    safe_pthread_mutex_unlock(&shm_hospital->shm_surg->teams_mutex);
    
    log_event(INFO, "SURGERY", "SHUTDOWN_BROADCAST", "Shutdown signal sent to all workers");
}

// --- Dispatcher: Create New Surgery Worker ---

static void spawn_surgery_worker(msg_new_surgery_t *msg) {
    active_surgery_t *surgery = malloc(sizeof(active_surgery_t));
    if (!surgery) {
        log_event(ERROR, "SURGERY", "MALLOC_FAIL", "Failed to allocate surgery control structure");
        return;
    }
    
    // Initialize control structure
    memset(surgery, 0, sizeof(active_surgery_t));
    
    surgery->surgery_id = msg->hdr.operation_id;
    strncpy(surgery->patient_id, msg->hdr.patient_id, sizeof(surgery->patient_id) - 1);
    surgery->surgery_type = msg->surgery_type;
    surgery->urgency = msg->urgency;
    surgery->scheduled_time = msg->scheduled_time;
    surgery->estimated_duration = msg->estimated_duration;
    surgery->tests_count = msg->tests_count;
    memcpy(surgery->tests_id, msg->tests_id, sizeof(surgery->tests_id));
    surgery->meds_count = msg->meds_count;
    memcpy(surgery->meds_id, msg->meds_id, sizeof(surgery->meds_id));
    
    // Initialize synchronization primitives
    safe_pthread_mutex_init(&surgery->mutex, NULL);
    safe_pthread_cond_init(&surgery->cond, NULL);
    
    // Initialize status flags
    surgery->tests_done = 0;
    surgery->meds_ok = 0;
    surgery->needs_tests = (msg->tests_count > 0);
    surgery->needs_meds = (msg->meds_count > 0);
    surgery->active = 1;
    surgery->next = NULL;
    
    // Register in active surgeries list BEFORE spawning thread
    register_surgery(surgery);
    
    char log_msg[150];
    snprintf(log_msg, sizeof(log_msg), "New surgery: %s (type: %s, urgency: %d, scheduled: %d)", 
             surgery->patient_id, get_room_name(surgery->surgery_type), 
             surgery->urgency, surgery->scheduled_time);
    log_event(INFO, "SURGERY", "TASK_RECEIVED", log_msg);
    
    // Spawn worker thread
    if (safe_pthread_create(&surgery->thread, NULL, surgery_worker, surgery) != 0) {
        log_event(ERROR, "SURGERY", "THREAD_FAIL", "Failed to create surgery worker thread");
        unregister_surgery(surgery);
        safe_pthread_mutex_destroy(&surgery->mutex);
        safe_pthread_cond_destroy(&surgery->cond);
        free(surgery);
        return;
    }
    
    // Detach thread for automatic cleanup
    safe_pthread_detach(surgery->thread);
}

// --- Dispatcher Main Loop ---
// The ONLY thread that performs msgrcv

static void dispatcher_loop(void) {
    // Generic message buffer large enough for any message type
    union {
        msg_new_surgery_t surgery;
        msg_header_t header;
        msg_lab_results_t lab;
        msg_pharm_ready_t pharm;
    } msg_buf;
    
    while (!check_shutdown()) {
        memset(&msg_buf, 0, sizeof(msg_buf));
        
        // Receive ALL message types in FIFO order by using 0
        // This ensures we don't filter by mtype and can receive Lab/Pharmacy responses
        if (receive_generic_message(mq_surgery_id, &msg_buf, sizeof(msg_buf), 0) == -1) {
            if (errno == EINTR) continue;
            log_event(ERROR, "SURGERY", "MQ_ERROR", "Failed to receive message");
            break;
        }
        
        // Route message based on kind
        switch (msg_buf.header.kind) {
            case MSG_SHUTDOWN:
                log_event(INFO, "SURGERY", "SHUTDOWN_RECV", "Shutdown message received");
                set_shutdown();
                broadcast_shutdown_to_workers();
                return;
                
            case MSG_NEW_SURGERY:
                spawn_surgery_worker(&msg_buf.surgery);
                break;
                
            case MSG_LAB_RESULTS_READY:
                handle_lab_response(msg_buf.header.operation_id);
                break;
                
            case MSG_PHARM_READY:
                handle_pharmacy_response(msg_buf.header.operation_id);
                break;
                
            default:
                {
                    char log_msg[100];
                    snprintf(log_msg, sizeof(log_msg), "Unknown message kind: %d", msg_buf.header.kind);
                    log_event(WARNING, "SURGERY", "UNKNOWN_MSG", log_msg);
                }
                break;
        }
    }
    
    // Shutdown triggered externally
    broadcast_shutdown_to_workers();
}

// --- Main Surgery Process ---

void surgery_main(void) {
    log_event(INFO, "SURGERY", "STARTUP", "Surgery process started");
    
    // Seed random number generator
    srand(time(NULL) ^ getpid());
    
    // Initialize global condition variable for medical teams
    safe_pthread_cond_init(&teams_available_cond, NULL);
    
    // Run dispatcher loop (this is the main thread)
    dispatcher_loop();
    
    // Wait briefly for detached worker threads to complete
    wait_time_units(10);
    
    // Cleanup any remaining surgeries (shouldn't happen if workers exit cleanly)
    safe_pthread_mutex_lock(&registry_mutex);
    while (active_surgeries_head) {
        active_surgery_t *next = active_surgeries_head->next;
        safe_pthread_mutex_destroy(&active_surgeries_head->mutex);
        safe_pthread_cond_destroy(&active_surgeries_head->cond);
        free(active_surgeries_head);
        active_surgeries_head = next;
    }
    safe_pthread_mutex_unlock(&registry_mutex);
    
    // Cleanup global resources
    safe_pthread_cond_destroy(&teams_available_cond);
    
    log_event(INFO, "SURGERY", "SHUTDOWN", "Surgery process finished");
}