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

// --- Constants ---
#define LAB1_ID         1   // Hematology
#define LAB2_ID         2   // Biochemistry

// Test IDs (must match console_input.c get_test_id)
#define TEST_HEMO       0
#define TEST_GLIC       1
#define TEST_COLEST     2
#define TEST_RENAL      3
#define TEST_HEPAT      4
#define TEST_PREOP      5

// Thread Pool Configuration
#define LAB_POOL_SIZE   5

// ============================================================================
// JOB QUEUE IMPLEMENTATION (Thread-Safe Producer-Consumer Queue)
// ============================================================================

/**
 * Job structure - contains all data needed to process a lab request
 */
typedef struct lab_job {
    char patient_id[20];
    int operation_id;
    int tests_count;
    int tests_id[5];
    time_t request_time;
    msg_sender_t sender;
    struct lab_job *next;
} lab_job_t;

/**
 * Thread-safe job queue with condition variable for blocking
 */
typedef struct {
    lab_job_t *head;
    lab_job_t *tail;
    int count;
    int shutdown;               // Flag to signal workers to exit
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} job_queue_t;

// Global job queue instance
static job_queue_t job_queue;

/**
 * Check if the lab should shutdown (thread-safe)
 * @return 1 if shutdown is requested, 0 otherwise
 */
static int lab_should_shutdown(void) {
    int val;
    safe_pthread_mutex_lock(&job_queue.mutex);
    val = job_queue.shutdown;
    safe_pthread_mutex_unlock(&job_queue.mutex);
    return val;
}

/**
 * Initialize the job queue
 */
static void job_queue_init(job_queue_t *q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    q->shutdown = 0;
    safe_pthread_mutex_init(&q->mutex, NULL);
    safe_pthread_cond_init(&q->cond, NULL);
}

/**
 * Destroy the job queue and free any remaining jobs
 */
static void job_queue_destroy(job_queue_t *q) {
    safe_pthread_mutex_lock(&q->mutex);
    
    // Free any remaining jobs
    lab_job_t *current = q->head;
    while (current != NULL) {
        lab_job_t *next = current->next;
        free(current);
        current = next;
    }
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    
    safe_pthread_mutex_unlock(&q->mutex);
    
    safe_pthread_mutex_destroy(&q->mutex);
    safe_pthread_cond_destroy(&q->cond);
}

/**
 * Push a job onto the queue (producer side)
 * @return 0 on success, -1 on failure
 */
static int job_queue_push(job_queue_t *q, const msg_lab_request_t *request) {
    lab_job_t *job = malloc(sizeof(lab_job_t));
    if (!job) {
        log_event(ERROR, "LAB", "MALLOC_FAIL", "Failed to allocate job");
        return -1;
    }
    
    // Copy request data into job
    strncpy(job->patient_id, request->hdr.patient_id, sizeof(job->patient_id) - 1);
    job->patient_id[sizeof(job->patient_id) - 1] = '\0';
    job->operation_id = request->hdr.operation_id;
    job->tests_count = request->tests_count;
    job->request_time = request->hdr.timestamp;
    job->sender = request->sender;
    
    for (int i = 0; i < request->tests_count && i < 5; i++) {
        job->tests_id[i] = request->tests_id[i];
    }
    job->next = NULL;
    
    safe_pthread_mutex_lock(&q->mutex);
    
    // Add to tail of queue
    if (q->tail == NULL) {
        q->head = job;
        q->tail = job;
    } else {
        q->tail->next = job;
        q->tail = job;
    }
    q->count++;
    
    // Signal one waiting worker
    safe_pthread_cond_signal(&q->cond);
    
    safe_pthread_mutex_unlock(&q->mutex);
    
    return 0;
}

/**
 * Pop a job from the queue (consumer/worker side)
 * Blocks until a job is available or shutdown is signaled
 * @return pointer to job on success, NULL if shutdown signaled
 */
