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
#define MAX_CONCURRENT_REQUESTS 20

// --- Worker Thread Argument Structure ---
typedef struct {
    char patient_id[20];
    int operation_id;
    int meds_count;
    int meds_id[8];
    int meds_qty[8];
    time_t request_time;
    msg_sender_t sender;
    int priority;
} pharmacy_worker_args_t;

// --- Active Workers Registry ---
static pthread_t active_workers[MAX_CONCURRENT_REQUESTS];
static int active_worker_count = 0;
static pthread_mutex_t workers_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Helper Functions ---

/**
 * Get medication name by ID from config
 */
static const char* get_med_name(int med_id) {
    if (med_id >= 0 && med_id < config->med_count) {
        return config->medications[med_id].name;
    }
    return "UNKNOWN";
}

/**
 * Get random preparation time within configured range
 */
static int get_prep_duration(void) {
    int min_dur = config->pharmacy_prep_time_min;
    int max_dur = config->pharmacy_prep_time_max;
    if (max_dur <= min_dur) return min_dur;
    return min_dur + (rand() % (max_dur - min_dur + 1));
}

/**
 * Acquire pharmacy access semaphore
 */
static int acquire_pharmacy_access(void) {
    return sem_wait_safe(sem_pharmacy, "PHARMACY_ACCESS");
}

/**
 * Release pharmacy access semaphore
 */
static int release_pharmacy_access(void) {
    return sem_post_safe(sem_pharmacy, "PHARMACY_ACCESS");
}

/**
 * Check stock availability for all medications in request
 * @return 1 if all items available, 0 otherwise
 */
static int check_stock_availability(int *meds_id, int *meds_qty, int meds_count) {
    for (int i = 0; i < meds_count; i++) {
        int med_id = meds_id[i];
        int qty_needed = meds_qty[i];
        
        if (med_id < 0 || med_id >= config->med_count) {
            return 0;  // Invalid medication
        }
        
        safe_pthread_mutex_lock(&shm_hospital->shm_pharm->medications[med_id].mutex);
        int available = shm_hospital->shm_pharm->medications[med_id].current_stock - 
                       shm_hospital->shm_pharm->medications[med_id].reserved;
        safe_pthread_mutex_unlock(&shm_hospital->shm_pharm->medications[med_id].mutex);
        
        if (available < qty_needed) {
            return 0;  // Not enough stock
        }
    }
    return 1;
}

/**
 * Reserve stock for a request (to prevent race conditions)
 */
static int reserve_stock(int *meds_id, int *meds_qty, int meds_count) {
    for (int i = 0; i < meds_count; i++) {
        int med_id = meds_id[i];
        int qty = meds_qty[i];
        
        safe_pthread_mutex_lock(&shm_hospital->shm_pharm->medications[med_id].mutex);
        shm_hospital->shm_pharm->medications[med_id].reserved += qty;
        safe_pthread_mutex_unlock(&shm_hospital->shm_pharm->medications[med_id].mutex);
    }
    return 0;
}

/**
 * Dispense medications - commit the reservation and reduce stock
 */
static int dispense_medications(int *meds_id, int *meds_qty, int meds_count) {
    for (int i = 0; i < meds_count; i++) {
        int med_id = meds_id[i];
        int qty = meds_qty[i];
        
        safe_pthread_mutex_lock(&shm_hospital->shm_pharm->medications[med_id].mutex);
        
        // Reduce both stock and reservation
        shm_hospital->shm_pharm->medications[med_id].current_stock -= qty;
        shm_hospital->shm_pharm->medications[med_id].reserved -= qty;
        
        // Check for stock depletion
        if (shm_hospital->shm_pharm->medications[med_id].current_stock == 0) {
            safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
            shm_hospital->shm_stats->stock_depletions++;
            safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
            
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "Stock depleted for %s", get_med_name(med_id));
            log_event(WARNING, "PHARMACY", "STOCK_DEPLETED", log_msg);
        }
        
        // Auto-restock if enabled and below threshold
        if (config->auto_restock_enabled && 
            shm_hospital->shm_pharm->medications[med_id].current_stock < 
            shm_hospital->shm_pharm->medications[med_id].threshold) {
            
            int restock_amount = shm_hospital->shm_pharm->medications[med_id].threshold * 
                                config->restock_qty_multiplier;
            shm_hospital->shm_pharm->medications[med_id].current_stock += restock_amount;
            
            safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
            shm_hospital->shm_stats->auto_restocks++;
            safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
            
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "Auto-restocked %s with %d units", 
                    get_med_name(med_id), restock_amount);
            log_event(INFO, "PHARMACY", "AUTO_RESTOCK", log_msg);
        }
        
        safe_pthread_mutex_unlock(&shm_hospital->shm_pharm->medications[med_id].mutex);
        
        // Update medication usage stats
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        shm_hospital->shm_stats->medication_usage[med_id] += qty;
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    }
    return 0;
}

