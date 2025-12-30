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
#include "../include/dispatcher.h"
#include "../include/safe_threads.h"

extern volatile sig_atomic_t g_shutdown;

// Mutex for g_shutdown flag
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static void terminate_child_process(const char *role, pid_t *pid_ptr) {
    (void)role;
    if (!pid_ptr || *pid_ptr <= 0) return;

    #ifdef DEBUG
        char dbg[160];
        snprintf(dbg, sizeof(dbg), "Terminating child: role=%s pid=%d", role ? role : "UNKNOWN", (int)*pid_ptr);
        log_event(DEBUG_LOG, "SYSTEM", "TERMINATE_CHILD", dbg);
    #endif

    if (kill(*pid_ptr, SIGTERM) == 0) {
        int status = 0;
        pid_t waited = waitpid(*pid_ptr, &status, 0);
        (void)waited;
        #ifdef DEBUG
            char dbg[200];
            if (waited == -1) {
                snprintf(dbg, sizeof(dbg), "waitpid failed: role=%s pid=%d errno=%d (%s)", role ? role : "UNKNOWN", (int)*pid_ptr, errno, strerror(errno));
            } else if (WIFEXITED(status)) {
                snprintf(dbg, sizeof(dbg), "Child terminated: role=%s pid=%d exit_code=%d", role ? role : "UNKNOWN", (int)*pid_ptr, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                snprintf(dbg, sizeof(dbg), "Child terminated: role=%s pid=%d sig=%d", role ? role : "UNKNOWN", (int)*pid_ptr, WTERMSIG(status));
            } else {
                snprintf(dbg, sizeof(dbg), "Child terminated: role=%s pid=%d status=0x%x", role ? role : "UNKNOWN", (int)*pid_ptr, status);
            }
            log_event(DEBUG_LOG, "SYSTEM", "CHILD_TERMINATED", dbg);
        #endif
    } else {
        #ifdef DEBUG
            char dbg[200];
            snprintf(dbg, sizeof(dbg), "kill(SIGTERM) failed: role=%s pid=%d errno=%d (%s)", role ? role : "UNKNOWN", (int)*pid_ptr, errno, strerror(errno));
            log_event(DEBUG_LOG, "SYSTEM", "TERMINATE_CHILD_FAIL", dbg);
        #endif
    }

    *pid_ptr = -1;
}

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
// Writes SIGINT to manager's signal pipe to trigger graceful shutdown
// Also sets local g_shutdown to interrupt blocking calls in this child
static void child_signal_handler(int sig) {
    // Set local flag to interrupt blocking calls (msgrcv, etc.)
    // g_shutdown = 1;
    
    // Forward SIGINT to manager via inherited signal pipe
    // This triggers the manager to initiate proper graceful shutdown
    // SIGTERM comes from manager so no need to forward it back
    if (sig == SIGINT) {
        notify_signal(SIGINT);
    }
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

    // User defined signals - ignored in children
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
    #ifdef DEBUG
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "Child cleanup starting: pid=%d", (int)getpid());
        log_event(DEBUG_LOG, "SYSTEM", "CHILD_CLEANUP", dbg);
    #endif
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "CHILD_CLEANUP_STEP", "Cleaning up child SHM");
    #endif
    cleanup_child_shm();
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "CHILD_CLEANUP_STEP", "Closing all semaphores");
    #endif
    close_all_semaphores();
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "CHILD_CLEANUP_STEP", "Cleaning up config");
    #endif
    cleanup_config();
    
    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "CHILD_CLEANUP_STEP", "Closing logging");
    #endif
    close_logging();
}

