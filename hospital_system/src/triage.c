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
#define RESPONSE_TIMEOUT_UNITS 50  // Safety timeout for pharmacy/lab responses

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

typedef struct {
    TriagePatient *head;
    int count;
    pthread_mutex_t mutex;
} PatientQueue;

// --- Treatment Control (Sync Registry for Dispatcher/Worker Pattern) ---

typedef struct {
    int active;             // 1 if thread is waiting for response
    int operation_id;       // Unique ID sent in the request (e.g., 1000 + thread_index)
    
    // Sync primitives
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    
    // State flags
    int meds_ok;      // 1 = Pharmacy responded
    int labs_ok;      // 1 = Lab responded
    
    // Request tracking
    int waiting_meds; // 1 if waiting for pharmacy response
    int waiting_labs; // 1 if waiting for lab response
} TreatmentControl;

// Global array (one slot per worker thread)
static TreatmentControl treatment_controls[MAX_TREATMENT_THREADS];

// --- Globals ---

// Queues
PatientQueue emergency_queue;
PatientQueue appointment_queue;

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
    
    // Wake up all treatment workers waiting on their condition variables
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        safe_pthread_mutex_lock(&treatment_controls[i].mutex);
        safe_pthread_cond_signal(&treatment_controls[i].cond);
        safe_pthread_mutex_unlock(&treatment_controls[i].mutex);
    }
}

// --- Treatment Control Functions ---

static void init_treatment_controls(void) {
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        treatment_controls[i].active = 0;
        treatment_controls[i].operation_id = 1000 + i;
        treatment_controls[i].meds_ok = 0;
        treatment_controls[i].labs_ok = 0;
        treatment_controls[i].waiting_meds = 0;
        treatment_controls[i].waiting_labs = 0;
        safe_pthread_mutex_init(&treatment_controls[i].mutex, NULL);
        safe_pthread_cond_init(&treatment_controls[i].cond, NULL);
    }
}

static void cleanup_treatment_controls(void) {
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        pthread_mutex_destroy(&treatment_controls[i].mutex);
        pthread_cond_destroy(&treatment_controls[i].cond);
    }
}

// --- Response Dispatcher Thread ---
// This is the ONLY thread that consumes messages from mq_responses_id for Triage
// It receives messages targeted at Triage treatment workers (operation_id 1000-1002)
// Uses blocking msgrcv with negative mtype to receive any message with mtype <= max_triage_opid

