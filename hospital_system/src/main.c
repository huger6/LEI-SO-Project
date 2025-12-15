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

// Child PIDs
pid_t pid_triage, pid_surgery, pid_pharmacy, pid_lab;

// FLags
volatile sig_atomic_t g_shutdown = 0;
volatile sig_atomic_t display_stats_request = 0;
volatile sig_atomic_t save_stats_request = 0;
volatile sig_atomic_t child_exit_request = 0;

// --- Signal Handlers ---

void sigint_handler(int sig) {
    (void)sig;
    const char *msg = "\n[MAIN] SIGINT received. Shutting down...\n";
    write(STDOUT_FILENO, msg, strlen(msg));
    g_shutdown = 1;
}

void sigusr1_handler(int sig) {
    (void)sig;
    display_stats_request = 1;
}

void sigusr2_handler(int sig) {
    (void)sig;
    save_stats_request = 1;
}

void sigchld_handler(int sig) {
    (void)sig;
    child_exit_request = 1;
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
    init_logging(LOGS_PATH);
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

    // --- @IPC ---

    log_event(INFO, "IPC", "INIT", "Starting IPC mechanisms");

    // --- Message Queues ---


    // --- SHM ---
    if (init_all_shm() != 0) {
        // log_event is already called on init_all_shm()
        cleanup_all_shm();
    }

    // Update critical logger ptr
    set_critical_log_shm_ptr(shm_hospital->shm_critical_logger);


    // --- Fork ---


    // --- Main loop ---
    while(!g_shutdown) {
        // --- SIGUSR1 ---
        if (display_stats_request == 1) {
            printf("SIGUSR1 called");
            display_stats_request = 0;
            display_statistics_console(shm_hospital->shm_stats);
        }
        // --- SIGUSR2 ---
        if (save_stats_request == 1) {
            save_stats_request = 0;
            save_statistics_snapshot(shm_hospital->shm_stats);
        }
        // --- SIGCHLD ---
        if (child_exit_request == 1) {
            child_exit_request = 0;

            int status;
            pid_t pid;

            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                char details[100]; 

                if (WIFEXITED(status)) {
                    // Child exited normaly
                    snprintf(details, sizeof(details), "Child process %d terminated normally (code %d)", pid, WEXITSTATUS(status));
                    log_event(INFO, "SYSTEM", "CHILD_EXIT", details);
                }
                else if (WIFSIGNALED(status)) {
                    // Child exited by signal
                    snprintf(details, sizeof(details), "Child process %d killed by signal %d", pid, WTERMSIG(status));
                    log_event(ERROR, "SYSTEM", "CHILD_KILLED", details);            
                }
            }
        }
        // --- Main logic ---

    }

    // --- Shutdown sequence ---
    log_event(INFO, "SYSTEM", "SHUTDOWN", "Initiating system shutdown");

    return 0;
}