// Cleanup all resources for the manager
void manager_cleanup() {
    #ifdef DEBUG
        char dbg[180];
        snprintf(dbg, sizeof(dbg), "Manager cleanup starting: manager_pid=%d triage=%d surgery=%d pharmacy=%d lab=%d",
                 (int)getpid(), (int)pid_triage, (int)pid_surgery, (int)pid_pharmacy, (int)pid_lab);
        log_event(DEBUG_LOG, "SYSTEM", "MANAGER_CLEANUP", dbg);
    #endif

    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "TERMINATE_CHILDREN", "Starting to terminate child processes");
    #endif
    // Kill children if needed
    terminate_child_process("TRIAGE", &pid_triage);
    terminate_child_process("SURGERY", &pid_surgery);
    terminate_child_process("PHARMACY", &pid_pharmacy);
    terminate_child_process("LAB", &pid_lab);

    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "DISABLE_LOGGING", "Disabling SHM logging");
    #endif
    // Disable SHM logging before destroying SHM
    set_critical_log_shm_ptr(NULL);

    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "CLEANUP_SCHEDULER", "Cleaning up scheduler");
    #endif
    cleanup_scheduler();

    #ifdef DEBUG
        log_event(DEBUG_LOG, "IPC", "CLEANUP", "Cleaning shared memory segments");
    #endif
    cleanup_all_shm();

    #ifdef DEBUG
        log_event(DEBUG_LOG, "IPC", "CLEANUP", "Removing message queues");
    #endif
    int mq_status = remove_all_message_queues();
    (void)mq_status;

    #ifdef DEBUG
        char dbg_mq[96];
        snprintf(dbg_mq, sizeof(dbg_mq), "remove_all_message_queues() -> %d", mq_status);
        log_event(DEBUG_LOG, "IPC", "CLEANUP_RESULT", dbg_mq);
    #endif

    #ifdef DEBUG
        log_event(DEBUG_LOG, "IPC", "CLEANUP", "Cleaning pipes");
    #endif
    int pipe_status = cleanup_pipes();
    (void)pipe_status;

    #ifdef DEBUG
        char dbg_pipe[96];
        snprintf(dbg_pipe, sizeof(dbg_pipe), "cleanup_pipes() -> %d", pipe_status);
        log_event(DEBUG_LOG, "IPC", "CLEANUP_RESULT", dbg_pipe);
    #endif

    #ifdef DEBUG
        log_event(DEBUG_LOG, "IPC", "CLEANUP", "Closing semaphore handles");
    #endif
    close_all_semaphores();

    #ifdef DEBUG
        log_event(DEBUG_LOG, "IPC", "CLEANUP", "Unlinking named semaphores");
    #endif
    int sem_unlink_status = unlink_all_semaphores();
    (void)sem_unlink_status;

    #ifdef DEBUG
        char dbg_sem[96];
        snprintf(dbg_sem, sizeof(dbg_sem), "unlink_all_semaphores() -> %d", sem_unlink_status);
        log_event(DEBUG_LOG, "IPC", "CLEANUP_RESULT", dbg_sem);
    #endif

    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "CLEANUP_CONFIG", "Cleaning up configuration");
    #endif
    cleanup_config();

    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "CLOSE_LOGGING", "Closing logging system");
    #endif

    // We call it here otherwise can't be called (we assume log closes successfully)
    log_event(INFO, "SYSTEM", "SHUTDOWN", "Shutdown was successful. Goodbye!");

    close_logging();

    #ifdef DEBUG
        log_event(DEBUG_LOG, "SYSTEM", "MANAGER_CLEANUP_COMPLETE", "Manager cleanup completed");
    #endif
}

void poison_pill_triage() {
    // Send poison pill for emergency queue manager
    msg_new_emergency_t poison_pill_emergency;
    memset(&poison_pill_emergency, 0, sizeof(msg_new_emergency_t));
    poison_pill_emergency.hdr.mtype = MSG_NEW_EMERGENCY;
    poison_pill_emergency.hdr.kind = MSG_SHUTDOWN;
    send_generic_message(mq_triage_id, &poison_pill_emergency, sizeof(msg_new_emergency_t));
    
    // Send poison pill for appointment queue manager
    msg_new_appointment_t poison_pill_appointment;
    memset(&poison_pill_appointment, 0, sizeof(msg_new_appointment_t));
    poison_pill_appointment.hdr.mtype = MSG_NEW_APPOINTMENT;
    poison_pill_appointment.hdr.kind = MSG_SHUTDOWN;
    send_generic_message(mq_triage_id, &poison_pill_appointment, sizeof(msg_new_appointment_t));
}

void poison_pill_surgery() {
    msg_new_surgery_t poison_pill;
    memset(&poison_pill, 0, sizeof(msg_new_surgery_t));
    poison_pill.hdr.mtype = MSG_NEW_SURGERY;
    poison_pill.hdr.kind = MSG_SHUTDOWN;
    send_generic_message(mq_surgery_id, &poison_pill, sizeof(msg_new_surgery_t));
}

void poison_pill_pharmacy() {
    msg_pharmacy_request_t poison_pill;
    memset(&poison_pill, 0, sizeof(msg_pharmacy_request_t));
    poison_pill.hdr.mtype = PRIORITY_URGENT;
    poison_pill.hdr.kind = MSG_SHUTDOWN;
    send_generic_message(mq_pharmacy_id, &poison_pill, sizeof(msg_pharmacy_request_t));
}

void poison_pill_lab() {
    msg_lab_request_t poison_pill;
    memset(&poison_pill, 0, sizeof(msg_lab_request_t));
    poison_pill.hdr.mtype = PRIORITY_URGENT;
    poison_pill.hdr.kind = MSG_SHUTDOWN;
    send_generic_message(mq_lab_id, &poison_pill, sizeof(msg_lab_request_t));
}