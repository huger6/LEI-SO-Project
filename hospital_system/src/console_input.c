#include <stdio.h>
#include <string.h>
#include <strings.h> // For strcasecmp

#include "../include/pipes.h"
#include "../include/log.h"

/**
 * @file console_input.c
 * @brief Handles reading commands from Standard Input (stdin).
 * Parses textual user commands and converts them to internal PIPE_CMD_* integer values.
 * Sends valid commands to the Manager process via PIPE_INPUT.
 * References PDF Section 2.1 (Command Input).
 */

void process_console_input(void) {
    char buffer[256];
    int command_code;
    char log_msg[300];

    // Log start of input loop
    log_event(INFO, "INPUT", "START", "Console input listener started");

    // Continuous loop reading from stdin
    while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        
        // Remove trailing newline character replaced by null terminator
        buffer[strcspn(buffer, "\n")] = 0;

        // Skip empty lines
        if (strlen(buffer) == 0) {
            continue;
        }

        // Reset command code for new iteration
        command_code = -1;

        // ---------------------------------------------------------
        // Command Parsing Logic
        // Maps textual commands to PIPE_CMD_* enums defined in pipes.h
        // ---------------------------------------------------------
        
        if (strcasecmp(buffer, "SHUTDOWN") == 0) {
            command_code = PIPE_CMD_SHUTDOWN;
        } 
        else if (strcasecmp(buffer, "PAUSE") == 0) {
            command_code = PIPE_CMD_PAUSE;
        } 
        else if (strcasecmp(buffer, "RESUME") == 0) {
            command_code = PIPE_CMD_RESUME;
        } 
        else if (strcasecmp(buffer, "STATUS") == 0) {
            command_code = PIPE_CMD_STATUS;
        } 
        else {
            // Handle unrecognized commands
            snprintf(log_msg, sizeof(log_msg), "Unrecognized console command: '%s'", buffer);
            log_event(WARNING, "INPUT", "INVALID_CMD", log_msg);
            continue;
        }

        // ---------------------------------------------------------
        // Command Transmission
        // Uses the transport mechanism from pipes.c
        // ---------------------------------------------------------

        if (command_code != -1) {
            // Send to PIPE_INPUT (corresponds to Manager's input pipe)
            // Note: send_pipe_command handles the low-level write() and locking if needed
            if (send_pipe_command(PIPE_INPUT, command_code) == -1) {
                log_event(ERROR, "INPUT", "SEND_FAIL", "Failed to send command to Manager via pipe");
            } else {
                // Optional: Debug log for successful send
                #ifdef DEBUG
                    snprintf(log_msg, sizeof(log_msg), "Sent command code %d to Manager", command_code);
                    log_event(DEBUG, "INPUT", "SENT", log_msg);
                #endif
            }
            
            // If shutdown was requested, we might want to break the loop 
            // depending on if this process should terminate immediately or wait for signals.
            // For now, we continue processing until the pipe is closed or process killed.
        }
    }
    
    log_event(INFO, "INPUT", "STOP", "Console input listener received EOF");
}