/**
 * Release reserved stock on failure
 */
static void release_reservation(int *meds_id, int *meds_qty, int meds_count) {
    for (int i = 0; i < meds_count; i++) {
        int med_id = meds_id[i];
        int qty = meds_qty[i];
        
        safe_pthread_mutex_lock(&shm_hospital->shm_pharm->medications[med_id].mutex);
        shm_hospital->shm_pharm->medications[med_id].reserved -= qty;
        safe_pthread_mutex_unlock(&shm_hospital->shm_pharm->medications[med_id].mutex);
    }
}

/**
 * Write delivery record to file
 */
static int write_delivery_file(const char *patient_id, int *meds_id, int *meds_qty, 
                                int meds_count, time_t request_time, time_t completion_time) {
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "results/pharmacy_deliveries/%s_%ld.txt", 
             patient_id, (long)completion_time);
    
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Failed to create delivery file: %s", filepath);
        log_event(ERROR, "PHARMACY", "FILE_ERROR", log_msg);
        return -1;
    }
    
    fprintf(fp, "============================================\n");
    fprintf(fp, "       PHARMACY DELIVERY RECORD\n");
    fprintf(fp, "============================================\n\n");
    fprintf(fp, "Patient/Request ID: %s\n", patient_id);
    fprintf(fp, "Request Time:       %s", ctime(&request_time));
    fprintf(fp, "Delivery Time:      %s", ctime(&completion_time));
    fprintf(fp, "Items Delivered:    %d\n\n", meds_count);
    fprintf(fp, "--------------------------------------------\n");
    fprintf(fp, "              MEDICATIONS\n");
    fprintf(fp, "--------------------------------------------\n\n");
    
    for (int i = 0; i < meds_count; i++) {
        fprintf(fp, "  %-20s  x%d\n", get_med_name(meds_id[i]), meds_qty[i]);
    }
    
    fprintf(fp, "\n--------------------------------------------\n");
    fprintf(fp, "Delivery confirmed by Hospital Pharmacy\n");
    fprintf(fp, "============================================\n");
    
    fclose(fp);
    
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Delivery file created: %s", filepath);
    log_event(INFO, "PHARMACY", "DELIVERY_FILE", log_msg);
    
    return 0;
}

/**
 * Send pharmacy notification back to requester
 * Routes to correct queue based on sender
 * Uses operation_id as mtype so receivers can filter for their specific messages
 * 
 * This follows the same "Envelope/Payload" pattern as lab.c send_results_notification
 */
static int send_pharmacy_notification(const char *patient_id, int operation_id, int success, msg_sender_t sender) {
    msg_pharm_ready_t response;
    memset(&response, 0, sizeof(response));
    
    // --- 1. Fill the Payload (Content) ---
    // This is what goes INSIDE the envelope. The receiver will read this.
    response.hdr.kind = MSG_PHARM_READY;
    strncpy(response.hdr.patient_id, patient_id, sizeof(response.hdr.patient_id) - 1);
    
    response.hdr.operation_id = operation_id;
    
    response.hdr.timestamp = time(NULL);
    response.success = success;
    
    // --- 2. Determine target queue (Envelope routing) ---
    int target_queue;
    const char *target_name;
    
    // Set mtype based on operation_id for filtering
    if (operation_id > 0) {
        response.hdr.mtype = operation_id;
    } else {
        response.hdr.mtype = PRIORITY_NORMAL;  // Default mtype
    }
    
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
            // Use a fixed mtype for manager responses
            response.hdr.mtype = 2002;  // Pharmacy manager responses
            break;
            
        default:
            target_queue = mq_responses_id;
            target_name = "Unknown (responses)";
            log_event(WARNING, "PHARMACY", "UNKNOWN_SENDER", 
                     "Response for unknown sender, routing to responses queue");
            break;
    }
    
    // --- 3. Send the message ---
    if (send_generic_message(target_queue, &response, sizeof(response)) != 0) {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Failed to send pharmacy notification for %s to %s", 
                patient_id, target_name);
        log_event(ERROR, "PHARMACY", "MSG_SEND_FAIL", log_msg);
        return -1;
    }
    
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Pharmacy notification sent for %s (op_id: %d, success: %d) to %s",
             patient_id, operation_id, success, target_name);
    log_event(INFO, "PHARMACY", "NOTIFICATION_SENT", log_msg);
    
    return 0;
}

