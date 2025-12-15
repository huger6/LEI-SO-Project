#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>

#include "../include/ipc.h" 
#include "../include/log.h"
#include "../include/hospital.h"
#include "../include/stats.h"


// External globals for IDs
extern int mq_urgent_id, mq_normal_id, mq_resp_id;
extern int shm_stats_id, shm_surgery_id, shm_pharm_id, shm_lab_id, shm_log_id;

// IPC IDs
int mq_urgent_id = -1;
int mq_normal_id = -1;
int mq_resp_id = -1;

int shm_stats_id = -1;
int shm_surgery_id = -1;
int shm_pharm_id = -1;
int shm_lab_id = -1;
int shm_log_id = -1;

// SHM
hospital_shm_t *shm_hospital = NULL;

// --- Message Queues ---

int create_message_queues() {
    // mq_triage = 
}

int send_message(int mqid, hospital_message_t *msg) {

}

int receive_message_priority(int mqid, hospital_message_t *msg) {

}

void cleanup_message_queues() {

}

// --- SHM ---

// Create all SHM IDS (no attach)
static int create_shm() {
    shm_stats_id = shmget(ftok(FTOK_PATH, SHM_STATS_KEY), sizeof(global_statistics_shm_t), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_stats_id == -1) {
        log_event(ERROR, "IPC", "SHM", "Failed to create Stats SHM");
        return -1;
    }

    shm_surgery_id = shmget(ftok(FTOK_PATH, SHM_SURG_KEY), sizeof(surgery_block_shm_t), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_surgery_id == -1) {
        log_event(ERROR, "IPC", "SHM", "Failed to create Surgery SHM");
        return -1;
    }

    shm_pharm_id = shmget(ftok(FTOK_PATH, SHM_PHARM_KEY), sizeof(pharmacy_shm_t), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_pharm_id == -1) {
        log_event(ERROR, "IPC", "SHM", "Failed to create Pharmacy SHM");
        return -1;
    }

    shm_lab_id = shmget(ftok(FTOK_PATH, SHM_LAB_KEY), sizeof(lab_queue_shm_t), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_lab_id == -1) {
        log_event(ERROR, "IPC", "SHM", "Failed to create Lab SHM");
        return -1;
    }

    shm_log_id = shmget(ftok(FTOK_PATH, SHM_LOG_KEY), sizeof(critical_log_shm_t), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_log_id == -1) {
        log_event(ERROR, "IPC", "SHM", "Failed to create Log SHM");
        return -1;
    }

    return 0;
}

// Attach shm
static void *attach_shm(int shm_id) {
    return shmat(shm_id, NULL, 0);
}

