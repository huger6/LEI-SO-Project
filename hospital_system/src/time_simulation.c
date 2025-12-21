#define _DEFAULT_SOURCE

#include <unistd.h>

#include "../include/config.h"
#include "../include/time_simulation.h"

void wait_time_units(int units) {
    if (units <= 0) return;
    
    // usleep takes microseconds
    long sleep_time = (long)units * config->time_unit_ms * 1000;
    
    usleep(sleep_time);
}