// --- Worker Thread Function ---

/**
 * Pharmacy Worker Thread: Processes one pharmacy request
 */
static void* pharmacy_worker_thread(void *arg) {
    pharmacy_worker_args_t *args = (pharmacy_worker_args_t *)arg;
    
    args->request_time = time(NULL);  // Record actual processing start
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Worker started for %s with %d items (op_id: %d)",
             args->patient_id, args->meds_count, args->operation_id);
    log_event(INFO, "PHARMACY", "WORKER_START", log_msg);
    
    int success = 0;
    int prep_duration = get_prep_duration();
    
    // Acquire pharmacy access (semaphore controls concurrent access)
    if (acquire_pharmacy_access() != 0) {
        if (check_shutdown()) {
            free(args);
            return NULL;
        }
        log_event(ERROR, "PHARMACY", "SEM_FAIL", "Failed to acquire pharmacy access");
        send_pharmacy_notification(args->patient_id, args->operation_id, 0, args->sender);
        free(args);
        return NULL;
    }
    
    // Check shutdown after acquiring
    if (check_shutdown()) {
        release_pharmacy_access();
        free(args);
        return NULL;
    }
    
    // Check stock availability
    if (check_stock_availability(args->meds_id, args->meds_qty, args->meds_count)) {
        // Reserve stock
        reserve_stock(args->meds_id, args->meds_qty, args->meds_count);
        
        // Release semaphore during preparation time (other requests can check stock)
        release_pharmacy_access();
        
        // Log start of preparation
        snprintf(log_msg, sizeof(log_msg), "Preparing order for %s (duration: %d units)",
                args->patient_id, prep_duration);
        log_event(INFO, "PHARMACY", "PREP_START", log_msg);
        
        // Simulate preparation time
        int start_time = get_simulation_time();
        wait_time_units(prep_duration);
        int end_time = get_simulation_time();
        
        if (check_shutdown()) {
            release_reservation(args->meds_id, args->meds_qty, args->meds_count);
            free(args);
            return NULL;
        }
        
        // Re-acquire for dispensing
        if (acquire_pharmacy_access() != 0) {
            if (!check_shutdown()) {
                log_event(ERROR, "PHARMACY", "SEM_FAIL", "Failed to re-acquire pharmacy access");
            }
            release_reservation(args->meds_id, args->meds_qty, args->meds_count);
            send_pharmacy_notification(args->patient_id, args->operation_id, 0, args->sender);
            free(args);
            return NULL;
        }
        
        // Dispense medications
        dispense_medications(args->meds_id, args->meds_qty, args->meds_count);
        
        // Update response time stats
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        shm_hospital->shm_stats->total_pharmacy_response_time += (end_time - start_time);
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
        
        release_pharmacy_access();
        
        success = 1;
        
        snprintf(log_msg, sizeof(log_msg), "Order completed for %s (%d items dispensed)",
                args->patient_id, args->meds_count);
        log_event(INFO, "PHARMACY", "PREP_COMPLETE", log_msg);
        
    } else {
        // Insufficient stock
        release_pharmacy_access();
        
        snprintf(log_msg, sizeof(log_msg), "Insufficient stock for %s request",
                args->patient_id);
        log_event(WARNING, "PHARMACY", "STOCK_INSUFFICIENT", log_msg);
    }
    
    time_t completion_time = time(NULL);
    
    if (!check_shutdown()) {
        // Generate delivery file on success
        if (success) {
            write_delivery_file(args->patient_id, args->meds_id, args->meds_qty,
                              args->meds_count, args->request_time, completion_time);
        }
        
        // Send notification to requester
        send_pharmacy_notification(args->patient_id, args->operation_id, success, args->sender);
    }
    
    snprintf(log_msg, sizeof(log_msg), "Worker completed for %s (success: %d)",
             args->patient_id, success);
    log_event(INFO, "PHARMACY", "WORKER_COMPLETE", log_msg);
    
    // Free arguments
    free(args);
    
    return NULL;
}

/**
 * Spawn a worker thread for a pharmacy request
 */