// Initialize all SHM
int init_all_shm() {
    // --- Create SHM ---
    if (create_shm() == -1) {
        // log already happens on create_shm
        return -1;
    }

    // --- Create main container ---
    shm_hospital = (hospital_shm_t *) calloc(1, sizeof(hospital_shm_t));
    if (!shm_hospital) {
        log_event(ERROR, "IPC", "SHM", "Memory allocation for the SHM container failed");
        return -1;
    }

    // --- Attach each segment ---
    // Stats
    shm_hospital->shm_stats = (global_statistics_shm_t *) attach_shm(shm_stats_id);
    if (shm_hospital->shm_stats == (void *)-1) {
        log_event(ERROR, "IPC", "SHM", "Failed to attach Stats SHM");
        return -1;
    }

    // Surgery
    shm_hospital->shm_surg = (surgery_block_shm_t *) attach_shm(shm_surgery_id);
    if (shm_hospital->shm_surg == (void *)-1) {
        log_event(ERROR, "IPC", "SHM", "Failed to attach Surgery SHM");
        return -1;
    }

    // Pharmacy
    shm_hospital->shm_pharm = (pharmacy_shm_t *) attach_shm(shm_pharm_id);
    if (shm_hospital->shm_pharm == (void *)-1) {
        log_event(ERROR, "IPC", "SHM", "Failed to attach Pharmacy SHM");
        return -1;
    }

    // Lab
    shm_hospital->shm_lab = (lab_queue_shm_t *) attach_shm(shm_lab_id);
    if (shm_hospital->shm_lab == (void *)-1) {
        log_event(ERROR, "IPC", "SHM", "Failed to attach Lab SHM");
        return -1;
    }

    // Log
    shm_hospital->shm_critical_logger = (critical_log_shm_t *) attach_shm(shm_log_id);
    if (shm_hospital->shm_critical_logger == (void *)-1) {
        log_event(ERROR, "IPC", "SHM", "Failed to attach Log SHM");
        return -1;
    }

    // --- Init SHM content ---
    memset(shm_hospital->shm_stats, 0, sizeof(global_statistics_shm_t));
    memset(shm_hospital->shm_surg, 0, sizeof(surgery_block_shm_t));
    memset(shm_hospital->shm_pharm, 0, sizeof(pharmacy_shm_t));
    memset(shm_hospital->shm_lab, 0, sizeof(lab_queue_shm_t));
    memset(shm_hospital->shm_critical_logger, 0, sizeof(critical_log_shm_t));

    // --- Init all mutex's ---
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

    // Stats Mutex
    init_stats_default(shm_hospital->shm_stats, &attr);

    // Surgery Mutexes
    pthread_mutex_init(&shm_hospital->shm_surg->teams_mutex, &attr);
    for(int i = 0; i < 3; i++) {
        pthread_mutex_init(&shm_hospital->shm_surg->rooms[i].mutex, &attr);
    }

    // Pharmacy Mutexes
    pthread_mutex_init(&shm_hospital->shm_pharm->global_mutex, &attr);
    for(int i = 0; i < 15; i++) {
        pthread_mutex_init(&shm_hospital->shm_pharm->medications[i].mutex, &attr);
    }

    // Lab Mutexes
    pthread_mutex_init(&shm_hospital->shm_lab->lab1_mutex, &attr);
    pthread_mutex_init(&shm_hospital->shm_lab->lab2_mutex, &attr);

    // Log Mutex
    pthread_mutex_init(&shm_hospital->shm_critical_logger->mutex, &attr);

    pthread_mutexattr_destroy(&attr);

    return 0;
}

// Clean all SHM 
void cleanup_all_shm() {
    log_event(INFO, "IPC", "SHM", "Starting SHM resources cleanup");
    if (shm_hospital) {
        // Detach
        if (shm_hospital->shm_stats && shm_hospital->shm_stats != (void *)-1) shmdt(shm_hospital->shm_stats);
        if (shm_hospital->shm_surg && shm_hospital->shm_surg != (void *)-1) shmdt(shm_hospital->shm_surg);
        if (shm_hospital->shm_pharm && shm_hospital->shm_pharm != (void *)-1) shmdt(shm_hospital->shm_pharm);
        if (shm_hospital->shm_lab && shm_hospital->shm_lab != (void *)-1) shmdt(shm_hospital->shm_lab);
        if (shm_hospital->shm_critical_logger && shm_hospital->shm_critical_logger != (void *)-1) shmdt(shm_hospital->shm_critical_logger);
        
        // Free main container
        free(shm_hospital);
        shm_hospital = NULL;
    }

    // Remove IDs 
    if (shm_stats_id != -1) shmctl(shm_stats_id, IPC_RMID, NULL);
    if (shm_surgery_id != -1) shmctl(shm_surgery_id, IPC_RMID, NULL);
    if (shm_pharm_id != -1) shmctl(shm_pharm_id, IPC_RMID, NULL);
    if (shm_lab_id != -1) shmctl(shm_lab_id, IPC_RMID, NULL);
    if (shm_log_id != -1) shmctl(shm_log_id, IPC_RMID, NULL);

    log_event(INFO, "IPC", "SHM", "Finished cleaning SHM resources");
}

