#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/select.h> // Added for select()

// IPC
#include "../include/mq.h"
#include "../include/shm.h"
#include "../include/sem.h"
#include "../include/pipes.h"

#include "../include/config.h"
#include "../include/hospital.h"
#include "../include/stats.h" 
#include "../include/log.h"

// --- FILE PATHS ---

#define CONFIG_PATH     "config/config.txt"         // config.txt 
#define LOGS_PATH       "logs/hospital_log.txt"     // hospital_logs.txt


// Child PIDs
pid_t pid_triage, pid_surgery, pid_pharmacy, pid_lab;
pid_t pid_console_input; // Added for console input process

// FLags
volatile sig_atomic_t g_shutdown = 0;

// --- Signal Handlers ---

// Generic handler that writes to the self-pipe
void generic_signal_handler(int sig) {
    notify_manager_from_signal(sig);
}

// Setup all signal handlers
void setup_signal_handlers() {
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

// Prototype for console input function (from src/console_input.c)
extern void process_console_input(void);

// --- Main ---

int main(void) {
    // --- Initialize default system configs ---
    if (init_default_config() == -1) {
        return -1;
    }

    // --- Initialize logging system ---
    init_logging(LOGS_PATH);
    log_event(INFO, "SYSTEM", "STARTUP", "Hospital system starting");

    // --- Load config.txt ---
    if (load_config(CONFIG_PATH) != 0) {
        log_event(ERROR, "CONFIG", "LOAD_FAILED", "Invalid config.txt file");
        return -1;
    }
    log_event(INFO, "CONFIG", "LOADED", "Configuration loaded sucessfully");

    // Console Output for Debugging
    #ifdef DEBUG
        print_configs();
    #endif

    // --- @IPC ---

    log_event(INFO, "IPC", "INIT", "Starting IPC mechanisms");

    // --- Message Queues ---
    if (create_all_message_queues() == -1) {
        (void)remove_all_message_queues();
        return -1;
    }

    // --- SHM ---
    if (init_all_shm() != 0) {
        cleanup_all_shm();
        (void)remove_all_message_queues();
        exit(EXIT_FAILURE);
    }
    init_all_shm_data(config); 

    // Update critical logger ptr
    set_critical_log_shm_ptr(shm_hospital->shm_critical_logger);

    // --- Pipes ---
    if (init_all_pipes() != 0) {
        log_event(ERROR, "IPC", "PIPE_INIT_FAIL", "Failed to initialize pipes");
        // Cleanup and exit
        cleanup_all_shm();
        remove_all_message_queues();
        return -1;
    }

    // --- Semaphores ---
    if (init_all_semaphores() != 0) {
        log_event(ERROR, "IPC", "SEM_INIT_FAIL", "Failed to initialize semaphores");
        cleanup_all_shm();
        remove_all_message_queues();
        destroy_all_pipes();
        return -1;
    }

    // --- Signal handlers ---
    // Setup after pipes are initialized so notify_manager_from_signal works
    setup_signal_handlers();

    // --- Fork ---

    pid_console_input = fork();
    if (pid_console_input == 0) {
        // Child: Console Input
        // Close unused pipe ends for Manager role? No, this is a helper.
        // It only needs to WRITE to PIPE_INPUT.
        // But close_unused_pipe_ends doesn't have a ROLE_CONSOLE.
        // We can manually close or define a role. For now, let's just run it.
        // Ideally, we should close everything except PIPE_INPUT write end.
        
        // Close all read ends
        for(int i=0; i<6; i++) close(get_pipe_read_end(i));
        // Close all write ends except INPUT
        for(int i=0; i<6; i++) {
            if (i != PIPE_INPUT) close(get_pipe_write_end(i));
        }

        process_console_input();
        exit(0);
    } else if (pid_console_input < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to fork console input process");
        g_shutdown = 1;
    }

    // --- Manager Process Setup ---
    close_unused_pipe_ends(ROLE_MANAGER);

    // --- Main loop ---
    
    fd_set readfds;
    int max_fd;
    int fd_input = get_pipe_read_end(PIPE_INPUT);
    int fd_signal = get_pipe_read_end(PIPE_SIGNAL);

    while(!g_shutdown) {
        FD_ZERO(&readfds);
        FD_SET(fd_input, &readfds);
        FD_SET(fd_signal, &readfds);

        max_fd = (fd_input > fd_signal) ? fd_input : fd_signal;

        // Wait for activity
        if (select(max_fd + 1, &readfds, NULL, NULL, NULL) == -1) {
            if (errno == EINTR) continue;
            log_event(ERROR, "SYSTEM", "SELECT_FAIL", strerror(errno));
            break;
        }

        // Check for Signals
        if (FD_ISSET(fd_signal, &readfds)) {
            int sig;
            if (read(fd_signal, &sig, sizeof(int)) > 0) {
                switch(sig) {
                    case SIGINT:
                        log_event(INFO, "SYSTEM", "SIGINT", "Shutdown signal received");
                        g_shutdown = 1;
                        break;
                    case SIGUSR1:
                        display_statistics_console(shm_hospital->shm_stats);
                        break;
                    case SIGUSR2:
                        save_statistics_snapshot(shm_hospital->shm_stats);
                        break;
                    case SIGCHLD:
                        {
                            int status;
                            pid_t pid;
                            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                                char details[100]; 
                                if (WIFEXITED(status)) {
                                    snprintf(details, sizeof(details), "Child %d exited (code %d)", pid, WEXITSTATUS(status));
                                    log_event(INFO, "SYSTEM", "CHILD_EXIT", details);
                                } else if (WIFSIGNALED(status)) {
                                    snprintf(details, sizeof(details), "Child %d killed (sig %d)", pid, WTERMSIG(status));
                                    log_event(ERROR, "SYSTEM", "CHILD_KILLED", details);            
                                }
                            }
                        }
                        break;
                }
            }
        }

        // Check for Console Commands
        if (FD_ISSET(fd_input, &readfds)) {
            int cmd;
            // read_pipe_command returns 0 on success, 1 on EOF, -1 on error
            int status = read_pipe_command(PIPE_INPUT, &cmd);
            if (status == 0) { 
                switch(cmd) {
                    case PIPE_CMD_SHUTDOWN:
                        g_shutdown = 1;
                        break;
                    case PIPE_CMD_STATUS:
                        display_statistics_console(shm_hospital->shm_stats);
                        break;
                    // Add other commands as needed
                }
            } else if (status == 1) {
                // EOF detected
                log_event(INFO, "SYSTEM", "PIPE_EOF", "Console input pipe closed");
                // Remove from select set? Or just shutdown?
                // For now, maybe just log it.
            }
        }
    }

    // --- Shutdown sequence ---
    log_event(INFO, "SYSTEM", "SHUTDOWN", "Initiating system shutdown");
    
    // Kill children if needed
    if (pid_console_input > 0) {
        kill(pid_console_input, SIGTERM);
        waitpid(pid_console_input, NULL, 0);
    }
    
    cleanup_all_shm();
    remove_all_message_queues();
    destroy_all_pipes();
    destroy_all_semaphores();

    return 0;
}