void *response_dispatcher(void *arg) {
    (void)arg;
    
    log_event(INFO, "TRIAGE", "DISPATCHER_START", "Response dispatcher thread started");
    
    // Buffer for receiving messages
    union {
        msg_header_t hdr;
        msg_pharm_ready_t pharm;
        msg_lab_results_t lab;
        char buffer[512];
    } msg;
    
    // Calculate the max operation_id for Triage threads
    // Using negative mtype: msgrcv(type=-N) receives first message with mtype <= N
    // Triage uses operation_ids 1000 to (1000 + MAX_TREATMENT_THREADS - 1)
    // Manager uses 2000+, so they won't be received with this filter
    long max_triage_opid = 1000 + MAX_TREATMENT_THREADS - 1;  // 1002 for 3 threads
    
    while (!check_shutdown()) {
        // Block waiting for any message with mtype <= max_triage_opid
        // This receives messages for any Triage treatment thread without busy-waiting
        memset(&msg, 0, sizeof(msg));
        int result = receive_message_up_to_type(mq_responses_id, &msg, sizeof(msg), max_triage_opid);
        
        if (result == -1) {
            // Error or signal interruption, check shutdown and retry
            if (check_shutdown()) break;
            continue;
        }
        
        // Check for shutdown message
        if (msg.hdr.kind == MSG_SHUTDOWN) {
            log_event(INFO, "TRIAGE", "DISPATCHER_SHUTDOWN", "Received shutdown notification");
            break;
        }
        
        // Extract operation_id from mtype (which was set to operation_id by sender)
        int operation_id = (int)msg.hdr.mtype;
        int thread_index = operation_id - 1000;
        
        // Validate thread index
        if (thread_index < 0 || thread_index >= MAX_TREATMENT_THREADS) {
            char err_msg[64];
            snprintf(err_msg, sizeof(err_msg), "Invalid operation_id from mtype: %d", operation_id);
            log_event(WARNING, "TRIAGE", "DISPATCHER_INVALID_OP", err_msg);
            continue;
        }
        
        TreatmentControl *ctrl = &treatment_controls[thread_index];
        
        safe_pthread_mutex_lock(&ctrl->mutex);
        
        // Only process if the thread is actively waiting
        if (!ctrl->active) {
            safe_pthread_mutex_unlock(&ctrl->mutex);
            char warn_msg[64];
            snprintf(warn_msg, sizeof(warn_msg), "Response for inactive thread %d", thread_index);
            log_event(WARNING, "TRIAGE", "DISPATCHER_STALE", warn_msg);
            continue;
        }
        
        // Update flags based on message kind
        switch (msg.hdr.kind) {
            case MSG_PHARM_READY:
                ctrl->meds_ok = 1;
                {
                    char log_msg[64];
                    snprintf(log_msg, sizeof(log_msg), "Pharmacy response for thread %d", thread_index);
                    log_event(INFO, "TRIAGE", "DISPATCHER_PHARM", log_msg);
                }
                break;
                
            case MSG_LAB_RESULTS_READY:
                ctrl->labs_ok = 1;
                {
                    char log_msg[64];
                    snprintf(log_msg, sizeof(log_msg), "Lab response for thread %d", thread_index);
                    log_event(INFO, "TRIAGE", "DISPATCHER_LAB", log_msg);
                }
                break;
                
            default:
                {
                    char log_msg[64];
                    snprintf(log_msg, sizeof(log_msg), "Unknown message kind %d for thread %d", msg.hdr.kind, thread_index);
                    log_event(WARNING, "TRIAGE", "DISPATCHER_UNKNOWN", log_msg);
                }
                break;
        }
        
        // Signal the waiting worker thread
        safe_pthread_cond_signal(&ctrl->cond);
        safe_pthread_mutex_unlock(&ctrl->mutex);
    }
    // On shutdown, wake up all waiting worker threads
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        safe_pthread_mutex_lock(&treatment_controls[i].mutex);
        safe_pthread_cond_signal(&treatment_controls[i].cond);
        safe_pthread_mutex_unlock(&treatment_controls[i].mutex);
    }
    
    log_event(INFO, "TRIAGE", "DISPATCHER_STOP", "Response dispatcher thread stopped");
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

    msg_new_emergency_t msg;
    while (!check_shutdown()) {
        if (receive_specific_message(mq_triage_id, &msg, sizeof(msg_new_emergency_t), MSG_NEW_EMERGENCY) == -1) {
            if (errno == EINTR) continue;
            log_event(ERROR, "TRIAGE", "MQ_ERROR", "Failed to receive emergency msg");
            break;
        }
        
        if (msg.hdr.kind == MSG_SHUTDOWN) {
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
        safe_pthread_cond_signal(&patient_ready_cond);
    }
    return NULL;
}

