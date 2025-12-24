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

void pharmacy_main(void) {
    setup_child_signals();
    
    close_unused_pipe_ends(ROLE_PHARMACY);

    // Cleanup on shutdown
    log_event(INFO, "PHARMACY", "SHUTDOWN", "Pharmacy process shutting down");
    
    // Resources cleanup
    log_event(INFO, "PHARMACY", "RESOURCES_CLEANUP", "Cleaning pharmacy resources");
    child_cleanup();

    exit(EXIT_SUCCESS);
}