static lab_job_t* job_queue_pop(job_queue_t *q) {
    safe_pthread_mutex_lock(&q->mutex);
    
    // Wait for a job or shutdown signal
    // Use regular cond_wait - shutdown will broadcast to wake all waiters
    while (q->head == NULL && !q->shutdown) {
        safe_pthread_cond_wait(&q->cond, &q->mutex);
    }
    
    // If shutdown is signaled, exit immediately (don't process remaining jobs)
    if (q->shutdown) {
        safe_pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    
    // Pop from head of queue
    lab_job_t *job = q->head;
    q->head = job->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    q->count--;
    
    safe_pthread_mutex_unlock(&q->mutex);
    
    return job;
}

/**
 * Signal all workers to shutdown
 */
static void job_queue_shutdown(job_queue_t *q) {
    safe_pthread_mutex_lock(&q->mutex);
    q->shutdown = 1;
    // Wake up ALL waiting workers so they can exit
    safe_pthread_cond_broadcast(&q->cond);
    safe_pthread_mutex_unlock(&q->mutex);
}

// ============================================================================
// LAB EQUIPMENT SEMAPHORE FUNCTIONS
// ============================================================================

/**
 * Acquires a slot for a specific laboratory
 * Blocks if the laboratory is at maximum capacity
 * @param lab_id The ID of the laboratory (1 for Hematology, 2 for Biochemistry)
 * @return 0 on success, -1 on failure
 */
int acquire_lab_equipment(int lab_id) {
    sem_t *target_sem = NULL;
    const char *sem_name = NULL;

    switch (lab_id) {
        case LAB1_ID:
            target_sem = sem_lab1;
            sem_name = "LAB1_EQUIPMENT";
            break;
        case LAB2_ID:
            target_sem = sem_lab2;
            sem_name = "LAB2_EQUIPMENT";
            break;
        default: {
            char log_buffer[128];
            snprintf(log_buffer, sizeof(log_buffer), "acquire_lab_equipment: Invalid lab_id %d", lab_id);
            log_event(ERROR, "SEMAPHORE", "LAB_ACQUIRE_FAIL", log_buffer);
            return -1;
        }
    }

    return sem_wait_safe(target_sem, sem_name);
}

/**
 * Releases a laboratory slot after an analysis is completed
 * @param lab_id The ID of the laboratory (1 for Hematology, 2 for Biochemistry)
 * @return 0 on success, -1 on failure
 */
int release_lab_equipment(int lab_id) {
    sem_t *target_sem = NULL;
    const char *sem_name = NULL;

    switch (lab_id) {
        case LAB1_ID:
            target_sem = sem_lab1;
            sem_name = "LAB1_EQUIPMENT";
            break;
        case LAB2_ID:
            target_sem = sem_lab2;
            sem_name = "LAB2_EQUIPMENT";
            break;
        default: {
            char log_buffer[128];
            snprintf(log_buffer, sizeof(log_buffer), "release_lab_equipment: Invalid lab_id %d", lab_id);
            log_event(ERROR, "SEMAPHORE", "LAB_RELEASE_FAIL", log_buffer);
            return -1;
        }
    }

    return sem_post_safe(target_sem, sem_name);
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static const char* get_test_name(int test_id) {
    switch (test_id) {
        case TEST_HEMO:   return "HEMO";
        case TEST_GLIC:   return "GLIC";
        case TEST_COLEST: return "COLEST";
        case TEST_RENAL:  return "RENAL";
        case TEST_HEPAT:  return "HEPAT";
        case TEST_PREOP:  return "PREOP";
        default:          return "UNKNOWN";
    }
}

/**
 * Determine which lab handles a specific test
 * Returns: LAB1_ID, LAB2_ID, or 0 for PREOP (special handling)
 */
static int get_target_lab(int test_id) {
    switch (test_id) {
        case TEST_HEMO:
        case TEST_GLIC:
            return LAB1_ID;  // Hematology
        case TEST_COLEST:
        case TEST_RENAL:
        case TEST_HEPAT:
            return LAB2_ID;  // Biochemistry
        case TEST_PREOP:
            return 0;        // Special: requires both labs in sequence
        default:
            return -1;       // Invalid
    }
}

static int get_lab1_duration(void) {
    int min_dur = config->lab1_min_duration;
    int max_dur = config->lab1_max_duration;
    if (max_dur <= min_dur) return min_dur;
    return min_dur + (rand() % (max_dur - min_dur + 1));
}

static int get_lab2_duration(void) {
    int min_dur = config->lab2_min_duration;
    int max_dur = config->lab2_max_duration;
    if (max_dur <= min_dur) return min_dur;
    return min_dur + (rand() % (max_dur - min_dur + 1));
}

/**
 * Get duration for PREOP test (split between both labs)
 * Returns total duration (half in LAB1, half in LAB2)
 */
static int get_preop_duration(void) {
    // PREOP: 20-40 units total
    int min_dur = 20;
    int max_dur = 40;
    return min_dur + (rand() % (max_dur - min_dur + 1));
}

static float random_float(float min, float max) {
    return min + ((float)rand() / RAND_MAX) * (max - min);
}

static void generate_test_result(int test_id, char *result_buf, size_t buf_size) {
    switch (test_id) {
        case TEST_HEMO:
            snprintf(result_buf, buf_size, 
                "Hemoglobin: %.1f g/dL, RBC: %.2f M/uL, WBC: %.1f K/uL, Platelets: %d K/uL",
                random_float(12.0, 17.0),
                random_float(4.0, 6.0),
                random_float(4.0, 11.0),
                (int)(150 + rand() % 250));
            break;
        case TEST_GLIC:
            snprintf(result_buf, buf_size,
                "Fasting Glucose: %d mg/dL, HbA1c: %.1f%%",
                70 + rand() % 60,
                random_float(4.0, 6.5));
            break;
        case TEST_COLEST:
            snprintf(result_buf, buf_size,
                "Total Cholesterol: %d mg/dL, LDL: %d mg/dL, HDL: %d mg/dL, Triglycerides: %d mg/dL",
                150 + rand() % 100,
                70 + rand() % 80,
                40 + rand() % 40,
                50 + rand() % 150);
            break;
        case TEST_RENAL:
            snprintf(result_buf, buf_size,
                "Creatinine: %.1f mg/dL, BUN: %d mg/dL, eGFR: %d mL/min",
                random_float(0.6, 1.3),
                8 + rand() % 15,
                60 + rand() % 60);
            break;
        case TEST_HEPAT:
            snprintf(result_buf, buf_size,
                "ALT: %d U/L, AST: %d U/L, Bilirubin: %.1f mg/dL, Albumin: %.1f g/dL",
                10 + rand() % 40,
                10 + rand() % 35,
                random_float(0.2, 1.2),
                random_float(3.5, 5.0));
            break;
        case TEST_PREOP:
            snprintf(result_buf, buf_size,
                "Coagulation PT: %.1f sec, INR: %.1f, CBC: Normal, Metabolic Panel: Normal, Clearance: APPROVED",
                random_float(11.0, 14.0),
                random_float(0.9, 1.2));
            break;
        default:
            snprintf(result_buf, buf_size, "Result: N/A");
            break;
    }
}

static int write_results_file(const char *patient_id, int tests_count, int *tests_id, time_t request_time, time_t completion_time) {
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "results/lab_results/%s_%ld.txt", 
             patient_id, (long)completion_time);
    
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Failed to create results file: %s", filepath);
        log_event(ERROR, "LAB", "FILE_ERROR", log_msg);
        return -1;
    }
    
    // Use thread-safe ctime_r() instead of ctime() to avoid race condition
    // on the static buffer used by ctime()/asctime()
    char request_time_str[26];
    char completion_time_str[26];
    ctime_r(&request_time, request_time_str);
    ctime_r(&completion_time, completion_time_str);
    
    fprintf(fp, "============================================\n");
    fprintf(fp, "       LABORATORY ANALYSIS REPORT\n");
    fprintf(fp, "============================================\n\n");
    fprintf(fp, "Patient ID:      %s\n", patient_id);
    fprintf(fp, "Request Time:    %s", request_time_str);
    fprintf(fp, "Completion Time: %s", completion_time_str);
    fprintf(fp, "Tests Performed: %d\n\n", tests_count);
    fprintf(fp, "--------------------------------------------\n");
    fprintf(fp, "                 RESULTS\n");
    fprintf(fp, "--------------------------------------------\n\n");
    
    for (int i = 0; i < tests_count; i++) {
        char result_buf[256];
        generate_test_result(tests_id[i], result_buf, sizeof(result_buf));
        fprintf(fp, "[%s]\n", get_test_name(tests_id[i]));
        fprintf(fp, "  %s\n\n", result_buf);
    }
    
    fprintf(fp, "--------------------------------------------\n");
    fprintf(fp, "Report generated by Hospital Lab System\n");
    fprintf(fp, "============================================\n");
    
    fclose(fp);
    
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Results file created: %s", filepath);
    log_event(INFO, "LAB", "RESULTS_FILE", log_msg);
    
    return 0;
}

