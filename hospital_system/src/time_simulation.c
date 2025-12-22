#define _DEFAULT_SOURCE

#include <unistd.h>

#include "../include/config.h"
#include "../include/time_simulation.h"
#include "../include/shm.h"
#include "../include/safe_threads.h"

void wait_time_units(int units) {
    if (units <= 0) return;
    
    // usleep takes microseconds
    long sleep_time = (long)units * config->time_unit_ms * 1000;
    
    usleep(sleep_time);
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

