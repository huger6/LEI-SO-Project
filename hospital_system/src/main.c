#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/msg.h>
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
#include "../include/safe_threads.h"

// New headers
#include "../include/scheduler.h"
#include "../include/command_handler.h"
#include "../include/manager_utils.h"

// --- FILE PATHS ---

#define CONFIG_PATH     "config/config.cfg"         // config.txt 
#define LOGS_PATH       "logs/hospital_log.log"     // hospital_logs.txt

// --- Function Headers ---
extern void triage_main(void);
extern void surgery_main(void);
extern void pharmacy_main(void);
extern void lab_main(void);


// Child PIDs
pid_t pid_triage, pid_surgery, pid_pharmacy, pid_lab;

// Flags
volatile sig_atomic_t g_shutdown = 0;

// Notification Monitor Thread
pthread_t t_notification_monitor;

// Manager operation_id base (used when Manager sends requests)
// Triage uses 1000-1002, Surgery uses surgery_id, Manager uses 2000+
#define MANAGER_OPERATION_ID_BASE 2000

// --- Notification Monitor Thread ---
// Listens for feedback messages from child processes on mq_responses_id
// Only receives messages with mtype >= MANAGER_OPERATION_ID_BASE (Manager's own requests)
// Triage messages (mtype 1000-1002) are received by Triage's dispatcher

void *notification_monitor(void *arg) {
    (void)arg;
    
    // Buffer large enough for any response message
    union {
        msg_header_t hdr;
        char buffer[512];
    } msg;
    
    while (!check_shutdown()) {
        memset(&msg, 0, sizeof(msg));
        int result = receive_specific_message(mq_responses_id, &msg, sizeof(msg), 
                                              MANAGER_OPERATION_ID_BASE);
        
        if (result == -1) {
            if (check_shutdown()) break;
            continue;
        }
        
        // Check for shutdown message
        if (msg.hdr.kind == MSG_SHUTDOWN) {
            break;
        }
        
        // Log meaningful feedback only
        char log_msg[128];
        switch (msg.hdr.kind) {
            case MSG_PHARM_READY:
                snprintf(log_msg, sizeof(log_msg), "Pharmacy ready for patient %s", msg.hdr.patient_id);
                log_event(INFO, "PHARMACY", "READY", log_msg);
                break;
                
            case MSG_LAB_RESULTS_READY:
                snprintf(log_msg, sizeof(log_msg), "Lab results ready for patient %s", msg.hdr.patient_id);
                log_event(INFO, "LAB", "RESULTS_READY", log_msg);
                break;
                
            case MSG_CRITICAL_STATUS:
                snprintf(log_msg, sizeof(log_msg), "Critical status update for patient %s", msg.hdr.patient_id);
                log_event(WARNING, "MANAGER", "CRITICAL", log_msg);
                break;
                
            default:
                #ifdef DEBUG
                snprintf(log_msg, sizeof(log_msg), 
                         "Feedback (kind: %d) for patient %s", msg.hdr.kind, msg.hdr.patient_id);
                log_event(DEBUG_LOG, "MANAGER", "FEEDBACK", log_msg);
                #endif
                break;
        }
    }
    
    return NULL;
}

// --- Main ---

