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

// --- Constants & Macros ---
#define MAX_TREATMENT_THREADS 3
#define PATIENT_TYPE_EMERGENCY 1
#define PATIENT_TYPE_APPOINTMENT 2

// --- Internal Structures ---

typedef struct TriagePatient {
    char id[20];
    int type; // PATIENT_TYPE_EMERGENCY or PATIENT_TYPE_APPOINTMENT
    int priority; // 1-5 (1 is highest)
    int stability;
    int arrival_time; // or scheduled_time
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

// --- Globals ---

extern volatile sig_atomic_t g_shutdown;

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
pthread_t t_treatments[MAX_TREATMENT_THREADS];

// --- Helper Functions ---

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
        if (p->arrival_time < (*curr)->arrival_time) {
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
    while (!g_shutdown) {
        if (receive_specific_message(mq_triage_id, &msg, sizeof(msg_new_emergency_t), MSG_NEW_EMERGENCY) == -1) {
            if (errno == EINTR) continue;
            log_event(ERROR, "TRIAGE", "MQ_ERROR", "Failed to receive emergency msg");
            break;
        }
        
        if (msg.hdr.kind == MSG_SHUTDOWN) {
            g_shutdown = 1;
            safe_pthread_cond_broadcast(&patient_ready_cond);
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
        p->arrival_time = (int)msg.hdr.timestamp; // Using timestamp as arrival
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
    while (!g_shutdown) {
        if (receive_specific_message(mq_triage_id, &msg, sizeof(msg_new_appointment_t), MSG_NEW_APPOINTMENT) == -1) {
            if (errno == EINTR) continue;
            break;
        }
        
        if (msg.hdr.kind == MSG_SHUTDOWN) {
            g_shutdown = 1;
            safe_pthread_cond_broadcast(&patient_ready_cond);
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
        p->stability = 100; // Default stability for appointments?
        p->arrival_time = msg.scheduled_time;
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

    while (!g_shutdown) {
        wait_time_units(1); // Sleep 1 time unit

        // 1. Check Emergency Queue
        safe_pthread_mutex_lock(&emergency_queue.mutex);
        TriagePatient *curr = emergency_queue.head;
        TriagePatient *prev = NULL;
        
        while (curr) {
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
    
    while (!g_shutdown) {
        TriagePatient *p = NULL;
        
        safe_pthread_mutex_lock(&treatment_mutex);
        while ((emergency_queue.head == NULL && appointment_queue.head == NULL) && !g_shutdown) {
            safe_pthread_cond_wait(&patient_ready_cond, &treatment_mutex);
        }
        
        if (g_shutdown) {
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
        double wait_time = difftime(time(NULL), (time_t)p->arrival_time);
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        if (p->type == PATIENT_TYPE_EMERGENCY) {
            shm_hospital->shm_stats->total_emergency_wait_time += wait_time;
        } else {
            shm_hospital->shm_stats->total_appointment_wait_time += wait_time;
        }
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
        
        int duration = (p->type == PATIENT_TYPE_EMERGENCY) ? 
                       config->triage_emergency_duration : config->triage_appointment_duration;
        
        wait_time_units(duration);
        
        // Meds
        if (p->meds_count > 0) {
            msg_pharmacy_request_t req;
            req.hdr.mtype = MSG_PHARMACY_REQUEST;
            req.hdr.kind = MSG_PHARMACY_REQUEST;
            strcpy(req.hdr.patient_id, p->id);
            req.hdr.operation_id = 1000 + thread_id; // Unique ID for response
            req.hdr.timestamp = time(NULL);
            req.meds_count = p->meds_count;
            memcpy(req.meds_id, p->meds_id, sizeof(req.meds_id));
            // Assuming qty 1 for now as not specified in patient struct
            for(int i=0; i<8; i++) req.meds_qty[i] = 1; 
            
            send_generic_message(mq_pharmacy_id, &req, sizeof(msg_pharmacy_request_t));
            
            // Wait for response
            msg_header_t resp;
            receive_specific_message(mq_responses_id, &resp, sizeof(msg_header_t), 1000 + thread_id);
        }
        
        // Labs
        if (p->tests_count > 0) {
            msg_lab_request_t req;
            req.hdr.mtype = MSG_LAB_REQUEST;
            req.hdr.kind = MSG_LAB_REQUEST;
            strcpy(req.hdr.patient_id, p->id);
            req.hdr.operation_id = 1000 + thread_id; // Unique ID for response
            req.hdr.timestamp = time(NULL);
            req.tests_count = p->tests_count;
            // Note: p->tests_id is size 3, req.tests_id is size 4. Safe to copy.
            memcpy(req.tests_id, p->tests_id, sizeof(p->tests_id));
            
            send_generic_message(mq_lab_id, &req, sizeof(msg_lab_request_t));
            
            // Wait for response
            msg_header_t resp;
            receive_specific_message(mq_responses_id, &resp, sizeof(msg_header_t), 1000 + thread_id);
        }
        
        log_event(INFO, "TRIAGE", "TREATMENT_COMPLETE", p->id);
        
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
    log_event(INFO, "TRIAGE", "STARTUP", "Triage process started");
    
    init_queue(&emergency_queue);
    init_queue(&appointment_queue);
    
    // Start Threads
    pthread_create(&t_emergency_mgr, NULL, emergency_queue_manager, NULL);
    pthread_create(&t_appointment_mgr, NULL, appointment_queue_manager, NULL);
    pthread_create(&t_vital_monitor, NULL, vital_stability_monitor, NULL);
    
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        int *arg = malloc(sizeof(int));
        *arg = i;
        pthread_create(&t_treatments[i], NULL, treatment_worker, arg);
    }
    
    // Wait for shutdown
    pthread_join(t_emergency_mgr, NULL);
    pthread_join(t_appointment_mgr, NULL);
    pthread_join(t_vital_monitor, NULL);
    for (int i = 0; i < MAX_TREATMENT_THREADS; i++) {
        pthread_join(t_treatments[i], NULL);
    }
    
    log_event(INFO, "TRIAGE", "SHUTDOWN", "Triage process finished");
    exit(EXIT_SUCCESS);
}
