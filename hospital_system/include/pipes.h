#ifndef PIPE_H
#define PIPE_H


typedef enum {
    PIPE_INPUT,
    PIPE_SIGNAL,
    PIPE_TRIAGE,
    PIPE_SURGERY,
    PIPE_PHARMACY,
    PIPE_LAB
} PipeID;


typedef enum {
    PIPE_CMD_SHUTDOWN,      // Graceful shutdown request
    PIPE_CMD_PAUSE,         // Pause operations
    PIPE_CMD_RESUME,        // Resume operations
    PIPE_CMD_STATUS         // Request status update to be written to SHM
} PipeCommand;


typedef enum {
    ROLE_MANAGER,           // The Central Manager Process
    ROLE_TRIAGE,            // Triage Center
    ROLE_SURGERY,           // Operating Blocks 
    ROLE_PHARMACY,          // Central Pharmacy
    ROLE_LAB                // Laboratories
} ProcessRole;


// --- Function Headers ---


int init_all_pipes(void);
void close_unused_pipe_ends(int process_role);
int send_pipe_command(int pipe_id, int command);
int read_pipe_command(int pipe_id, int *command);
int destroy_all_pipes(void);
int notify_manager_from_signal(int signal_number);
int get_pipe_read_end(int pipe_id);
int get_pipe_write_end(int pipe_id);


#endif