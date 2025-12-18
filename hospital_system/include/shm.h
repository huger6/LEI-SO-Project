#ifndef SHM_H
#define SHM_H

#include <sys/types.h>
#include <time.h>

#include "config.h"
#include "hospital.h"
#include "stats.h"
#include "log.h"

// FTOK (update mq if changed)
#define FTOK_PATH     "/tmp/hospital_key"

// Shared Memory Keys (Arbitrary chars for ftok)
#define SHM_STATS_KEY 'S'
#define SHM_SURG_KEY  'O'
#define SHM_PHARM_KEY 'P'
#define SHM_LAB_KEY   'L'
#define SHM_LOG_KEY   'G'

// Hospital SHM memory structure
typedef struct {
    global_statistics_shm_t *shm_stats;
    surgery_block_shm_t *shm_surg;
    pharmacy_shm_t *shm_pharm;
    lab_queue_shm_t *shm_lab;
    critical_log_shm_t *shm_critical_logger;
} hospital_shm_t;

// SHM
extern hospital_shm_t *shm_hospital;


// --- Function Headers ---

int init_all_shm();
void cleanup_all_shm();
void init_all_shm_data(system_config_t *configs);


#endif