void *appointment_queue_manager(void *arg) {
    (void)arg;

    msg_new_appointment_t msg;
    while (!check_shutdown()) {
        if (receive_specific_message(mq_triage_id, &msg, sizeof(msg_new_appointment_t), MSG_NEW_APPOINTMENT) == -1) {
            if (errno == EINTR) continue;
            break;
        }
        
        if (msg.hdr.kind == MSG_SHUTDOWN) {
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
        safe_pthread_cond_signal(&patient_ready_cond);
    }
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

        // 2. Check Appointment Queue
        safe_pthread_mutex_lock(&appointment_queue.mutex);
        curr = appointment_queue.head;
        prev = NULL;
        
        while (curr) {
            curr->stability--;
            
             if (curr->stability <= 0) {
                log_event(CRITICAL, "TRIAGE", "PATIENT_DIED", curr->id);
                if (prev) prev->next = curr->next;
                else appointment_queue.head = curr->next;
                TriagePatient *to_free = curr;
                curr = curr->next;
                appointment_queue.count--;
                free(to_free);
                continue;
            }

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
                to_move->type = PATIENT_TYPE_EMERGENCY; // Convert to emergency?
                // Prompt says "moved to the top of the emergency queue".
                
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
    
    TreatmentControl *ctrl = &treatment_controls[thread_id];
    
    while (!check_shutdown()) {
        TriagePatient *p = NULL;
        
        safe_pthread_mutex_lock(&treatment_mutex);
        while ((emergency_queue.head == NULL && appointment_queue.head == NULL) && !check_shutdown()) {
            safe_pthread_cond_wait(&patient_ready_cond, &treatment_mutex);
        }
        
        if (check_shutdown()) {
            safe_pthread_mutex_unlock(&treatment_mutex);
            break;
        }
        
        // Try Emergency first
        safe_pthread_mutex_lock(&emergency_queue.mutex);
        if (emergency_queue.head) {
            p = emergency_queue.head;
            emergency_queue.head = p->next;
            emergency_queue.count--;
        }
        safe_pthread_mutex_unlock(&emergency_queue.mutex);
        
        // If no emergency, try Appointment
        if (!p) {
            safe_pthread_mutex_lock(&appointment_queue.mutex);
            if (appointment_queue.head) {
                p = appointment_queue.head;
                appointment_queue.head = p->next;
                appointment_queue.count--;
            }
            safe_pthread_mutex_unlock(&appointment_queue.mutex);
        }
        
        if (!p) {
            safe_pthread_mutex_unlock(&treatment_mutex);
            continue;
        }
        
        active_treatments++;
        safe_pthread_mutex_unlock(&treatment_mutex);
        
        // --- Process Patient ---
        log_event(INFO, "TRIAGE", "TREATMENT_START", p->id);
        
        // Calculate Wait Time
        int current_time = get_simulation_time();
        int wait_time = 0;
        
        if (p->type == PATIENT_TYPE_APPOINTMENT) {
            // For appointments, wait time is delay after scheduled time
            wait_time = diff_time_units(p->scheduled_time, current_time);
            if (wait_time < 0) wait_time = 0;
        } else {
            // For emergencies, wait time is delay after arrival
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
        
        // --- Prepare TreatmentControl for passive waiting ---
        safe_pthread_mutex_lock(&ctrl->mutex);
        ctrl->meds_ok = 0;
        ctrl->labs_ok = 0;
        ctrl->waiting_meds = (p->meds_count > 0) ? 1 : 0;
        ctrl->waiting_labs = (p->tests_count > 0) ? 1 : 0;
        ctrl->operation_id = 1000 + thread_id;
        ctrl->active = 1;
        safe_pthread_mutex_unlock(&ctrl->mutex);
        
        // --- Send async requests ---
        
        // Meds
        if (p->meds_count > 0) {
            msg_pharmacy_request_t req;
            memset(&req, 0, sizeof(msg_pharmacy_request_t));
            req.hdr.mtype = PRIORITY_NORMAL;
            req.hdr.kind = MSG_PHARMACY_REQUEST;
            strcpy(req.hdr.patient_id, p->id);
            req.hdr.operation_id = 1000 + thread_id;
            req.hdr.timestamp = time(NULL);
            req.sender = SENT_BY_TRIAGE;
            req.meds_count = p->meds_count;
            memcpy(req.meds_id, p->meds_id, sizeof(req.meds_id));
            for(int i=0; i<8; i++) req.meds_qty[i] = 1; 
            
            send_generic_message(mq_pharmacy_id, &req, sizeof(msg_pharmacy_request_t));
        }
        
        // Labs
        if (p->tests_count > 0) {
            msg_lab_request_t req;
            memset(&req, 0, sizeof(msg_lab_request_t));
            req.hdr.mtype = PRIORITY_NORMAL;
            req.hdr.kind = MSG_LAB_REQUEST;
            strcpy(req.hdr.patient_id, p->id);
            req.hdr.operation_id = 1000 + thread_id;
            req.hdr.timestamp = time(NULL);
            req.sender = SENT_BY_TRIAGE;
            req.tests_count = p->tests_count;
            memcpy(req.tests_id, p->tests_id, sizeof(p->tests_id));
            
            send_generic_message(mq_lab_id, &req, sizeof(msg_lab_request_t));
        }
        
        // --- Passive Wait using pthread_cond_timedwait ---
        int pharmacy_success = 1;
        int lab_success = 1;
        int wait_result = 0;  // 0 = success, -1 = shutdown, -2 = timeout
        
        if (ctrl->waiting_meds || ctrl->waiting_labs) {
            // Calculate absolute timeout time
            struct timespec timeout_abs;
            clock_gettime(CLOCK_REALTIME, &timeout_abs);
            
            // Add timeout: RESPONSE_TIMEOUT_UNITS * time_unit_ms milliseconds
            long timeout_ms = (long)RESPONSE_TIMEOUT_UNITS * config->time_unit_ms;
            timeout_abs.tv_sec += timeout_ms / 1000;
            timeout_abs.tv_nsec += (timeout_ms % 1000) * 1000000L;
            
            // Normalize nanoseconds
            if (timeout_abs.tv_nsec >= 1000000000L) {
                timeout_abs.tv_sec += 1;
                timeout_abs.tv_nsec -= 1000000000L;
            }
            
            safe_pthread_mutex_lock(&ctrl->mutex);
            
            // Wait until all expected responses are received, or timeout/shutdown
            while (ctrl->active && !check_shutdown()) {
                // Check if we have all responses we're waiting for
                int meds_done = !ctrl->waiting_meds || ctrl->meds_ok;
                int labs_done = !ctrl->waiting_labs || ctrl->labs_ok;
                
                if (meds_done && labs_done) {
                    break;  // All responses received
                }
                
                // Wait with timeout
                int rc = pthread_cond_timedwait(&ctrl->cond, &ctrl->mutex, &timeout_abs);
                
                if (rc == ETIMEDOUT) {
                    wait_result = -2;  // Timeout
                    break;
                }
            }
            
            // Check final state
            if (check_shutdown()) {
                wait_result = -1;  // Shutdown
            } else if (wait_result == 0) {
                // Check if we got all responses
                if (ctrl->waiting_meds && !ctrl->meds_ok) {
                    pharmacy_success = 0;
                }
                if (ctrl->waiting_labs && !ctrl->labs_ok) {
                    lab_success = 0;
                }
            } else if (wait_result == -2) {
                // Timeout - check which ones failed
                if (ctrl->waiting_meds && !ctrl->meds_ok) {
                    pharmacy_success = 0;
                    char err_msg[64];
                    snprintf(err_msg, sizeof(err_msg), "Pharmacy timeout for patient %s", p->id);
                    log_event(ERROR, "TRIAGE", "PHARMACY_TIMEOUT", err_msg);
                }
                if (ctrl->waiting_labs && !ctrl->labs_ok) {
                    lab_success = 0;
                    char err_msg[64];
                    snprintf(err_msg, sizeof(err_msg), "Lab timeout for patient %s", p->id);
                    log_event(ERROR, "TRIAGE", "LAB_TIMEOUT", err_msg);
                }
            }
            
            // Cleanup: deactivate
            ctrl->active = 0;
            safe_pthread_mutex_unlock(&ctrl->mutex);
        }
        
        // Handle shutdown case
        if (wait_result == -1) {
            log_event(WARNING, "TRIAGE", "SHUTDOWN_ABORT", p->id);
            free(p);
            safe_pthread_mutex_lock(&treatment_mutex);
            active_treatments--;
            safe_pthread_mutex_unlock(&treatment_mutex);
            break;
        }
        
        // Log treatment completion with status
        if (pharmacy_success && lab_success) {
            log_event(INFO, "TRIAGE", "TREATMENT_COMPLETE", p->id);
        } else {
            char status_msg[128];
            snprintf(status_msg, sizeof(status_msg), "%s (meds:%s, tests:%s)", 
                     p->id, pharmacy_success ? "OK" : "FAILED", lab_success ? "OK" : "FAILED");
            log_event(WARNING, "TRIAGE", "TREATMENT_PARTIAL", status_msg);
        }
        
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        if (p->type == PATIENT_TYPE_EMERGENCY) shm_hospital->shm_stats->completed_emergencies++;
        else shm_hospital->shm_stats->completed_appointments++;
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
    log_event(INFO, "TRIAGE", "STARTUP", "Booting triage process");

    setup_child_signals();

    close_unused_pipe_ends(ROLE_TRIAGE);
    
    init_queue(&emergency_queue);
    init_queue(&appointment_queue);
    
    // Initialize TreatmentControl sync registry
    init_treatment_controls();
    
    // Start Threads
    safe_pthread_create(&t_emergency_mgr, NULL, emergency_queue_manager, NULL);
    safe_pthread_create(&t_appointment_mgr, NULL, appointment_queue_manager, NULL);
    safe_pthread_create(&t_vital_monitor, NULL, vital_stability_monitor, NULL);
    
    // Start Response Dispatcher (must be before treatment workers)
    safe_pthread_create(&t_response_dispatcher, NULL, response_dispatcher, NULL);
    
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        int *arg = malloc(sizeof(int));
        *arg = i;
        safe_pthread_create(&t_treatments[i], NULL, treatment_worker, arg);
    }

    // Wait for shutdown
    safe_pthread_join(t_emergency_mgr, NULL);
    safe_pthread_join(t_appointment_mgr, NULL);
    safe_pthread_join(t_vital_monitor, NULL);
    
    // Send shutdown message to response dispatcher to unblock it
    msg_header_t shutdown_msg;
    memset(&shutdown_msg, 0, sizeof(shutdown_msg));
    shutdown_msg.mtype = PRIORITY_NORMAL;
    shutdown_msg.kind = MSG_SHUTDOWN;
    shutdown_msg.timestamp = time(NULL);
    strcpy(shutdown_msg.patient_id, "TRIAGE_SHUTDOWN");
    send_generic_message(mq_responses_id, &shutdown_msg, sizeof(msg_header_t));
    
    safe_pthread_join(t_response_dispatcher, NULL);
    
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        safe_pthread_join(t_treatments[i], NULL);
    }
    
    // Cleanup TreatmentControl resources
    cleanup_treatment_controls();
    
    log_event(INFO, "TRIAGE", "SHUTDOWN", "Triage process shutting down");

    // Resources cleanup
    log_event(INFO, "TRIAGE", "RESOURCES_CLEANUP", "Cleaning triage resources");
    child_cleanup();

    exit(EXIT_SUCCESS);
}