/**
 * Execute a single normal test (LAB1 or LAB2)
 */
static int execute_normal_test(int test_id, const char *patient_id) {
    int lab_id = get_target_lab(test_id);
    if (lab_id < 1) {
        return -1;  // Invalid or PREOP (handled separately)
    }
    
    const char *lab_name = (lab_id == LAB1_ID) ? "LAB1" : "LAB2";
    int duration = (lab_id == LAB1_ID) ? get_lab1_duration() : get_lab2_duration();
    
    // Log start
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "%s: Starting %s test for %s (duration: %d units)",
             lab_name, get_test_name(test_id), patient_id, duration);
    log_event(INFO, "LAB", "TEST_START", log_msg);
    
    // Acquire lab equipment (semaphore)
    if (acquire_lab_equipment(lab_id) != 0) {
        if (lab_should_shutdown()) return -1;
        log_event(ERROR, "LAB", "SEM_FAIL", "Failed to acquire lab equipment");
        return -1;
    }
    
    // Check shutdown after acquiring
    if (lab_should_shutdown()) {
        release_lab_equipment(lab_id);
        return -1;
    }
    
    // Update statistics
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    if (lab_id == LAB1_ID) {
        shm_hospital->shm_stats->total_lab_tests_lab1++;
    } else {
        shm_hospital->shm_stats->total_lab_tests_lab2++;
    }
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    // Simulate test duration
    int start_time = get_simulation_time();
    wait_time_units(duration);
    int end_time = get_simulation_time();
    
    // Update lab time statistics
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    if (lab_id == LAB1_ID) {
        shm_hospital->shm_stats->total_lab1_time += (end_time - start_time);
    } else {
        shm_hospital->shm_stats->total_lab2_time += (end_time - start_time);
    }
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    // Release lab equipment
    release_lab_equipment(lab_id);
    
    // Log completion
    snprintf(log_msg, sizeof(log_msg), "%s: Completed %s test for %s",
             lab_name, get_test_name(test_id), patient_id);
    log_event(INFO, "LAB", "TEST_COMPLETE", log_msg);
    
    return 0;
}

