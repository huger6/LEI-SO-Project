#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "../include/config.h"
#include "../include/hospital.h"
#include "../include/ipc.h"
#include "../include/stats.h"
#include "../include/log.h"

// --- FILE PATHS ---
#define CONFIG_PATH     "config/config.txt"         // config.txt 
#define LOGS_PATH       "logs/hospital_log.txt"     // hospital_logs.txt


// --- Global Resources ---
system_config_t config;
critical_log_shm_t critical_logger;
global_statistics_t *shm_stats;
pharmacy_shm_t *shm_pharm;

// IPC IDs
int mq_urgent_id, mq_normal_id, mq_resp_id;
int shm_stats_id, shm_surgery_id, shm_pharm_id, shm_lab_id, shm_log_id;
// Child PIDs
pid_t pid_triage, pid_surgery, pid_pharmacy, pid_lab;

volatile sig_atomic_t g_shutdown = 0;


// --- Signal Handlers ---

void sigint_handler(int sig) {
    (void)sig;
    const char *msg = "\n[MAIN] SIGINT received. Shutting down...\n";
    write(STDOUT_FILENO, msg, strlen(msg));
    g_shutdown = 1;
}

void sigusr1_handler(int sig) {
    (void)sig;
    // display_statistics_console(shm_stats);
}

void sigusr2_handler(int sig) {
    (void)sig;
    // save_statistics_snapshot(shm_stats);
}

void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// Setup all signal handlers
void setup_signal_handlers() {
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // SIGINT
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    // SIGUSR1
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    // SIGUSR2
    sa.sa_handler = sigusr2_handler;
    sigaction(SIGUSR2, &sa, NULL);

    // SIGCHLD
    // Restart only if child process dies
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; // SA_RESTART automaitcally restarts sys calls
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);
}

// --- Main ---

int main(void) {
    // --- Initialize default system configs ---
    init_default_config(&config);

    // --- Initialize logging system ---
    init_logging(LOGS_PATH, &critical_logger);
    log_event(INFO, "SYSTEM", "STARTUP", "Hospital system starting");

    // --- Signal handlers ---
    setup_signal_handlers();

    // --- Load config.txt ---
    if (load_config(CONFIG_PATH, &config) != 0) {
        log_event(ERROR, "CONFIG", "LOAD_FAILED", "Invalid config.txt file");
        // SHUTDOWN_SYSTEM CALL HERE
    }
    log_event(INFO, "CONFIG", "LOADED", "Configuration loaded sucessfully");

    // Console Output for Debugging
    #ifdef DEBUG
        print_configs(&config);
    #endif

    // --- IPC ---


    return 0;
}