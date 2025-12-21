#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

#include "../include/manager_utils.h"
#include "../include/pipes.h"
#include "../include/log.h"
#include "../include/sem.h"
#include "../include/config.h"
#include "../include/mq.h"
#include "../include/shm.h"

volatile sig_atomic_t g_stop_child = 0;

// Generic handler that writes to the self-pipe
static void generic_signal_handler(int sig) {
    notify_manager_from_signal(sig);
}

// Handler for child processes (no SA_RESTART)
static void child_signal_handler(int sig) {
    (void)sig;
    g_stop_child = 1;
}

// Setup all signal handlers
void setup_signal_handlers(void) {
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // Restart syscalls if possible

    sa.sa_handler = generic_signal_handler;

    // Register signals to be handled via pipe
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
}

// Setup signal handlers for child processes
void setup_child_signals(void) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART to interrupt blocking calls
    sa.sa_handler = child_signal_handler;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

// Returns 1 if valid, 0 if invalid
int validate_patient_id(const char *id) {
    if (!id) return 0;
    
    size_t len = strlen(id);

    // 1. Check strict length constraint from PDF (5-15 chars)
    if (len < 5 || len > 15) return 0;

    // 2. Check for "PAC" prefix (Optional, based on your preference)
    if (strncmp(id, "PAC", 3) != 0) return 0;

    // 3. Check that the rest are digits
    for (size_t i = 3; i < len; i++) {
        if (!isdigit((unsigned char)id[i])) return 0;
    }

    return 1;
}

// Use only on child processes
void child_cleanup() {
    cleanup_child_shm();
    close_all_semaphores();
    cleanup_config();
    close_logging();
}

// Cleanup all resources for the manager
void manager_cleanup() {
    log_event(INFO, "SYSTEM", "SHUTDOWN", "Initiating system shutdown");
    
    // Kill children if needed

    if (pid_console_input > 0) {
        if (kill(pid_console_input, SIGTERM) == 0) {
            waitpid(pid_console_input, NULL, 0);
        } else if (errno != ESRCH) {
            log_event(ERROR, "MANAGER", "PROCESS_KILL", "Kill console input process command failed");
        }
    }
    if (pid_triage > 0) {
        if (kill(pid_triage, SIGTERM) == 0) {
            waitpid(pid_triage, NULL, 0);
        } else if (errno != ESRCH) {
            log_event(ERROR, "MANAGER", "PROCESS_KILL", "Kill triage process command failed");
        }
    }
    if (pid_surgery > 0) {
        if (kill(pid_surgery, SIGTERM) == 0) {
            waitpid(pid_surgery, NULL, 0);
        } else if (errno != ESRCH) {
            log_event(ERROR, "MANAGER", "PROCESS_KILL", "Kill surgery process command failed");
        }
    }
    if (pid_pharmacy > 0) {
        if (kill(pid_pharmacy, SIGTERM) == 0) {
            waitpid(pid_pharmacy, NULL, 0);
        } else if (errno != ESRCH) {
            log_event(ERROR, "MANAGER", "PROCESS_KILL", "Kill pharmacy process command failed");
        }
    }
    if (pid_lab > 0) {
        if (kill(pid_lab, SIGTERM) == 0) {
            waitpid(pid_lab, NULL, 0);
        } else if (errno != ESRCH) {
            log_event(ERROR, "MANAGER", "PROCESS_KILL", "Kill laboratory process command failed");
        }
    }

    // Disable SHM logging before destroying SHM
    set_critical_log_shm_ptr(NULL);

    cleanup_all_shm();
    remove_all_message_queues();
    destroy_all_pipes();
    close_all_semaphores();
    unlink_all_semaphores();
    cleanup_config();

    log_event(INFO, "SYSTEM", "SHUTDOWN", "Shutdown was successful. Goodbye");

    close_logging();
}