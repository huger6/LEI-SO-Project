#include <stdio.h>
#include <semaphore.h>

#include "sem.h"
#include "../include/log.h"

#define LAB1_ID 1
#define LAB2_ID 2

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