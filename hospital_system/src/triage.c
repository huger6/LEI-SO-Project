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
#include "../include/pipes.h"

// --- Constants & Macros ---
#define MAX_TREATMENT_THREADS 3
#define PATIENT_TYPE_EMERGENCY 1
#define PATIENT_TYPE_APPOINTMENT 2
#define MAX_WAIT_DEPENDENCIES_TIME 8000

// Triage operation ID range: 1000-1999 (Manager uses 2000+)
#define MIN_TRIAGE_OP_ID 1000
#define MAX_TRIAGE_OP_ID 1999

// --- Internal Structures ---

typedef struct TriagePatient {
    char id[20];
    int type; // PATIENT_TYPE_EMERGENCY or PATIENT_TYPE_APPOINTMENT
    int priority; // 1-5 (1 is highest)
    int stability;
    int arrival_time;
    int scheduled_time; // only for appointments
    int is_critical; // 0 or 1
    
    // Medical Data
    int tests_count;
    int tests_id[3];
    int meds_count;
    int meds_id[5];
    int doctor_specialty; // For appointments

    struct TriagePatient *next;
} TriagePatient;

// --- Pending Patient Structure ---
// Patients waiting on hold for pharmacy/lab responses (worker freed)
typedef struct PendingPatient {
    char id[20];
    int type;
    int priority;
    int stability;
    int arrival_time;
    int scheduled_time;
    int is_critical;
    int tests_count;
    int tests_id[3];
    int meds_count;
    int meds_id[5];
    int doctor_specialty;
    
    // Request tracking
    int operation_id;       // ID used for pharmacy/lab requests
    int waiting_meds;       // 1 if waiting for pharmacy
    int waiting_labs;       // 1 if waiting for labs
    int meds_ok;            // 1 = pharmacy responded
    int labs_ok;            // 1 = lab responded
    int hold_start_time;    // Simulation time when put on hold
    
    struct PendingPatient *next;
} PendingPatient;

typedef struct {
    TriagePatient *head;
    int count;
    pthread_mutex_t mutex;
} PatientQueue;

// Global operation ID counter for pending patients (range: 1000-1999)
static int next_pending_op_id = MIN_TRIAGE_OP_ID;
static pthread_mutex_t pending_op_id_mutex = PTHREAD_MUTEX_INITIALIZER;

static int pending_patients_count = 0;

// --- Globals ---

// Queues
PatientQueue emergency_queue;
PatientQueue appointment_queue;

// Pending patients list (on hold waiting for pharmacy/lab)
static PendingPatient *pending_patients_head = NULL;
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;

// Synchronization
pthread_mutex_t treatment_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t patient_ready_cond = PTHREAD_COND_INITIALIZER;
int active_treatments = 0;

// Threads
pthread_t t_emergency_mgr;
pthread_t t_appointment_mgr;
pthread_t t_vital_monitor;
pthread_t t_response_dispatcher;
pthread_t t_treatments[MAX_TREATMENT_THREADS];

// --- Helper Functions ---

/**
 * Determine message priority (mtype) based on patient status.
 * 
 * Priority determination logic:
 * - URGENT (1): Critical patients (stability <= threshold) OR highest priority (1)
 * - HIGH (2): Low stability (< 2x threshold) OR high priority (2)
 * - NORMAL (3): All other cases
 * 
 * @param priority  Patient's triage priority level (1-5, where 1 is highest)
 * @param stability Patient's stability value (lower = more critical)
 * @return PRIORITY_URGENT, PRIORITY_HIGH, or PRIORITY_NORMAL
 */
static long determine_patient_mtype(int priority, int stability) {
    int critical_threshold = config->triage_critical_stability;
    
    // Critical patients or highest priority = URGENT
    if (stability <= critical_threshold || priority == 1) {
        return PRIORITY_URGENT;
    }
    
    // Low stability (< 2x threshold) or high priority (2) = HIGH
    if (stability < (critical_threshold * 2) || priority == 2) {
        return PRIORITY_HIGH;
    }
    
    // All other cases = NORMAL
    return PRIORITY_NORMAL;
}

