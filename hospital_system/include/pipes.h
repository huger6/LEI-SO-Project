#ifndef PIPE_H
#define PIPE_H

#include <stddef.h>

// Named pipe path for external command input
#define INPUT_PIPE_PATH "input_pipe"

// Process roles (for documentation, no longer used for pipe management)
typedef enum {
    ROLE_MANAGER,           // The Central Manager Process
    ROLE_TRIAGE,            // Triage Center
    ROLE_SURGERY,           // Operating Blocks 
    ROLE_PHARMACY,          // Central Pharmacy
    ROLE_LAB                // Laboratories
} ProcessRole;


// --- Function Headers ---

// Initialize the named input pipe (FIFO) and signal self-pipe
int init_pipes(void);

// Cleanup: close file descriptors and remove named pipe
int cleanup_pipes(void);

// Signal self-pipe: used by signal handlers to notify main loop
int notify_signal(int signal_number);
int get_signal_read_fd(void);

// Input pipe: reading commands from the named pipe
int get_input_pipe_fd(void);
int read_input_line(char *buffer, size_t size);


#endif