/**
 * Execute PREOP test (requires LAB1 then LAB2 in strict sequence)
 */
static int execute_preop_test(const char *patient_id) {
    int total_duration = get_preop_duration();
    int phase1_duration = total_duration / 2;
    int phase2_duration = total_duration - phase1_duration;
    
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "PREOP: Starting for %s (total duration: %d units)",
             patient_id, total_duration);
    log_event(INFO, "LAB", "PREOP_START", log_msg);
    
    // --- Phase 1: LAB1 (Hematology) ---
    snprintf(log_msg, sizeof(log_msg), "PREOP Phase 1: Acquiring LAB1 for %s", patient_id);
    log_event(INFO, "LAB", "PREOP_PHASE1", log_msg);
    
    if (acquire_lab_equipment(LAB1_ID) != 0) {
        if (lab_should_shutdown()) return -1;
        log_event(ERROR, "LAB", "SEM_FAIL", "PREOP: Failed to acquire LAB1");
        return -1;
    }
    
    if (lab_should_shutdown()) {
        release_lab_equipment(LAB1_ID);
        return -1;
    }
    
    // Update LAB1 stats
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    shm_hospital->shm_stats->total_lab_tests_lab1++;
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    // Simulate Phase 1
    int phase1_start = get_simulation_time();
    wait_time_units(phase1_duration);
    int phase1_end = get_simulation_time();
    
    // Update LAB1 time
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    shm_hospital->shm_stats->total_lab1_time += (phase1_end - phase1_start);
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    // Release LAB1 before acquiring LAB2
    release_lab_equipment(LAB1_ID);
    
    if (lab_should_shutdown()) return -1;
    
    // --- Phase 2: LAB2 (Biochemistry) ---
    snprintf(log_msg, sizeof(log_msg), "PREOP Phase 2: Acquiring LAB2 for %s", patient_id);
    log_event(INFO, "LAB", "PREOP_PHASE2", log_msg);
    
    if (acquire_lab_equipment(LAB2_ID) != 0) {
        if (lab_should_shutdown()) return -1;
        log_event(ERROR, "LAB", "SEM_FAIL", "PREOP: Failed to acquire LAB2");
        return -1;
    }
    
    if (lab_should_shutdown()) {
        release_lab_equipment(LAB2_ID);
        return -1;
    }
    
    // Update LAB2 stats
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    shm_hospital->shm_stats->total_lab_tests_lab2++;
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    // Simulate Phase 2
    int phase2_start = get_simulation_time();
    wait_time_units(phase2_duration);
    int phase2_end = get_simulation_time();
    
    // Update LAB2 time
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    shm_hospital->shm_stats->total_lab2_time += (phase2_end - phase2_start);
    shm_hospital->shm_stats->total_preop_tests++;
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    // Release LAB2
    release_lab_equipment(LAB2_ID);
    
    // Log completion
    snprintf(log_msg, sizeof(log_msg), "PREOP: Completed for %s", patient_id);
    log_event(INFO, "LAB", "PREOP_COMPLETE", log_msg);
    
    return 0;
}