static void wake_all_threads(void) {
    safe_pthread_mutex_lock(&treatment_mutex);

    set_shutdown(); // it should be 1, but just to be sure

    safe_pthread_cond_broadcast(&patient_ready_cond);

    msg_new_appointment_t poison_pill;
    memset(&poison_pill, 0, sizeof(msg_new_appointment_t));
    poison_pill.hdr.mtype = MSG_NEW_APPOINTMENT;
    poison_pill.hdr.kind = MSG_SHUTDOWN;
    send_generic_message(mq_triage_id, &poison_pill, sizeof(msg_new_appointment_t));

    safe_pthread_mutex_unlock(&treatment_mutex);
}

// --- Pending Patient Management Functions ---

static int get_next_pending_op_id(void) {
    pthread_mutex_lock(&pending_op_id_mutex);
    int id = next_pending_op_id++;
    // Wraparound to stay within triage range (1000-1999)
    if (next_pending_op_id > MAX_TRIAGE_OP_ID) {
        next_pending_op_id = MIN_TRIAGE_OP_ID;
    }
    pthread_mutex_unlock(&pending_op_id_mutex);
    return id;
}

static void add_to_pending(TriagePatient *p, int operation_id, int waiting_meds, int waiting_labs) {
    PendingPatient *pending = malloc(sizeof(PendingPatient));
    if (!pending) {
        log_event(ERROR, "TRIAGE", "MALLOC_FAIL", "Failed to allocate pending patient");
        return;
    }
    
    strncpy(pending->id, p->id, sizeof(pending->id) - 1);
    pending->type = p->type;
    pending->priority = p->priority;
    pending->stability = p->stability;
    pending->arrival_time = p->arrival_time;
    pending->scheduled_time = p->scheduled_time;
    pending->is_critical = p->is_critical;
    pending->tests_count = p->tests_count;
    memcpy(pending->tests_id, p->tests_id, sizeof(pending->tests_id));
    pending->meds_count = p->meds_count;
    memcpy(pending->meds_id, p->meds_id, sizeof(pending->meds_id));
    pending->doctor_specialty = p->doctor_specialty;
    
    pending->operation_id = operation_id;
    pending->waiting_meds = waiting_meds;
    pending->waiting_labs = waiting_labs;
    pending->meds_ok = 0;
    pending->labs_ok = 0;
    pending->hold_start_time = get_simulation_time();
    pending->next = NULL;
    
    safe_pthread_mutex_lock(&pending_mutex);
    pending->next = pending_patients_head;
    pending_patients_head = pending;
    safe_pthread_mutex_unlock(&pending_mutex);
    
    char log_msg[100];
    snprintf(log_msg, sizeof(log_msg), "Patient %s put on hold (op_id=%d)", p->id, operation_id);
    log_event(INFO, "TRIAGE", "ON_HOLD", log_msg);
}

