#define _DEFAULT_SOURCE

#include <unistd.h>
#include <errno.h>

#include "../include/config.h"
#include "../include/time_simulation.h"
#include "../include/shm.h"
#include "../include/safe_threads.h"
#include "../include/manager_utils.h"

void wait_time_units(int units) {
    if (units <= 0) return;
    
    // Sleep in small intervals to allow shutdown checks
    // Use 100ms intervals (same as semaphore timeout)
    long interval_us = 100000; // 100ms in microseconds
    long total_us = (long)units * config->time_unit_ms * 1000;
    
    while (total_us > 0 && !check_shutdown()) {
        long sleep_time = (total_us < interval_us) ? total_us : interval_us;
        int result = usleep(sleep_time);
        if (result == -1 && errno == EINTR) {
            // Interrupted by signal, check shutdown and continue
            continue;
        }
        total_us -= sleep_time;
    }
}

int get_simulation_time(void) {
    if (!shm_hospital || !shm_hospital->shm_stats) return 0;
    
    safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
    int time = shm_hospital->shm_stats->simulation_time_units;
    safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);
    
    return time;
}

int diff_time_units(int start, int end) {
    return end - start;
}

