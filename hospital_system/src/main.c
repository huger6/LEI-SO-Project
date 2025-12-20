#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>

#include "../include/mq.h"
#include "../include/shm.h"
#include "../include/sem.h"
#include "../include/pipes.h"

#include "../include/config.h"
#include "../include/hospital.h"
#include "../include/stats.h" 
#include "../include/log.h"
#include "../include/console_input.h"

// New headers
#include "../include/scheduler.h"
#include "../include/command_handler.h"
#include "../include/manager_utils.h"

// --- FILE PATHS ---

#define CONFIG_PATH     "config/config.txt"         // config.txt 
#define LOGS_PATH       "logs/hospital_log.txt"     // hospital_logs.txt

// --- Function Headers ---
extern void triage_main(void);
extern void surgery_main(void);
extern void pharmacy_main(void);
extern void lab_main(void);


// Child PIDs
pid_t pid_triage, pid_surgery, pid_pharmacy, pid_lab;
pid_t pid_console_input; 

// FLags
volatile sig_atomic_t g_shutdown = 0;

// --- Main ---

int main(void) {
    // --- Initialize default system configs ---
    if (init_default_config() == -1) {
        exit(EXIT_FAILURE);
    }

    // --- Initialize logging system ---
    init_logging(LOGS_PATH);
    log_event(INFO, "SYSTEM", "STARTUP", "Hospital system starting");

    // --- Load config.txt ---
    if (load_config(CONFIG_PATH) != 0) {
        log_event(ERROR, "CONFIG", "LOAD_FAILED", "Invalid config.txt file");
        exit(EXIT_FAILURE);
    }
    log_event(INFO, "CONFIG", "LOADED", "Configuration loaded sucessfully");

    // Console Output for Debugging
    #ifdef DEBUG
        print_configs();
    #endif

    // --- @IPC ---

    log_event(INFO, "SYSTEM", "IPC_INIT", "Starting IPC mechanisms");

    // --- Message Queues ---
    if (create_all_message_queues() == -1) {
        (void)remove_all_message_queues();
        exit(EXIT_FAILURE);
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
        exit(EXIT_FAILURE);
    }

    // --- Semaphores ---
    if (init_all_semaphores() != 0) {
        log_event(ERROR, "IPC", "SEM_INIT_FAIL", "Failed to initialize semaphores");
        cleanup_all_shm();
        remove_all_message_queues();
        destroy_all_pipes();
        exit(EXIT_FAILURE);
    }

    log_event(INFO, "SYSTEM", "IPC_INIT", "All IPC mechanisms initialized successfully");

    // --- Signal handlers ---
    // Setup after pipes are initialized so notify_manager_from_signal() works
    setup_signal_handlers();

    log_event(INFO, "SYSTEM", "SIGNALS", "Signal handlers setup initialized successfully");

    // --- Forks ---

    log_event(INFO, "SYSTEM", "FORK", "Starting system fork() calls");

    // Input
    pid_console_input = fork();
    if (pid_console_input == 0) {
        // Child: Console Input
        // Close all read ends
        for(int i = 0; i < 6; i++) close(get_pipe_read_end(i));
        // Close all write ends except INPUT
        for(int i = 0; i < 6; i++) {
            if (i != PIPE_INPUT) close(get_pipe_write_end(i));
        }

        process_console_input();
        exit(EXIT_SUCCESS);
    } 
    else if (pid_console_input < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to fork console input process");
        g_shutdown = 1;
    }

    // Triage
    pid_triage = fork();
    if (pid_triage == 0) {
        close_unused_pipe_ends(ROLE_TRIAGE);
        triage_main();
        exit(EXIT_SUCCESS);
    } 
    else if (pid_triage < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to fork triage process");
        g_shutdown = 1;
    }

    // Surgery
    pid_surgery = fork();
    if (pid_surgery == 0) {
        close_unused_pipe_ends(ROLE_SURGERY);
        surgery_main();
        exit(EXIT_SUCCESS);
    }
    else if (pid_surgery < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to fork surgery process");
        g_shutdown = 1;
    }

    // Pharmacy
    pid_pharmacy = fork();
    if (pid_pharmacy == 0) {
        close_unused_pipe_ends(ROLE_PHARMACY);
        pharmacy_main();
        exit(EXIT_SUCCESS);
    } 
    else if (pid_pharmacy < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to fork pharmacy process");
        g_shutdown = 1;
    }

    // Lab
    pid_lab = fork();
    if (pid_lab == 0) {
        close_unused_pipe_ends(ROLE_LAB);
        lab_main();
        exit(EXIT_SUCCESS);
    }
    else if (pid_lab < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to fork laboratory process");
        g_shutdown = 1;
    }

    if (!g_shutdown) log_event(INFO, "SYSTEM", "FORK", "All forks were successful");

    // --- Manager Process Setup ---

    close_unused_pipe_ends(ROLE_MANAGER);

    // --- Main loop ---
    
    fd_set readfds;
    int max_fd;
    int fd_input = get_pipe_read_end(PIPE_INPUT);
    int fd_signal = get_pipe_read_end(PIPE_SIGNAL);

    // Time management
    struct timespec last_real_time, now;
    clock_gettime(CLOCK_MONOTONIC, &last_real_time);
    long accumulated_ms = 0;
    int current_logical_time = 0;

    while (!g_shutdown) {
        // 1. Calculate Timeout
        struct timeval timeout;
        struct timeval *timeout_ptr = NULL;

        if (has_scheduled_events()) {
            int next_event_time = get_next_scheduled_time();
            long ms_until_next = 0;
            
            if (next_event_time > current_logical_time) {
                int ticks_needed = next_event_time - current_logical_time;
                ms_until_next = (long)ticks_needed * config->time_unit_ms - accumulated_ms;
                if (ms_until_next < 0) ms_until_next = 0;
            } else {
                ms_until_next = 0;
            }

            timeout.tv_sec = ms_until_next / 1000;
            timeout.tv_usec = (ms_until_next % 1000) * 1000;
            timeout_ptr = &timeout;
        } else {
            timeout_ptr = NULL; // Block indefinitely
        }

        FD_ZERO(&readfds);
        if (fd_input != -1) FD_SET(fd_input, &readfds);
        FD_SET(fd_signal, &readfds);

        max_fd = fd_signal;
        if (fd_input != -1 && fd_input > max_fd) max_fd = fd_input;

        // 2. Select
        int ret = select(max_fd + 1, &readfds, NULL, NULL, timeout_ptr);

        // 3. Update Clock
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - last_real_time.tv_sec) * 1000 + 
                          (now.tv_nsec - last_real_time.tv_nsec) / 1000000;
        last_real_time = now;
        
        accumulated_ms += elapsed_ms;
        
        int ticks_passed = accumulated_ms / config->time_unit_ms;
        if (ticks_passed > 0) {
            current_logical_time += ticks_passed;
            accumulated_ms %= config->time_unit_ms;
            
            #ifdef DEBUG
            char tick_msg[50];
            snprintf(tick_msg, sizeof(tick_msg), "Tick: %d", current_logical_time);
            log_event(DEBUG, "SCHEDULER", "TICK", tick_msg);
            #endif
        }

        if (ret == -1) {
            if (errno == EINTR) continue;
            log_event(ERROR, "SYSTEM", "SELECT_FAIL", strerror(errno));
            break;
        }

        // 4. Process Due Events
        process_scheduled_events(current_logical_time);

        // --- Signals ---
        if (FD_ISSET(fd_signal, &readfds)) {
            int sig;
            if (read(fd_signal, &sig, sizeof(int)) > 0) {
                switch (sig) {
                    case SIGINT:
                        log_event(INFO, "SYSTEM", "SIGINT", "Shutdown signal received");
                        g_shutdown = 1;
                        break;

                    case SIGUSR1:
                        display_statistics_console(shm_hospital->shm_stats, NULL);
                        break;

                    case SIGUSR2:
                        save_statistics_snapshot(shm_hospital->shm_stats);
                        break;

                    case SIGCHLD:
                        int status;
                        pid_t pid;
                        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                            char details[100];
                            if (WIFEXITED(status)) {
                                snprintf(details, sizeof(details),
                                        "Child %d exited (code %d)",
                                        pid, WEXITSTATUS(status));
                                log_event(INFO, "SYSTEM", "CHILD_EXIT", details);
                            } 
                            else if (WIFSIGNALED(status)) {
                                snprintf(details, sizeof(details),
                                        "Child %d killed (sig %d)",
                                        pid, WTERMSIG(status));
                                log_event(ERROR, "SYSTEM", "CHILD_KILLED", details);
                            }
                        }
                        break;
                }
            }
        }

        // --- Console commands ---
        if (fd_input != -1 && FD_ISSET(fd_input, &readfds)) {
            char cmd_buf[256];
            int status = read_pipe_string(PIPE_INPUT, cmd_buf, sizeof(cmd_buf));

            if (status == 0) {
                handle_command(cmd_buf, current_logical_time);
            }
            else if (status == 1) {
                // EOF detected
                log_event(INFO, "SYSTEM", "PIPE_EOF", "Console input pipe closed");

                close(fd_input);
                fd_input = -1;   // remove from select()
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
    
    // Disable SHM logging before destroying SHM
    set_critical_log_shm_ptr(NULL);

    cleanup_all_shm();
    remove_all_message_queues();
    destroy_all_pipes();
    destroy_all_semaphores();

    log_event(INFO, "SYSTEM", "SHUTDOWN", "Shutdown was successful. Goodbye");

    close_logging();

    return 0;
}