static PendingPatient* find_pending_by_op_id(int operation_id) {
    // Caller must hold pending_mutex
    PendingPatient *curr = pending_patients_head;
    while (curr) {
        if (curr->operation_id == operation_id) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

static PendingPatient* remove_pending_by_op_id(int operation_id) {
    // Caller must hold pending_mutex
    PendingPatient **curr = &pending_patients_head;
    while (*curr) {
        if ((*curr)->operation_id == operation_id) {
            PendingPatient *removed = *curr;
            *curr = removed->next;
            return removed;
        }
        curr = &(*curr)->next;
    }
    return NULL;
}

static void complete_pending_patient(PendingPatient *pending) {
    // Called when all dependencies are satisfied
    log_event(INFO, "TRIAGE", "TREATMENT_COMPLETE", pending->id);
    
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    if (pending->type == PATIENT_TYPE_EMERGENCY) {
        shm_hospital->shm_stats->completed_emergencies++;
    } else {
        shm_hospital->shm_stats->completed_appointments++;
    }
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    free(pending);
}

static void check_pending_timeouts(void) {
    int current_time = get_simulation_time();
    
    safe_pthread_mutex_lock(&pending_mutex);
    
    PendingPatient **curr = &pending_patients_head;
    while (*curr) {
        int wait_time = current_time - (*curr)->hold_start_time;
        if (wait_time >= MAX_WAIT_DEPENDENCIES_TIME) {
            PendingPatient *expired = *curr;
            *curr = expired->next;
            
            char log_msg[150];
            snprintf(log_msg, sizeof(log_msg), 
                     "Patient %s released (exceeded max hold time of %d)",
                     expired->id, MAX_WAIT_DEPENDENCIES_TIME);
            log_event(WARNING, "TRIAGE", "HOLD_TIMEOUT", log_msg);
            
            #ifdef DEBUG
                {
                    char debug_msg[120];
                    snprintf(debug_msg, sizeof(debug_msg), 
                            "TRIAGE_TIMEOUT: %s (meds: %d/%d; tests: %d/%d)", 
                            expired->id, 
                            expired->meds_ok, expired->waiting_meds,
                            expired->labs_ok, expired->waiting_labs);
                    log_event(DEBUG_LOG, "TRIAGE", "TIMEOUT_STATUS", debug_msg);
                }
            #endif
            
            free(expired);
        } else {
            curr = &(*curr)->next;
        }
    }
    
    safe_pthread_mutex_unlock(&pending_mutex);
}

// --- Response Dispatcher Thread ---
// Handles responses for both active workers and pending patients

void *response_dispatcher(void *arg) {
    (void)arg;
    
    // Buffer for receiving messages
    union {
        msg_header_t hdr;
        msg_pharm_ready_t pharm;
        msg_lab_results_t lab;
        char buffer[512];
    } msg;
    
    while (!check_shutdown()) {
        memset(&msg, 0, sizeof(msg));
        
        // Receive messages with mtype <= MAX_TRIAGE_OP_ID (1999)
        // This uses receive_message_up_to_type which calls msgrcv with negative msgtyp
        // ensuring we only get Triage messages and leave Manager messages (2000+) in the queue
        int result = receive_message_up_to_type(mq_responses_id, &msg, sizeof(msg), MAX_TRIAGE_OP_ID);
        #ifdef DEBUG
            log_event(DEBUG_LOG, "TRIAGE", "RESPONSE_DISPATCHER", "Awake");
        #endif
        if (result == -1) {
            #ifdef DEBUG
                log_event(DEBUG_LOG, "TRIAGE", "RESPONSE_DISPATCHER", "Received messaged status -1");
            #endif
            if (errno == EINTR) continue;
            if (check_shutdown()) break;
            continue;
        }
        
        // Check for shutdown message (shutdown for triage uses priority mtype like PRIORITY_NORMAL=3)
        if (msg.hdr.kind == MSG_SHUTDOWN) {
            #ifdef DEBUG
                log_event(DEBUG_LOG, "TRIAGE", "RESPONSE_DISPATCHER", "Received shutdown message");
            #endif
            break;
        }
        
        // Extract operation_id from mtype
        int operation_id = (int)msg.hdr.mtype;
        
        // Check if this is for a pending patient (op_id in triage range)
        if (operation_id >= MIN_TRIAGE_OP_ID && operation_id <= MAX_TRIAGE_OP_ID) {
            safe_pthread_mutex_lock(&pending_mutex);
            PendingPatient *pending = find_pending_by_op_id(operation_id);
            
            if (pending) {
                // Update flags based on message kind
                if (msg.hdr.kind == MSG_PHARM_READY) {
                    pending->meds_ok = 1;
                } else if (msg.hdr.kind == MSG_LAB_RESULTS_READY) {
                    pending->labs_ok = 1;
                }
                
                // Check if all dependencies are satisfied
                int meds_done = !pending->waiting_meds || pending->meds_ok;
                int labs_done = !pending->waiting_labs || pending->labs_ok;
                
                if (meds_done && labs_done) {
                    // Remove from pending and complete
                    PendingPatient *removed = remove_pending_by_op_id(operation_id);
                    safe_pthread_mutex_unlock(&pending_mutex);
                    
                    if (removed) {
                        complete_pending_patient(removed);
                    }
                } else {
                    safe_pthread_mutex_unlock(&pending_mutex);
                }
            } else {
                safe_pthread_mutex_unlock(&pending_mutex);
            }
        }
        
        // Check for expired pending patients after each message
        check_pending_timeouts();
    }
    
    return NULL;
}

void init_queue(PatientQueue *q) {
    q->head = NULL;
    q->count = 0;
    safe_pthread_mutex_init(&q->mutex, NULL);
}

// Insert into Emergency Queue (Sorted: Critical > Priority ASC > Arrival ASC)
void insert_emergency_sorted(TriagePatient *p) {
    // If critical, priority is effectively 0 (super high)
    // But we keep p->priority as is, and use p->is_critical for sorting
    
    TriagePatient **curr = &emergency_queue.head;
    while (*curr) {
        TriagePatient *entry = *curr;
        
        // 1. Critical Status
        if (p->is_critical && !entry->is_critical) {
            break; // p goes before entry
        }
        if (!p->is_critical && entry->is_critical) {
            curr = &entry->next;
            continue; // p goes after entry
        }
        
        // 2. Priority (Lower value = Higher priority)
        if (p->priority < entry->priority) {
            break;
        }
        if (p->priority > entry->priority) {
            curr = &entry->next;
            continue;
        }
        
        // 3. Arrival Time (Lower = Earlier = Higher priority)
        if (p->arrival_time < entry->arrival_time) {
            break;
        }
        
        curr = &entry->next;
    }
    
    p->next = *curr;
    *curr = p;
    emergency_queue.count++;
}

// Insert into Appointment Queue (Sorted: Scheduled Time ASC)
void insert_appointment_sorted(TriagePatient *p) {
    TriagePatient **curr = &appointment_queue.head;
    while (*curr) {
        if (p->scheduled_time < (*curr)->scheduled_time) {
            break;
        }
        curr = &(*curr)->next;
    }
    p->next = *curr;
    *curr = p;
    appointment_queue.count++;
}

// Remove specific patient from queue (for moving to emergency or deletion)
// Returns 1 if found and removed, 0 otherwise
int remove_patient_from_queue(PatientQueue *q, TriagePatient *p) {
    TriagePatient **curr = &q->head;
    while (*curr) {
        if (*curr == p) {
            *curr = p->next;
            q->count--;
            return 1;
        }
        curr = &(*curr)->next;
    }
    return 0;
}

// --- Thread Functions ---

void *emergency_queue_manager(void *arg) {
    (void)arg;
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_START", "Emergency queue manager thread started");
    #endif

    msg_new_emergency_t msg;
    while (!check_shutdown()) {
        if (receive_specific_message(mq_triage_id, &msg, sizeof(msg_new_emergency_t), MSG_NEW_EMERGENCY) == -1) {
            if (errno == EINTR) continue;
            #ifdef DEBUG
                char err_msg[128];
                snprintf(err_msg, sizeof(err_msg), "Failed to receive emergency msg, errno=%d (%s)", errno, strerror(errno));
                log_event(DEBUG_LOG, "TRIAGE", "MQ_RECEIVE_FAIL", err_msg);
            #endif
            log_event(ERROR, "TRIAGE", "MQ_ERROR", "Failed to receive emergency msg");
            break;
        }
        
        if (msg.hdr.kind == MSG_SHUTDOWN) {
            #ifdef DEBUG
                log_event(DEBUG_LOG, "TRIAGE", "THREAD_SHUTDOWN", "Emergency queue manager received shutdown");
            #endif
            wake_all_threads();
            break;
        }

        safe_pthread_mutex_lock(&emergency_queue.mutex);
        if (emergency_queue.count >= config->max_emergency_patients) {
            log_event(WARNING, "TRIAGE", "REJECTED", msg.hdr.patient_id);
            
            safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
            shm_hospital->shm_stats->rejected_patients++;
            safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
            
            safe_pthread_mutex_unlock(&emergency_queue.mutex);
            continue;
        }

        TriagePatient *p = malloc(sizeof(TriagePatient));
        strcpy(p->id, msg.hdr.patient_id);
        p->type = PATIENT_TYPE_EMERGENCY;
        p->priority = msg.triage_level;
        p->stability = msg.stability;
        p->arrival_time = get_simulation_time();
        p->is_critical = (p->stability <= config->triage_critical_stability);
        p->tests_count = msg.tests_count;
        memcpy(p->tests_id, msg.tests_id, sizeof(p->tests_id));
        p->meds_count = msg.meds_count;
        memcpy(p->meds_id, msg.meds_id, sizeof(p->meds_id));
        p->doctor_specialty = 0; 

        insert_emergency_sorted(p);
        log_event(INFO, "TRIAGE", "PATIENT_ADDED", p->id);
        
        // Update Stats
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        shm_hospital->shm_stats->total_emergency_patients++;
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);

        safe_pthread_mutex_unlock(&emergency_queue.mutex);

        // Increment pending work counter and signal workers
        safe_pthread_mutex_lock(&treatment_mutex);
        pending_patients_count++;
        safe_pthread_cond_signal(&patient_ready_cond);
        safe_pthread_mutex_unlock(&treatment_mutex);
    }
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_EXIT", "Emergency queue manager thread exiting");
    #endif
    
    return NULL;
}

void *appointment_queue_manager(void *arg) {
    (void)arg;
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_START", "Appointment queue manager thread started");
    #endif

    msg_new_appointment_t msg;
    while (!check_shutdown()) {
        if (receive_specific_message(mq_triage_id, &msg, sizeof(msg_new_appointment_t), MSG_NEW_APPOINTMENT) == -1) {
            if (errno == EINTR) continue;
            #ifdef DEBUG
                char err_msg[128];
                snprintf(err_msg, sizeof(err_msg), "Failed to receive appointment msg, errno=%d (%s)", errno, strerror(errno));
                log_event(DEBUG_LOG, "TRIAGE", "MQ_RECEIVE_FAIL", err_msg);
            #endif
            break;
        }
        
        if (msg.hdr.kind == MSG_SHUTDOWN) {
            #ifdef DEBUG
                log_event(DEBUG_LOG, "TRIAGE", "THREAD_SHUTDOWN", "Appointment queue manager received shutdown");
            #endif
            safe_pthread_mutex_lock(&treatment_mutex);

            safe_pthread_cond_broadcast(&patient_ready_cond);

            safe_pthread_mutex_unlock(&treatment_mutex);
            break;
        }

        safe_pthread_mutex_lock(&appointment_queue.mutex);
        if (appointment_queue.count >= config->max_appointments) {
            log_event(WARNING, "TRIAGE", "REJECTED_APPT", msg.hdr.patient_id);
            safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
            shm_hospital->shm_stats->rejected_patients++;
            safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
            safe_pthread_mutex_unlock(&appointment_queue.mutex);
            continue;
        }

        TriagePatient *p = malloc(sizeof(TriagePatient));
        strcpy(p->id, msg.hdr.patient_id);
        p->type = PATIENT_TYPE_APPOINTMENT;
        p->priority = 5; // Lowest priority default
        p->stability = 1000; // Max stability
        p->arrival_time = get_simulation_time();
        p->scheduled_time = msg.scheduled_time;
        p->is_critical = 0;
        p->tests_count = msg.tests_count;
        memcpy(p->tests_id, msg.tests_id, sizeof(p->tests_id));
        p->meds_count = 0;
        p->doctor_specialty = msg.doctor_specialty;

        insert_appointment_sorted(p);
        log_event(INFO, "TRIAGE", "APPT_ADDED", p->id);

        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        shm_hospital->shm_stats->total_appointments++;
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);

        safe_pthread_mutex_unlock(&appointment_queue.mutex);
        
        // Increment pending work counter and signal workers
        safe_pthread_mutex_lock(&treatment_mutex);
        pending_patients_count++;
        safe_pthread_cond_signal(&patient_ready_cond);
        safe_pthread_mutex_unlock(&treatment_mutex);
    }
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_EXIT", "Appointment queue manager thread exiting");
    #endif
    
    return NULL;
}

