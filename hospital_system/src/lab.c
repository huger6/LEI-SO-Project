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

#define MAX_CONCURRENT_TESTS 20

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

// --- Worker Thread Argument Structure ---
typedef struct {
    char patient_id[20];
    int operation_id;
    int tests_count;
    int tests_id[5];
    time_t request_time;
    msg_sender_t sender;
} lab_worker_args_t;

// --- Active Workers Registry ---
static pthread_t active_workers[MAX_CONCURRENT_TESTS];
static int active_worker_count = 0;
static pthread_mutex_t workers_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Helper Functions ---

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
    
    fprintf(fp, "============================================\n");
    fprintf(fp, "       LABORATORY ANALYSIS REPORT\n");
    fprintf(fp, "============================================\n\n");
    fprintf(fp, "Patient ID:      %s\n", patient_id);
    fprintf(fp, "Request Time:    %s", ctime(&request_time));
    fprintf(fp, "Completion Time: %s", ctime(&completion_time));
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
        if (check_shutdown()) return -1;
        log_event(ERROR, "LAB", "SEM_FAIL", "Failed to acquire lab equipment");
        return -1;
    }
    
    // Check shutdown after acquiring
    if (check_shutdown()) {
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
        if (check_shutdown()) return -1;
        log_event(ERROR, "LAB", "SEM_FAIL", "PREOP: Failed to acquire LAB1");
        return -1;
    }
    
    if (check_shutdown()) {
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
    
    if (check_shutdown()) return -1;
    
    // --- Phase 2: LAB2 (Biochemistry) ---
    snprintf(log_msg, sizeof(log_msg), "PREOP Phase 2: Acquiring LAB2 for %s", patient_id);
    log_event(INFO, "LAB", "PREOP_PHASE2", log_msg);
    
    if (acquire_lab_equipment(LAB2_ID) != 0) {
        if (check_shutdown()) return -1;
        log_event(ERROR, "LAB", "SEM_FAIL", "PREOP: Failed to acquire LAB2");
        return -1;
    }
    
    if (check_shutdown()) {
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

// --- Worker Thread Function ---

/**
 * Lab Worker Thread: Processes all tests for one request
 */
static void* lab_worker_thread(void *arg) {
    lab_worker_args_t *args = (lab_worker_args_t *)arg;

    args->request_time = time(NULL); /// Correct time (from units to real time)
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Worker started for %s with %d tests (op_id: %d)",
             args->patient_id, args->tests_count, args->operation_id);
    log_event(INFO, "LAB", "WORKER_START", log_msg);
    
    int all_success = 1;
    
    // Process each test in the request
    for (int i = 0; i < args->tests_count && !check_shutdown(); i++) {
        int test_id = args->tests_id[i];
        int result;
        
        if (test_id == TEST_PREOP) {
            // Special PREOP handling (LAB1 -> LAB2)
            result = execute_preop_test(args->patient_id);
        } else {
            // Normal test (single lab)
            result = execute_normal_test(test_id, args->patient_id);
        }
        
        if (result != 0) {
            all_success = 0;
            if (check_shutdown()) break;
        }
    }
    
    time_t completion_time = time(NULL);
    
    // Update turnaround time statistics
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    shm_hospital->shm_stats->total_lab_turnaround_time += difftime(completion_time, args->request_time);
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    if (!check_shutdown()) {
        // Generate results file
        write_results_file(args->patient_id, args->tests_count, args->tests_id,
                          args->request_time, completion_time);
        
        // Send notification to surgery/triage (routed based on sender)
        send_results_notification(args->patient_id, args->operation_id, all_success, args->sender);
    }
    
    snprintf(log_msg, sizeof(log_msg), "Worker completed for %s (success: %d)",
             args->patient_id, all_success);
    log_event(INFO, "LAB", "WORKER_COMPLETE", log_msg);
    
    // Free arguments
    free(args);
    
    return NULL;
}

/**
 * Spawn a worker thread for a lab request
 */
static int spawn_worker(msg_lab_request_t *request) {
    // Allocate worker arguments
    lab_worker_args_t *args = malloc(sizeof(lab_worker_args_t));
    if (!args) {
        log_event(ERROR, "LAB", "MALLOC_FAIL", "Failed to allocate worker args");
        return -1;
    }
    
    // Copy request data to worker args
    strncpy(args->patient_id, request->hdr.patient_id, sizeof(args->patient_id) - 1);
    args->patient_id[sizeof(args->patient_id) - 1] = '\0';
    args->operation_id = request->hdr.operation_id;
    args->tests_count = request->tests_count;
    args->request_time = request->hdr.timestamp;
    args->sender = request->sender;  // Copy sender for response routing
    
    for (int i = 0; i < request->tests_count && i < 5; i++) {
        args->tests_id[i] = request->tests_id[i];
    }
    
    // Create worker thread (detached)
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
    if (safe_pthread_create(&thread, &attr, lab_worker_thread, args) != 0) {
        log_event(ERROR, "LAB", "THREAD_FAIL", "Failed to create worker thread");
        free(args);
        pthread_attr_destroy(&attr);
        return -1;
    }
    
    pthread_attr_destroy(&attr);
    
    // Track active worker (for potential cleanup)
    safe_pthread_mutex_lock(&workers_mutex);
    if (active_worker_count < MAX_CONCURRENT_TESTS) {
        active_workers[active_worker_count++] = thread;
    }
    safe_pthread_mutex_unlock(&workers_mutex);
    
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Spawned worker for %s (%d tests)",
             request->hdr.patient_id, request->tests_count);
    log_event(INFO, "LAB", "WORKER_SPAWNED", log_msg);
    
    return 0;
}

// --- Dispatcher Loop ---

/**
 * Main dispatcher loop for the Laboratory Process
 * Receives MSG_LAB_REQUEST messages and spawns worker threads
 */
static void dispatcher_loop(void) {
    msg_lab_request_t request;
    
    while (!check_shutdown()) {
        // Clear message buffer
        memset(&request, 0, sizeof(request));
        
        // Receive next lab request (blocking with priority)
        // Priority: URGENT (1) > NORMAL (3)
        int rc = receive_generic_message(mq_lab_id, &request, sizeof(request), PRIORITY_NORMAL);
        
        if (rc != 0) {
            if (check_shutdown()) break;
            if (errno == EINTR) continue;  // Interrupted by signal
            
            // Log error but continue
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "msgrcv error: %d", errno);
            log_event(WARNING, "LAB", "RECV_ERROR", log_msg);
            continue;
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
        
        // Spawn worker thread to handle this request
        if (spawn_worker(&request) != 0) {
            log_event(ERROR, "LAB", "SPAWN_FAIL", "Failed to spawn worker for request");
            // Send failure notification (using sender from request for proper routing)
            send_results_notification(request.hdr.patient_id, request.hdr.operation_id, 0, request.sender);
        }
    }
}

// --- Main Entry Point ---

void lab_main(void) {
    setup_child_signals();
    
    // Seed random number generator
    srand((unsigned int)(time(NULL) ^ getpid()));
    
    // Run the dispatcher loop
    dispatcher_loop();
    
    child_cleanup();
    exit(EXIT_SUCCESS);
}