int main(void) {
    // --- Initialize default system configs ---
    if (init_default_config() == -1) {
        exit(EXIT_FAILURE);
    }

    // --- Initialize logging system ---
    if (init_logging(LOGS_PATH) == 0) {
        log_event(INFO, "SYSTEM", "STARTUP", "Hospital system starting");
    } else {
        cleanup_config();
        exit(EXIT_FAILURE);
    }

    // --- Load config.txt ---
    if (load_config(CONFIG_PATH) != 0) {
        log_event(ERROR, "CONFIG", "LOAD_FAILED", "Invalid configuration file");
        cleanup_config();
        exit(EXIT_FAILURE);
    }

    // --- @IPC ---

    // --- Message Queues ---
    if (create_all_message_queues() == -1) {
        (void)remove_all_message_queues();
        cleanup_config();
        exit(EXIT_FAILURE);
    }

    // --- SHM ---
    if (init_all_shm() != 0) {
        cleanup_all_shm();
        (void)remove_all_message_queues();
        cleanup_config();
        exit(EXIT_FAILURE);
    }
    init_all_shm_data(config); 

    // Update critical logger ptr
    set_critical_log_shm_ptr(shm_hospital->shm_critical_logger);

    // --- Pipes ---
    if (init_pipes() != 0) {
        log_event(ERROR, "SYSTEM", "INIT_FAIL", "Failed to initialize communication pipes");
        cleanup_all_shm();
        (void)remove_all_message_queues();
        cleanup_config();
        exit(EXIT_FAILURE);
    }

    // --- Semaphores ---
    if (init_all_semaphores() != 0) {
        log_event(ERROR, "SYSTEM", "INIT_FAIL", "Failed to initialize semaphores");
        cleanup_all_shm();
        remove_all_message_queues();
        cleanup_pipes();
        cleanup_config();
        exit(EXIT_FAILURE);
    }

    log_event(INFO, "SYSTEM", "READY", "System initialized successfully");

    // --- Signal handlers ---
    setup_signal_handlers();

    // --- Forks ---

    // Triage
    pid_triage = fork();
    if (pid_triage == 0) {
        triage_main();
    } 
    else if (pid_triage < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to start triage process");
        set_shutdown();
    }

    // Surgery
    pid_surgery = fork();
    if (pid_surgery == 0) {
        surgery_main();
    }
    else if (pid_surgery < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to start surgery process");
        set_shutdown();
    }

    // Pharmacy
    pid_pharmacy = fork();
    if (pid_pharmacy == 0) {
        pharmacy_main();
    } 
    else if (pid_pharmacy < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to start pharmacy process");
        set_shutdown();
    }

    // Lab
    pid_lab = fork();
    if (pid_lab == 0) {
        lab_main();
    }
    else if (pid_lab < 0) {
        log_event(ERROR, "SYSTEM", "FORK_FAIL", "Failed to start laboratory process");
        set_shutdown();
    }

    if (!check_shutdown()) log_event(INFO, "SYSTEM", "RUNNING", "All modules started successfully");

    // --- Manager Process Setup ---

    // --- Start Notification Monitor Thread ---
    if (safe_pthread_create(&t_notification_monitor, NULL, notification_monitor, NULL) != 0) {
        log_event(ERROR, "MANAGER", "THREAD_FAIL", "Failed to create notification monitor");
    }

    // --- Main loop ---
    
    fd_set readfds;
    int max_fd;
    int fd_input = get_input_pipe_fd();
    int fd_signal = get_signal_read_fd();
    int fd_stdin = STDIN_FILENO;

    // Time management
    struct timespec last_real_time, now;
    clock_gettime(CLOCK_MONOTONIC, &last_real_time);
    long accumulated_ms = 0;
    int current_logical_time = 0;

    // Stdin line buffer
    char stdin_buf[512];
    size_t stdin_pos = 0;

    while (!check_shutdown()) {
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
        FD_SET(fd_stdin, &readfds);  // Always listen to stdin
        if (fd_input != -1) FD_SET(fd_input, &readfds);
        FD_SET(fd_signal, &readfds);

        max_fd = fd_signal;
        if (fd_stdin > max_fd) max_fd = fd_stdin;
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

            // Update global clock time
            safe_pthread_mutex_lock(&shm_hospital->shm_stats->mutex);
            shm_hospital->shm_stats->simulation_time_units = current_logical_time;
            safe_pthread_mutex_unlock(&shm_hospital->shm_stats->mutex);

            #ifdef DEBUG
                char tick_msg[50];
                snprintf(tick_msg, sizeof(tick_msg), "Tick: %d", current_logical_time);
                log_event(DEBUG_LOG, "SCHEDULER", "TICK", tick_msg);
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
                        {
                            log_event(INFO, "SYSTEM", "SIGINT", "Shutdown signal received");
                            set_shutdown();

                            // --- Broadcast SHUTDOWN message ---
                            poison_pill_triage();
                            poison_pill_surgery();
                        }
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
                            // Mark pid as waited
                            if (pid == pid_triage) {
                                pid_triage = -1;
                            } else if (pid == pid_surgery) {
                                pid_surgery = -1;
                            } else if (pid == pid_pharmacy) {
                                pid_pharmacy = -1;
                            } else if (pid == pid_lab) {
                                pid_lab = -1;
                            }

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

        // --- Console commands (from named pipe) ---
        if (fd_input != -1 && FD_ISSET(fd_input, &readfds)) {
            char cmd_buf[256];
            int status;
            
            // Process all available complete lines
            while ((status = read_input_line(cmd_buf, sizeof(cmd_buf))) == 0) {
                handle_command(cmd_buf, current_logical_time);
            }
            // status == 1 means no more complete lines available
            // status == -1 would be an error, but we keep the pipe open
        }

        // --- Console commands (from stdin / terminal) ---
        if (FD_ISSET(fd_stdin, &readfds)) {
            char temp[256];
            ssize_t n = read(fd_stdin, temp, sizeof(temp));
            
            if (n > 0) {
                // Append to stdin buffer
                for (ssize_t i = 0; i < n && stdin_pos < sizeof(stdin_buf) - 1; i++) {
                    char ch = temp[i];
                    
                    if (ch == '\n') {
                        // Complete line - process it
                        stdin_buf[stdin_pos] = '\0';
                        if (stdin_pos > 0) {
                            handle_command(stdin_buf, current_logical_time);
                        }
                        stdin_pos = 0;
                    } else {
                        stdin_buf[stdin_pos++] = ch;
                    }
                }
            }
            // EOF or error on stdin - continue running (pipe input still works)
        }
    }

    // --- Shutdown sequence ---
    
    // Poison pill for notification_monitor thread
    msg_header_t shutdown_msg;
    memset(&shutdown_msg, 0, sizeof(shutdown_msg));
    shutdown_msg.mtype = MANAGER_OPERATION_ID_BASE;
    shutdown_msg.kind = MSG_SHUTDOWN;
    shutdown_msg.timestamp = time(NULL);
    strcpy(shutdown_msg.patient_id, "SYSTEM");
    send_generic_message(mq_responses_id, &shutdown_msg, sizeof(msg_header_t));
    
    // Wait for the notification monitor thread to finish
    safe_pthread_join(t_notification_monitor, NULL);
    
    manager_cleanup();

    return 0;
}