void *vital_stability_monitor(void *arg) {
    (void)arg;

    while (!check_shutdown()) {
        wait_time_units(1); // Sleep 1 time unit

        // 1. Check Emergency Queue
        safe_pthread_mutex_lock(&emergency_queue.mutex);
        TriagePatient *curr = emergency_queue.head;
        TriagePatient *prev = NULL;
        
        while (curr) {
            // Decrement 1 point of stability per 1 time unit passed
            curr->stability--;
            
            // Check Death/Transfer
            if (curr->stability <= 0) {
                log_event(CRITICAL, "TRIAGE", "PATIENT_DIED", curr->id);
                
                // Remove
                if (prev) prev->next = curr->next;
                else emergency_queue.head = curr->next;
                
                TriagePatient *to_free = curr;
                curr = curr->next;
                emergency_queue.count--;
                free(to_free);
                continue;
            }
            
            // Check Critical
            if (!curr->is_critical && curr->stability <= config->triage_critical_stability) {
                curr->is_critical = 1;
                log_event(CRITICAL, "TRIAGE", "CRITICAL_STATUS", curr->id);
                
                // We need to re-insert to maintain sort order (Critical > others)
                // Remove from current position
                if (prev) prev->next = curr->next;
                else emergency_queue.head = curr->next;
                
                TriagePatient *to_move = curr;
                curr = curr->next; // Advance iteration
                emergency_queue.count--; // Decrement temporarily
                
                // Re-insert
                insert_emergency_sorted(to_move);
                // Note: insert_emergency_sorted increments count
                
                // Since we moved 'to_move', we don't know where it landed relative to 'curr'.
                // But 'curr' is the next node in the original list, so we are safe to continue.
                continue;
            }
            
            prev = curr;
            curr = curr->next;
        }
        safe_pthread_mutex_unlock(&emergency_queue.mutex);

        // 2. Check Appointment Queue (no stability decrement for appointments)
        safe_pthread_mutex_lock(&appointment_queue.mutex);
        curr = appointment_queue.head;
        prev = NULL;
        
        while (curr) {
            // Appointments don't have stability decremented, but check if already critical
            if (curr->stability <= config->triage_critical_stability) {
                // Move to Emergency Queue
                log_event(CRITICAL, "TRIAGE", "APPT_CRITICAL", curr->id);
                
                if (prev) prev->next = curr->next;
                else appointment_queue.head = curr->next;
                
                TriagePatient *to_move = curr;
                curr = curr->next;
                appointment_queue.count--;
                
                // Add to Emergency Queue
                to_move->is_critical = 1;
                to_move->type = PATIENT_TYPE_EMERGENCY; // Convert to emergency
                
                safe_pthread_mutex_lock(&emergency_queue.mutex);
                insert_emergency_sorted(to_move);
                safe_pthread_mutex_unlock(&emergency_queue.mutex);
                
                safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
                shm_hospital->shm_stats->critical_transfers++;
                safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);

                continue;
            }
            
            prev = curr;
            curr = curr->next;
        }
        safe_pthread_mutex_unlock(&appointment_queue.mutex);
    }
    return NULL;
}