/**
 * Send results notification back to requester
 * Routes to correct queue based on sender
 * Uses operation_id as mtype so receivers can filter for their specific messages
 */
static int send_results_notification(const char *patient_id, int operation_id, int success, msg_sender_t sender) {
    msg_lab_results_t response;
    memset(&response, 0, sizeof(response));
    
    // --- 1. Preencher o Conteúdo (Payload) ---
    // Isto é o que vai DENTRO do envelope. O destinatário vai ler isto.
    response.hdr.kind = MSG_LAB_RESULTS_READY;
    strncpy(response.hdr.patient_id, patient_id, sizeof(response.hdr.patient_id) - 1);
    
    response.hdr.operation_id = operation_id; 
    
    response.hdr.timestamp = time(NULL);
    response.results_code = success ? 0 : -1;
    
    // --- 2. Where to send ---
    int target_queue;
    const char *target_name;

    if (operation_id > 0) response.hdr.mtype = operation_id;
    else response.hdr.mtype = PRIORITY_NORMAL; // dummy
    
    switch (sender) {
        case SENT_BY_SURGERY:
            target_queue = mq_surgery_id;
            target_name = "Surgery";
            
            break;

        case SENT_BY_TRIAGE:
            target_queue = mq_responses_id;
            target_name = "Triage (responses)";
            
            break;

        case SENT_BY_MANAGER:
            target_queue = mq_responses_id;
            target_name = "Manager (responses)";

            response.hdr.mtype = 2001;
            break;

        default:
            target_queue = mq_responses_id;
            target_name = "Unknown (responses)";
            log_event(WARNING, "LAB", "UNKNOWN_SENDER", "Response for unknown sender, routing to responses queue");
            break;
    }
    
    // --- 3. Send ---
    if (send_generic_message(target_queue, &response, sizeof(response)) != 0) {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Failed to send results notification for %s to %s", patient_id, target_name);
        log_event(ERROR, "LAB", "MSG_SEND_FAIL", log_msg);
        return -1;
    }
    
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Results notification sent for %s (op_id: %d, success: %d) to %s",
             patient_id, operation_id, success, target_name);
    log_event(INFO, "LAB", "RESULTS_SENT", log_msg);
    
    return 0;
}

// ============================================================================
// THREAD POOL WORKER FUNCTION
// ============================================================================

/**
 * Worker thread argument (just the worker ID for logging)
 */
