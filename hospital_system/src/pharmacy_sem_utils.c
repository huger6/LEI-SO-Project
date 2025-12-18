#include <stdio.h>
#include <semaphore.h>

#include "sem.h"


/**
 * Acquires access to the Pharmacy Central
 * Blocks if the maximum number of concurrent pharmacy operations (4) is reached
 * @return 0 on success, -1 on failure
 */
int acquire_pharmacy_access(void) {
    return sem_wait_safe(sem_pharmacy, "PHARMACY_ACCESS");
}

/**
 * Releases access to the Pharmacy Central
 * Should be called immediately after pharmacy operations (stock check/update) are complete
 * * @return 0 on success, -1 on failure
 */
int release_pharmacy_access(void) {
    return sem_post_safe(sem_pharmacy, "PHARMACY_ACCESS");
}