void *treatment_worker(void *arg) {
    int thread_id = *(int*)arg;
    free(arg);
    
    // Thread 2 is the "Appointment Specialist", Threads 0 and 1 are "General/Emergency Workers"
    int is_appointment_specialist = (thread_id == 2);
    
    while (!check_shutdown()) {
        TriagePatient *p = NULL;
        
        safe_pthread_mutex_lock(&treatment_mutex);
        // Wait until there is pending work or shutdown is requested
        while (pending_patients_count == 0 && !check_shutdown()) {
            safe_pthread_cond_wait(&patient_ready_cond, &treatment_mutex);
        }
        
        if (check_shutdown()) {
            safe_pthread_mutex_unlock(&treatment_mutex);
            break;
        }
        
        // Claim work by decrementing the counter while holding treatment_mutex
        pending_patients_count--;
        safe_pthread_mutex_unlock(&treatment_mutex);
        
        // Now acquire queue mutexes to get the actual patient
        if (is_appointment_specialist) {
            // Appointment Specialist: Try Appointment Queue first
            safe_pthread_mutex_lock(&appointment_queue.mutex);
            if (appointment_queue.head) {
                p = appointment_queue.head;
                appointment_queue.head = p->next;
                appointment_queue.count--;
            }
            safe_pthread_mutex_unlock(&appointment_queue.mutex);
            
            // Fallback to Emergency Queue if Appointment Queue is empty
            if (!p) {
                safe_pthread_mutex_lock(&emergency_queue.mutex);
                if (emergency_queue.head) {
                    p = emergency_queue.head;
                    emergency_queue.head = p->next;
                    emergency_queue.count--;
                }
                safe_pthread_mutex_unlock(&emergency_queue.mutex);
            }
        } else {
            // General/Emergency Workers: Try Emergency Queue first
            safe_pthread_mutex_lock(&emergency_queue.mutex);
            if (emergency_queue.head) {
                p = emergency_queue.head;
                emergency_queue.head = p->next;
                emergency_queue.count--;
            }
            safe_pthread_mutex_unlock(&emergency_queue.mutex);
            
            // Fallback to Appointment Queue if Emergency Queue is empty
            if (!p) {
                safe_pthread_mutex_lock(&appointment_queue.mutex);
                if (appointment_queue.head) {
                    p = appointment_queue.head;
                    appointment_queue.head = p->next;
                    appointment_queue.count--;
                }
                safe_pthread_mutex_unlock(&appointment_queue.mutex);
            }
        }
        
        if (!p) {
            // Counter was decremented but no patient found (rare race with vital_monitor removing patients)
            // Just continue to next iteration
            continue;
        }
        
        safe_pthread_mutex_lock(&treatment_mutex);
        active_treatments++;
        safe_pthread_mutex_unlock(&treatment_mutex);
        
        // --- Process Patient ---
        log_event(INFO, "TRIAGE", "TREATMENT_START", p->id);
        
        // Calculate Wait Time
        int current_time = get_simulation_time();
        int wait_time = 0;
        
        if (p->type == PATIENT_TYPE_APPOINTMENT) {
            wait_time = diff_time_units(p->scheduled_time, current_time);
            if (wait_time < 0) wait_time = 0;
        } else {
            wait_time = diff_time_units(p->arrival_time, current_time);
        }

        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        if (p->type == PATIENT_TYPE_EMERGENCY) {
            shm_hospital->shm_stats->total_emergency_wait_time += (double)wait_time;
        } else {
            shm_hospital->shm_stats->total_appointment_wait_time += (double)wait_time;
        }
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
        
        int duration = (p->type == PATIENT_TYPE_EMERGENCY) ? 
                       config->triage_emergency_duration : config->triage_appointment_duration;
        
        wait_time_units(duration);
        
        // Update triage usage time statistics
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        shm_hospital->shm_stats->total_triage_usage_time += (double)duration;
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
        
        // Check if patient needs meds or labs
        int needs_meds = (p->meds_count > 0);
        int needs_labs = (p->tests_count > 0);
        
        if (needs_meds || needs_labs) {
            // Get unique operation ID for this patient
            int operation_id = get_next_pending_op_id();
            
            // Determine message priority based on patient status
            long msg_priority = determine_patient_mtype(p->priority, p->stability);
            
            // Send async requests
            if (needs_meds) {
                msg_pharmacy_request_t req;
                memset(&req, 0, sizeof(msg_pharmacy_request_t));
                req.hdr.mtype = msg_priority;
                req.hdr.kind = MSG_PHARMACY_REQUEST;
                strcpy(req.hdr.patient_id, p->id);
                req.hdr.operation_id = operation_id;
                req.hdr.timestamp = time(NULL);
                req.sender = SENT_BY_TRIAGE;
                req.meds_count = p->meds_count;
                memcpy(req.meds_id, p->meds_id, sizeof(req.meds_id));
                for(int i = 0; i < 8; i++) req.meds_qty[i] = 1;
                
                send_generic_message(mq_pharmacy_id, &req, sizeof(msg_pharmacy_request_t));
            }
            
            if (needs_labs) {
                msg_lab_request_t req;
                memset(&req, 0, sizeof(msg_lab_request_t));
                req.hdr.mtype = msg_priority;
                req.hdr.kind = MSG_LAB_REQUEST;
                strcpy(req.hdr.patient_id, p->id);
                req.hdr.operation_id = operation_id;
                req.hdr.timestamp = time(NULL);
                req.sender = SENT_BY_TRIAGE;
                req.tests_count = p->tests_count;
                memcpy(req.tests_id, p->tests_id, sizeof(p->tests_id));
                
                send_generic_message(mq_lab_id, &req, sizeof(msg_lab_request_t));
            }
            
            // Put patient on hold and free worker
            add_to_pending(p, operation_id, needs_meds, needs_labs);
            free(p);
            
            safe_pthread_mutex_lock(&treatment_mutex);
            active_treatments--;
            safe_pthread_mutex_unlock(&treatment_mutex);
            continue;
        }
        
        // No meds or labs needed, complete immediately
        log_event(INFO, "TRIAGE", "TREATMENT_COMPLETE", p->id);
        
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        if (p->type == PATIENT_TYPE_EMERGENCY) {
            shm_hospital->shm_stats->completed_emergencies++;
        } else {
            shm_hospital->shm_stats->completed_appointments++;
        }
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
        
        free(p);
        
        safe_pthread_mutex_lock(&treatment_mutex);
        active_treatments--;
        safe_pthread_mutex_unlock(&treatment_mutex);
    }
    return NULL;
}

