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
#include "../include/scheduler.h"
#include "../include/safe_threads.h"

extern volatile sig_atomic_t g_shutdown;
volatile sig_atomic_t g_stop_child = 0;

pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

int check_shutdown() {
    int val;

    safe_pthread_mutex_lock(&state_mutex);
    val = g_shutdown;
    safe_pthread_mutex_unlock(&state_mutex);

    return val;
}

void set_shutdown() {
    safe_pthread_mutex_lock(&state_mutex);
    g_shutdown = 1;
    safe_pthread_mutex_unlock(&state_mutex);
}

// Generic handler that writes to the self-pipe
static void generic_signal_handler(int sig) {
    notify_signal(sig);
}

// Handler for child processes (no SA_RESTART)
static void child_signal_handler(int sig) {
    (void)sig;
    g_stop_child = 1;
    g_shutdown = 1;
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

    // User defined signals
    sa.sa_handler = SIG_IGN;

    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
}

// Returns 1 if valid, 0 if invalid
// Validates ID based on type:
// - ID_TYPE_PATIENT: PAC{number} (5-15 chars)
// - ID_TYPE_LAB: LAB{number} (5-15 chars)
// - ID_TYPE_PHARMACY: REQ{number} (5-15 chars)
int validate_id(const char *id, id_type_t type) {
    if (!id) return 0;
    
    size_t len = strlen(id);

    // Check strict length constraint (5-15 chars)
    if (len < 5 || len > 15) return 0;

    // Check prefix based on type
    const char *expected_prefix;
    switch (type) {
        case ID_TYPE_PATIENT:
            expected_prefix = "PAC";
            break;
        case ID_TYPE_LAB:
            expected_prefix = "LAB";
            break;
        case ID_TYPE_PHARMACY:
            expected_prefix = "REQ";
            break;
        default:
            return 0;
    }

    if (strncmp(id, expected_prefix, 3) != 0) return 0;

    // Check that the rest are digits
    for (size_t i = 3; i < len; i++) {
        if (!isdigit((unsigned char)id[i])) return 0;
    }

    return 1;
}

// Legacy function for backward compatibility
int validate_patient_id(const char *id) {
    return validate_id(id, ID_TYPE_PATIENT);
}

// ===== Command Format Print Helpers =====

void print_status_format(void) {
    printf("Format: STATUS <component>\n");
    printf("  <component>: ALL | TRIAGE | SURGERY | PHARMACY | LAB\n");
}

void print_emergency_format(void) {
    printf("Format: EMERGENCY <patient_id> init: <time> triage: <1-5> stability: <value> [tests: <test1,test2,...>] [meds: <med1,med2,...>]\n");
    printf("  <patient_id>: PAC followed by digits (e.g., PAC001)\n");
}

void print_appointment_format(void) {
    printf("Format: APPOINTMENT <patient_id> init: <time> scheduled: <time> doctor: <specialty> [tests: <test1,test2,...>]\n");
    printf("  <patient_id>: PAC followed by digits (e.g., PAC001)\n");
    printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
}

void print_surgery_format(void) {
    printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
    printf("  <patient_id>: PAC followed by digits (e.g., PAC001)\n");
    printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
    printf("  <level>: LOW | MEDIUM | HIGH\n");
    printf("  Note: PREOP test is required\n");
}

void print_pharmacy_format(void) {
    printf("Format: PHARMACY_REQUEST <request_id> init: <time> priority: <priority> items: <med1:qty1,med2:qty2,...>\n");
    printf("  <request_id>: REQ followed by digits (e.g., REQ001)\n");
    printf("  <priority>: URGENT | HIGH | NORMAL\n");
}

void print_lab_format(void) {
    printf("Format: LAB_REQUEST <lab_id> init: <time> priority: <priority> lab: <lab> tests: <test1,test2,...>\n");
    printf("  <lab_id>: LAB followed by digits (e.g., LAB001)\n");
    printf("  <priority>: URGENT | NORMAL\n");
    printf("  <lab>: LAB1 | LAB2 | BOTH\n");
    printf("  Tests per lab:\n");
    printf("    LAB1: HEMO, GLIC\n");
    printf("    LAB2: COLEST, RENAL, HEPAT\n");
    printf("    BOTH: any test (PREOP requires BOTH)\n");
}

void print_restock_format(void) {
    printf("Format: RESTOCK <medication_name> quantity: <amount>\n");
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
    // Kill children if needed
    if (pid_triage > 0) {
        if (kill(pid_triage, SIGTERM) == 0) {
            waitpid(pid_triage, NULL, 0);
        }
    }
    if (pid_surgery > 0) {
        if (kill(pid_surgery, SIGTERM) == 0) {
            waitpid(pid_surgery, NULL, 0);
        }
    }
    if (pid_pharmacy > 0) {
        if (kill(pid_pharmacy, SIGTERM) == 0) {
            waitpid(pid_pharmacy, NULL, 0);
        }
    }
    if (pid_lab > 0) {
        if (kill(pid_lab, SIGTERM) == 0) {
            waitpid(pid_lab, NULL, 0);
        }
    }

    // Disable SHM logging before destroying SHM
    set_critical_log_shm_ptr(NULL);

    cleanup_scheduler();
    cleanup_all_shm();
    remove_all_message_queues();
    cleanup_pipes();
    close_all_semaphores();
    unlink_all_semaphores();
    cleanup_config();

    log_event(INFO, "SYSTEM", "SHUTDOWN", "System shutdown complete");

    close_logging();
}

void poison_pill_triage() {
    msg_new_emergency_t poison_pill;
    memset(&poison_pill, 0, sizeof(msg_new_emergency_t));
    poison_pill.hdr.mtype = MSG_NEW_EMERGENCY;
    poison_pill.hdr.kind = MSG_SHUTDOWN;
    send_generic_message(mq_triage_id, &poison_pill, sizeof(msg_new_emergency_t));
}

void poison_pill_surgery() {
    msg_new_surgery_t poison_pill;
    memset(&poison_pill, 0, sizeof(msg_new_surgery_t));
    poison_pill.hdr.mtype = MSG_NEW_SURGERY;
    poison_pill.hdr.kind = MSG_SHUTDOWN;
    send_generic_message(mq_triage_id, &poison_pill, sizeof(msg_new_surgery_t));
}