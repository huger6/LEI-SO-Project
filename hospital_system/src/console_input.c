#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "../include/console_input.h"

#include "../include/pipes.h"
#include "../include/log.h"
#include "../include/config.h"
#include "../include/manager_utils.h"


// Helper to trim leading/trailing whitespace
static char* trim_whitespace(char* str) {
    char* end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return str;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    *(end+1) = 0;
    return str;
}

void process_console_input(void) {
    // Close all read ends
    for(int i = 0; i < 6; i++) close(get_pipe_read_end(i));
    // Close all write ends except INPUT
    for(int i = 0; i < 6; i++) {
        if (i != PIPE_INPUT) close(get_pipe_write_end(i));
    }

    char buffer[256];

    // Setup signal handlers for child process (no SA_RESTART)
    setup_child_signals();

    // Log start of input loop
    log_event(INFO, "INPUT", "START", "Console input listener started");

    // Continuous loop reading from stdin
    while (!g_stop_child) {
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            if (errno == EINTR) continue; // Interrupted by signal, check g_stop_child
            break; // EOF or error
        }
        
        // Remove trailing newline character replaced by null terminator
        buffer[strcspn(buffer, "\n")] = 0;

        // Skip empty lines
        if (strlen(buffer) == 0) {
            continue;
        }

        // Reset command code for new iteration
        // command_code = -1;

        // ---------------------------------------------------------
        // Command Transmission
        // Sends raw command string to Manager via PIPE_INPUT
        // ---------------------------------------------------------

        if (send_pipe_string(PIPE_INPUT, buffer) == -1) {
            log_event(ERROR, "INPUT", "SEND_FAIL", "Failed to send command to Manager via pipe");
        } else {
            #ifdef DEBUG
                snprintf(log_msg, sizeof(log_msg), "Sent command '%s' to Manager", buffer);
                log_event(DEBUG, "INPUT", "SENT", log_msg);
            #endif
        }
    }
    
    log_event(INFO, "INPUT", "STOP", "Console input listener shutting down");

    // Resources cleanup
    log_event(INFO, "CONSOLE_INPUT", "RESOURCES_CLEANUP", "Cleaning console input receiver resources");
    child_cleanup();

    exit(EXIT_SUCCESS);
}

int get_med_id(const char *name) {
    for (int i = 0; i < config->med_count; i++) {
        if (strcmp(config->medications[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int get_test_id(const char *name) {
    if (strcmp(name, "HEMO") == 0) return 0;
    if (strcmp(name, "GLIC") == 0) return 1;
    if (strcmp(name, "COLEST") == 0) return 2;
    if (strcmp(name, "RENAL") == 0) return 3;
    if (strcmp(name, "HEPAT") == 0) return 4;
    if (strcmp(name, "PREOP") == 0) return 5;
    return -1;
}

int get_specialty_id(const char *name) {
    if (strcmp(name, "CARDIO") == 0) return 0;
    if (strcmp(name, "ORTHO") == 0) return 1;
    if (strcmp(name, "NEURO") == 0) return 2;
    return -1;
}

int get_urgency_id(const char *name) {
    if (strcmp(name, "LOW") == 0) return 0;
    if (strcmp(name, "MEDIUM") == 0) return 1;
    if (strcmp(name, "HIGH") == 0) return 2;
    return -1; // Invalid urgency
}

int parse_list_ids(char *str, int *ids, int max_count, int (*map_func)(const char*)) {
    if (!str || str[0] != '[') return 0;
    char *content = str + 1;
    char *end = strchr(content, ']');
    if (end) *end = '\0';
    
    int count = 0;
    char *token = strtok(content, ",");
    while (token && count < max_count) {
        int id = map_func(trim_whitespace(token));
        if (id != -1) {
            ids[count++] = id;
        }
        token = strtok(NULL, ",");
    }
    return count;
}

int parse_med_qty_list(char *str, int *ids, int *qtys, int max_count) {
    if (!str || str[0] != '[') return 0;
    char *content = str + 1;
    char *end = strchr(content, ']');
    if (end) *end = '\0';
    
    int count = 0;
    char *token = strtok(content, ",");
    while (token && count < max_count) {
        char *colon = strchr(token, ':');
        if (colon) {
            *colon = '\0';
            char *name = token;
            int qty = atoi(colon + 1);
            int id = get_med_id(name);
            if (id != -1) {
                ids[count] = id;
                qtys[count] = qty;
                count++;
            }
        }
        token = strtok(NULL, ",");
    }
    return count;
}