// --- Main Triage Process ---

void triage_main(void) {
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "PROCESS_START", "Triage process main started");
    #endif
    
    setup_child_signals();
    
    init_queue(&emergency_queue);
    init_queue(&appointment_queue);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_CREATE", "Creating emergency manager thread");
    #endif
    // Start Threads
    safe_pthread_create(&t_emergency_mgr, NULL, emergency_queue_manager, NULL);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_CREATE", "Creating appointment manager thread");
    #endif
    safe_pthread_create(&t_appointment_mgr, NULL, appointment_queue_manager, NULL);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_CREATE", "Creating vital monitor thread");
    #endif
    safe_pthread_create(&t_vital_monitor, NULL, vital_stability_monitor, NULL);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_CREATE", "Creating response dispatcher thread");
    #endif
    // Start Response Dispatcher (must be before treatment workers)
    safe_pthread_create(&t_response_dispatcher, NULL, response_dispatcher, NULL);
    
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        int *arg = malloc(sizeof(int));
        *arg = i;
        #ifdef DEBUG
            char thread_msg[64];
            snprintf(thread_msg, sizeof(thread_msg), "Creating treatment worker thread %d", i);
            log_event(DEBUG_LOG, "TRIAGE", "THREAD_CREATE", thread_msg);
        #endif
        safe_pthread_create(&t_treatments[i], NULL, treatment_worker, arg);
    }

    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_JOIN", "Waiting for emergency manager thread to join");
    #endif
    // Wait for shutdown
    safe_pthread_join(t_emergency_mgr, NULL);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_JOIN", "Waiting for appointment manager thread to join");
    #endif
    safe_pthread_join(t_appointment_mgr, NULL);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_JOIN", "Waiting for vital monitor thread to join");
    #endif
    safe_pthread_join(t_vital_monitor, NULL);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "SHUTDOWN_MSG", "Sending shutdown message to response dispatcher");
    #endif
    // Send shutdown message to response dispatcher to unblock it
    msg_header_t shutdown_msg;
    memset(&shutdown_msg, 0, sizeof(shutdown_msg));
    shutdown_msg.mtype = PRIORITY_NORMAL;
    shutdown_msg.kind = MSG_SHUTDOWN;
    shutdown_msg.timestamp = time(NULL);
    strcpy(shutdown_msg.patient_id, "TRIAGE_SHUTDOWN");
    send_generic_message(mq_responses_id, &shutdown_msg, sizeof(msg_header_t));
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "THREAD_JOIN", "Waiting for response dispatcher thread to join");
    #endif
    safe_pthread_join(t_response_dispatcher, NULL);
    
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        #ifdef DEBUG
            char join_msg[64];
            snprintf(join_msg, sizeof(join_msg), "Waiting for treatment worker thread %d to join", i);
            log_event(DEBUG_LOG, "TRIAGE", "THREAD_JOIN", join_msg);
        #endif
        safe_pthread_join(t_treatments[i], NULL);
    }
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "CLEANUP", "Cleaning up pending patients list");
    #endif
    // Cleanup pending patients list
    safe_pthread_mutex_lock(&pending_mutex);
    while (pending_patients_head) {
        PendingPatient *next = pending_patients_head->next;
        free(pending_patients_head);
        pending_patients_head = next;
    }
    safe_pthread_mutex_unlock(&pending_mutex);
    
    // Cleanup emergency queue
    safe_pthread_mutex_lock(&emergency_queue.mutex);
    while (emergency_queue.head) {
        TriagePatient *next = emergency_queue.head->next;
        free(emergency_queue.head);
        emergency_queue.head = next;
    }
    emergency_queue.count = 0;
    safe_pthread_mutex_unlock(&emergency_queue.mutex);
    
    // Cleanup appointment queue
    safe_pthread_mutex_lock(&appointment_queue.mutex);
    while (appointment_queue.head) {
        TriagePatient *next = appointment_queue.head->next;
        free(appointment_queue.head);
        appointment_queue.head = next;
    }
    appointment_queue.count = 0;
    safe_pthread_mutex_unlock(&appointment_queue.mutex);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "CHILD_CLEANUP", "Calling child_cleanup");
    #endif
    child_cleanup();

    #ifdef DEBUG
        log_event(DEBUG_LOG, "TRIAGE", "PROCESS_EXIT", "Triage process exiting");
    #endif
    exit(EXIT_SUCCESS);
}
