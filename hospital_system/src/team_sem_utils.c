#include <stdio.h>
#include <semaphore.h>

#include "sem.h"


/**
 * Acquires a medical team for a surgery
 * Blocks if all medical teams are currently occupied
 * @return 0 on success, -1 on failure
 */
int acquire_medical_team(void) {
    return sem_wait_safe(sem_medical_teams, "MEDICAL_TEAMS");
}

/**
 * Releases a medical team back to the pool after a surgery is completed
 * @return 0 on success, -1 on failure
 */
int release_medical_team(void) {
    return sem_post_safe(sem_medical_teams, "MEDICAL_TEAMS");
}