typedef struct {
    int worker_id;
} worker_thread_args_t;

/**
 * Process a single job (patient's lab tests)
 * This is called by a worker thread for each job it picks up
 */
static void process_job(lab_job_t *job, int worker_id) {
    time_t actual_request_time = time(NULL);  // Real time for turnaround calculation
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Worker %d: Processing request for %s with %d tests (op_id: %d)",
             worker_id, job->patient_id, job->tests_count, job->operation_id);
    log_event(INFO, "LAB", "JOB_START", log_msg);
    
    int all_success = 1;
    
    // Process each test in the request
    for (int i = 0; i < job->tests_count && !lab_should_shutdown(); i++) {
        int test_id = job->tests_id[i];
        int result;
        
        if (test_id == TEST_PREOP) {
            // Special PREOP handling (LAB1 -> LAB2)
            result = execute_preop_test(job->patient_id);
        } else {
            // Normal test (single lab)
            result = execute_normal_test(test_id, job->patient_id);
        }
        
        if (result != 0) {
            all_success = 0;
            if (lab_should_shutdown()) break;
        }
    }
    
    time_t completion_time = time(NULL);
    
    // Update turnaround time statistics
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    shm_hospital->shm_stats->total_lab_turnaround_time += difftime(completion_time, actual_request_time);
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    if (!lab_should_shutdown()) {
        // Generate results file
        write_results_file(job->patient_id, job->tests_count, job->tests_id,
                          actual_request_time, completion_time);
        
        // Send notification to surgery/triage (routed based on sender)
        send_results_notification(job->patient_id, job->operation_id, all_success, job->sender);
    }
    
    snprintf(log_msg, sizeof(log_msg), "Worker %d: Completed request for %s (success: %d)",
             worker_id, job->patient_id, all_success);
    log_event(INFO, "LAB", "JOB_COMPLETE", log_msg);
}

/**
 * Thread Pool Worker Function
 * Runs in a loop taking jobs from the queue until shutdown is signaled
 */
static void* pool_worker_thread(void *arg) {
    worker_thread_args_t *wargs = (worker_thread_args_t *)arg;
    int worker_id = wargs->worker_id;
    free(wargs);  // Free the argument struct immediately
    
    char log_msg[64];
    snprintf(log_msg, sizeof(log_msg), "Worker %d started", worker_id);
    log_event(INFO, "LAB", "WORKER_START", log_msg);
    
    while (1) {
        // Pop a job from the queue (blocks until job available or shutdown)
        lab_job_t *job = job_queue_pop(&job_queue);
        
        if (job == NULL) {
            // Shutdown signaled - exit cleanly
            break;
        }
        
        // Process the job
        process_job(job, worker_id);
        
        // Free the job
        free(job);
    }
    
    snprintf(log_msg, sizeof(log_msg), "Worker %d exiting", worker_id);
    log_event(INFO, "LAB", "WORKER_EXIT", log_msg);
    
    return NULL;
}

// ============================================================================
// DISPATCHER LOOP
// ============================================================================

/**
 * Main dispatcher loop for the Laboratory Process
 * Receives MSG_LAB_REQUEST messages and pushes them to the job queue
 * Exits when MSG_SHUTDOWN (poison pill) is received
 */
