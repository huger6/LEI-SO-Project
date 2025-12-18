#include <stdio.h>
#include <semaphore.h>

#include "../include/sem.h"
#include "../include/log.h"

#define ROOM_BO1 1
#define ROOM_BO2 2
#define ROOM_BO3 3

/**
 * Acquires access to a specific surgery room
 * Blocks if the room is currently occupied
 * @param room_id The ID of the room (1, 2, or 3)
 * @return 0 on success, -1 on failure
 */
int acquire_surgery_room(int room_id) {
    sem_t *target_sem = NULL;
    const char *sem_name = NULL;

    // Select the correct global semaphore based on room ID
    switch (room_id) {
        case ROOM_BO1:
            target_sem = sem_bo1;
            sem_name = "BO1_SEMAPHORE";
            break;
        case ROOM_BO2:
            target_sem = sem_bo2;
            sem_name = "BO2_SEMAPHORE";
            break;
        case ROOM_BO3:
            target_sem = sem_bo3;
            sem_name = "BO3_SEMAPHORE";
            break;
        default: {
            char log_buffer[128];
            snprintf(log_buffer, sizeof(log_buffer), "acquire_surgery_room: Invalid room_id %d", room_id);
            log_event(ERROR, "SEMAPHORE", "SURG_ACQUIRE_FAIL", log_buffer);
            return -1;
        }
    }

    return sem_wait_safe(target_sem, sem_name);
}

/**
 * Releases a specific surgery room, making it available for other operations.
 * * @param room_id The ID of the room (1, 2, or 3)
 * @return 0 on success, -1 on failure (invalid ID or semaphore error)
 */
int release_surgery_room(int room_id) {
    sem_t *target_sem = NULL;
    const char *sem_name = NULL;

    // Select the correct global semaphore based on room ID
    switch (room_id) {
        case ROOM_BO1:
            target_sem = sem_bo1;
            sem_name = "BO1_SEMAPHORE";
            break;
        case ROOM_BO2:
            target_sem = sem_bo2;
            sem_name = "BO2_SEMAPHORE";
            break;
        case ROOM_BO3:
            target_sem = sem_bo3;
            sem_name = "BO3_SEMAPHORE";
            break;
        default: {
            char log_buffer[128];
            snprintf(log_buffer, sizeof(log_buffer), "release_surgery_room: Invalid room_id %d", room_id);
            log_event(ERROR, "SEMAPHORE", "SURG_RELEASE_FAIL", log_buffer);
            return -1;
        }
    }

    // Use the safe checked post function
    return sem_post_safe(target_sem, sem_name);
}