static int spawn_worker(msg_pharmacy_request_t *request) {
    // Allocate worker arguments
    pharmacy_worker_args_t *args = malloc(sizeof(pharmacy_worker_args_t));
    if (!args) {
        log_event(ERROR, "PHARMACY", "MALLOC_FAIL", "Failed to allocate worker args");
        return -1;
    }
    
    // Copy request data to worker args
    strncpy(args->patient_id, request->hdr.patient_id, sizeof(args->patient_id) - 1);
    args->patient_id[sizeof(args->patient_id) - 1] = '\0';
    args->operation_id = request->hdr.operation_id;
    args->meds_count = request->meds_count;
    args->request_time = request->hdr.timestamp;
    args->sender = request->sender;
    args->priority = (int)request->hdr.mtype;
    
    for (int i = 0; i < request->meds_count && i < 8; i++) {
        args->meds_id[i] = request->meds_id[i];
        args->meds_qty[i] = request->meds_qty[i];
    }
    
    // Create worker thread (detached)
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
    if (safe_pthread_create(&thread, &attr, pharmacy_worker_thread, args) != 0) {
        log_event(ERROR, "PHARMACY", "THREAD_FAIL", "Failed to create worker thread");
        free(args);
        pthread_attr_destroy(&attr);
        return -1;
    }
    
    pthread_attr_destroy(&attr);
    
    // Track active worker (for potential cleanup)
    safe_pthread_mutex_lock(&workers_mutex);
    if (active_worker_count < MAX_CONCURRENT_REQUESTS) {
        active_workers[active_worker_count++] = thread;
    }
    safe_pthread_mutex_unlock(&workers_mutex);
    
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Spawned worker for %s (%d items, priority: %d)",
             request->hdr.patient_id, request->meds_count, (int)request->hdr.mtype);
    log_event(INFO, "PHARMACY", "WORKER_SPAWNED", log_msg);
    
    return 0;
}

// --- Dispatcher Loop ---

/**
 * Main dispatcher loop for the Pharmacy Process
 * Receives MSG_PHARMACY_REQUEST messages and spawns worker threads
 * Handles priority: URGENT (1) > HIGH (2) > NORMAL (3)
 */
static void process_pharmacy_requests(void) {
    msg_pharmacy_request_t request;
    
    while (!check_shutdown()) {
        // Clear message buffer
        memset(&request, 0, sizeof(request));
        
        // Receive next pharmacy request (blocking with priority)
        // Using MAX_PRIORITY_TO_ACCEPT: URGENT(1) > HIGH(2) > NORMAL(3)
        int rc = receive_generic_message(mq_pharmacy_id, &request, sizeof(request), PRIORITY_NORMAL);
        
        if (rc != 0) {
            if (check_shutdown()) break;
            if (errno == EINTR) continue;  // Interrupted by signal
            
            // Log error but continue
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "msgrcv error: %d", errno);
            log_event(WARNING, "PHARMACY", "RECV_ERROR", log_msg);
            continue;
        }
        
        // Validate message type
        if (request.hdr.kind != MSG_PHARMACY_REQUEST) {
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "Unexpected message kind: %d", request.hdr.kind);
            log_event(WARNING, "PHARMACY", "INVALID_MSG", log_msg);
            continue;
        }
        
        // Update statistics based on priority
        safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
        shm_hospital->shm_stats->total_pharmacy_requests++;
        if (request.hdr.mtype == PRIORITY_URGENT) {
            shm_hospital->shm_stats->urgent_requests++;
        } else {
            shm_hospital->shm_stats->normal_requests++;
        }
        safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
        
        // Update active requests in pharmacy SHM
        safe_pthread_mutex_lock(&shm_hospital->shm_pharm->global_mutex);
        shm_hospital->shm_pharm->total_active_requests++;
        safe_pthread_mutex_unlock(&shm_hospital->shm_pharm->global_mutex);
        
        // Log received request
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Received pharmacy request for %s (%d items, op_id: %d, priority: %ld)",
                 request.hdr.patient_id, request.meds_count, request.hdr.operation_id, request.hdr.mtype);
        log_event(INFO, "PHARMACY", "REQUEST_RECV", log_msg);
        
        // Spawn worker thread to handle this request
        if (spawn_worker(&request) != 0) {
            log_event(ERROR, "PHARMACY", "SPAWN_FAIL", "Failed to spawn worker for request");
            
            // Decrement active requests on failure
            safe_pthread_mutex_lock(&shm_hospital->shm_pharm->global_mutex);
            shm_hospital->shm_pharm->total_active_requests--;
            safe_pthread_mutex_unlock(&shm_hospital->shm_pharm->global_mutex);
            
            // Send failure notification
            send_pharmacy_notification(request.hdr.patient_id, request.hdr.operation_id, 0, request.sender);
        }
    }
}

// --- Main Entry Point ---

void pharmacy_main(void) {
    setup_child_signals();
    
    // Seed random number generator
    srand((unsigned int)(time(NULL) ^ getpid()));
    
    // Run the dispatcher loop
    process_pharmacy_requests();
    
    child_cleanup();
    exit(EXIT_SUCCESS);
}