static void dispatcher_loop(void) {
    msg_lab_request_t request;
    
    while (1) {
        // Clear message buffer
        memset(&request, 0, sizeof(request));
        
        // Receive next lab request (blocking with priority)
        // Priority: URGENT (1) > NORMAL (3)
        int rc = receive_generic_message(mq_lab_id, &request, sizeof(request), PRIORITY_NORMAL);
        
        if (rc != 0) {
            if (errno == EINTR) continue;  // Interrupted by signal
            
            // Log error but continue
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "msgrcv error: %d", errno);
            log_event(WARNING, "LAB", "RECV_ERROR", log_msg);
            continue;
        }
        
        // Check for shutdown message (poison pill from manager)
        if (request.hdr.kind == MSG_SHUTDOWN) {
            log_event(INFO, "LAB", "SHUTDOWN_RECV", "Received shutdown signal");
            break;
        }
        
        // Validate message type
        if (request.hdr.kind != MSG_LAB_REQUEST) {
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "Unexpected message kind: %d", request.hdr.kind);
            log_event(WARNING, "LAB", "INVALID_MSG", log_msg);
            continue;
        }
        
        // Update statistics - track urgent lab tests
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        if (request.hdr.mtype == PRIORITY_URGENT) {
            shm_hospital->shm_stats->urgent_lab_tests++;
        }
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
        
        // Log received request
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Received lab request for %s (%d tests, op_id: %d)",
                 request.hdr.patient_id, request.tests_count, request.hdr.operation_id);
        log_event(INFO, "LAB", "REQUEST_RECV", log_msg);
        
        // Push to job queue for worker threads to process
        if (job_queue_push(&job_queue, &request) != 0) {
            log_event(ERROR, "LAB", "QUEUE_FAIL", "Failed to enqueue job");
            // Send failure notification
            send_results_notification(request.hdr.patient_id, request.hdr.operation_id, 0, request.sender);
        }
    }
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

void lab_main(void) {
    #ifdef DEBUG
        log_event(DEBUG_LOG, "LAB", "PROCESS_START", "Lab process main started");
    #endif
    
    setup_child_signals();
    
    // Seed random number generator
    srand((unsigned int)(time(NULL) ^ getpid()));
    
    // Initialize the job queue
    job_queue_init(&job_queue);
    
    // Create the fixed thread pool
    pthread_t workers[LAB_POOL_SIZE];
    int workers_created = 0;
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "LAB", "POOL_CREATE", "Creating thread pool");
    #endif
    
    for (int i = 0; i < LAB_POOL_SIZE; i++) {
        worker_thread_args_t *wargs = malloc(sizeof(worker_thread_args_t));
        if (!wargs) {
            log_event(ERROR, "LAB", "MALLOC_FAIL", "Failed to allocate worker args");
            continue;
        }
        wargs->worker_id = i + 1;
        
        if (safe_pthread_create(&workers[i], NULL, pool_worker_thread, wargs) != 0) {
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "Failed to create worker thread %d", i + 1);
            log_event(ERROR, "LAB", "THREAD_FAIL", log_msg);
            free(wargs);
            continue;
        }
        workers_created++;
    }
    
    char log_msg[64];
    snprintf(log_msg, sizeof(log_msg), "Created %d/%d worker threads", workers_created, LAB_POOL_SIZE);
    log_event(INFO, "LAB", "POOL_READY", log_msg);
    
    if (workers_created == 0) {
        log_event(ERROR, "LAB", "POOL_FAIL", "No worker threads created, exiting");
        job_queue_destroy(&job_queue);
        child_cleanup();
        exit(EXIT_FAILURE);
    }
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "LAB", "DISPATCHER_START", "Starting lab dispatcher loop");
    #endif
    
    // Run the dispatcher loop (this blocks until shutdown)
    dispatcher_loop();
    
    // --- Shutdown Sequence ---
    #ifdef DEBUG
        log_event(DEBUG_LOG, "LAB", "SHUTDOWN_START", "Starting shutdown sequence");
    #endif
    
    // 1. Signal all workers to shutdown
    job_queue_shutdown(&job_queue);
    
    // 2. Wait for all worker threads to join
    #ifdef DEBUG
        log_event(DEBUG_LOG, "LAB", "WAIT_WORKERS", "Waiting for worker threads to join");
    #endif
    
    for (int i = 0; i < LAB_POOL_SIZE; i++) {
        // Only join if thread was successfully created
        // (pthread_t is initialized to 0 by array initialization)
        if (workers[i] != 0) {
            safe_pthread_join(workers[i], NULL);
        }
    }
    
    log_event(INFO, "LAB", "WORKERS_JOINED", "All worker threads joined");
    
    // 3. Cleanup the job queue
    job_queue_destroy(&job_queue);
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "LAB", "CHILD_CLEANUP", "Calling child_cleanup");
    #endif
    child_cleanup();
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "LAB", "PROCESS_EXIT", "Lab process exiting");
    #endif
    exit(EXIT_SUCCESS);
}

