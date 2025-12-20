#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "../include/pipes.h"
#include "../include/log.h"


#define NUM_PIPES 6
#define READ_END 0
#define WRITE_END 1




// [PipeID][0=Read, 1=Write]
static int pipe_fds[NUM_PIPES][2];


int init_all_pipes(void) {
    int i;
    char error_msg[256];

    // Create exactly 6 Unix pipes using pipe()
    for (i = 0; i < NUM_PIPES; i++) {
        if (pipe(pipe_fds[i]) == -1) {
            snprintf(error_msg, sizeof(error_msg), 
                     "Failed to create pipe index %d: %s", i, strerror(errno));
            
            log_event(ERROR, "IPC", "PIPE_CREATION_FAIL", error_msg);
            
            // Clean up any pipes already created before failing
            int j;
            for (j = 0; j < i; j++) {
                close(pipe_fds[j][READ_END]);
                close(pipe_fds[j][WRITE_END]);
            }
            return -1;
        }
    }

    log_event(INFO, "IPC", "PIPE_INIT", "All 6 anonymous pipes initialized successfully");
    return 0;
}



void close_unused_pipe_ends(int process_role) {
    int i;
    char log_details[256];

    snprintf(log_details, sizeof(log_details), "Closing unused pipe ends for role %d", process_role);
    log_event(INFO, "IPC", "PIPE_CLEANUP", log_details);

    for (i = 0; i < NUM_PIPES; i++) {
        // Default: close both ends unless specified otherwise
        int keep_read = 0;
        int keep_write = 0;

        // Determine which ends to keep based on Role and Pipe ID
        switch (process_role) {
            case ROLE_MANAGER:
                // Manager reads from INPUT
                if (i == PIPE_INPUT) {
                    keep_read = 1;
                    keep_write = 0; // Close write to detect EOF
                }
                // Manager reads from SIGNAL (Self-pipe: must keep WRITE open for signal handler)
                else if (i == PIPE_SIGNAL) {
                    keep_read = 1;
                    keep_write = 1; 
                }
                // Manager writes to Child components
                else if (i == PIPE_TRIAGE || i == PIPE_SURGERY || 
                         i == PIPE_PHARMACY || i == PIPE_LAB) {
                    keep_read = 0; // Close read to prevent reading own messages
                    keep_write = 1;
                }
                break;

            case ROLE_TRIAGE:
                if (i == PIPE_TRIAGE) {
                    keep_read = 1; // Read commands from Manager
                    keep_write = 0; // Close write to allow EOF detection if Manager dies
                }
                break;

            case ROLE_SURGERY:
                if (i == PIPE_SURGERY) {
                    keep_read = 1;
                    keep_write = 0;
                }
                break;

            case ROLE_PHARMACY:
                if (i == PIPE_PHARMACY) {
                    keep_read = 1;
                    keep_write = 0;
                }
                break;

            case ROLE_LAB:
                if (i == PIPE_LAB) {
                    keep_read = 1;
                    keep_write = 0;
                }
                break;
        }

        // Perform closing operations
        if (!keep_read) {
            if (pipe_fds[i][READ_END] != -1) {
                if (close(pipe_fds[i][READ_END]) == -1) {
                    snprintf(log_details, sizeof(log_details), 
                             "Failed to close READ end of pipe %d: %s", i, strerror(errno));
                    log_event(WARNING, "IPC", "PIPE_CLOSE_FAIL", log_details);
                }
                pipe_fds[i][READ_END] = -1;
            }
        }

        if (!keep_write) {
            if (pipe_fds[i][WRITE_END] != -1) {
                if (close(pipe_fds[i][WRITE_END]) == -1) {
                    snprintf(log_details, sizeof(log_details), 
                             "Failed to close WRITE end of pipe %d: %s", i, strerror(errno));
                    log_event(WARNING, "IPC", "PIPE_CLOSE_FAIL", log_details);
                }
                pipe_fds[i][WRITE_END] = -1;
            }
        }
    }
}

int send_pipe_command(int pipe_id, int command) {
    // Validate pipe ID range
    if (pipe_id < 0 || pipe_id >= NUM_PIPES) {
        log_event(ERROR, "IPC", "PIPE_SEND_INVALID_ID", "Attempted to send command to invalid pipe ID");
        return -1;
    }

    // Get the write file descriptor for the target pipe
    int fd = pipe_fds[pipe_id][WRITE_END];
    
    // Check if the file descriptor is valid (open)
    if (fd < 0) {
        log_event(ERROR, "IPC", "PIPE_SEND_CLOSED", "Attempted to write to a closed pipe endpoint");
        return -1;
    }

    // Variables for handling partial writes
    size_t total_to_write = sizeof(int);
    size_t bytes_written = 0;
    const char *buffer = (const char *)&command;
    ssize_t result;
    char log_buffer[256];

    // Loop to handle partial writes and interruptions
    while (bytes_written < total_to_write) {
        result = write(fd, buffer + bytes_written, total_to_write - bytes_written);

        if (result == -1) {
            // If interrupted by a signal, retry immediately
            if (errno == EINTR) {
                continue;
            }
            
            // For broken pipes (EPIPE), the reader has closed their end
            if (errno == EPIPE) {
                 snprintf(log_buffer, sizeof(log_buffer), 
                         "Reader closed connection on pipe ID %d (EPIPE)", pipe_id);
                 log_event(ERROR, "IPC", "PIPE_BROKEN", log_buffer);
                 return -1;
            }

            // Log other write errors
            snprintf(log_buffer, sizeof(log_buffer), 
                     "Failed to write to pipe ID %d: %s", pipe_id, strerror(errno));
            log_event(ERROR, "IPC", "PIPE_WRITE_FAIL", log_buffer);
            return -1;
        }

        bytes_written += result;
    }

    return 0;
}

int read_pipe_command(int pipe_id, int *command) {
    // Validate arguments
    if (pipe_id < 0 || pipe_id >= NUM_PIPES) {
        log_event(ERROR, "IPC", "PIPE_READ_INVALID_ID", "Attempted to read from invalid pipe ID");
        return -1;
    }
    
    if (command == NULL) {
        log_event(ERROR, "IPC", "PIPE_READ_NULL_PTR", "Null pointer provided for command buffer");
        return -1;
    }

    // Get the read file descriptor
    int fd = pipe_fds[pipe_id][READ_END];

    // Check if the file descriptor is valid
    if (fd < 0) {
        log_event(ERROR, "IPC", "PIPE_READ_CLOSED", "Attempted to read from a closed pipe endpoint");
        return -1;
    }

    // Variables for handling partial reads
    size_t total_to_read = sizeof(int);
    size_t bytes_read = 0;
    char *buffer = (char *)command;
    ssize_t result;
    char log_buffer[256];

    // Loop to handle partial reads and interruptions
    while (bytes_read < total_to_read) {
        result = read(fd, buffer + bytes_read, total_to_read - bytes_read);

        if (result == -1) {
            // If interrupted by a signal, retry immediately
            if (errno == EINTR) {
                continue;
            }

            // Log read error
            snprintf(log_buffer, sizeof(log_buffer), 
                     "Failed to read from pipe ID %d: %s", pipe_id, strerror(errno));
            log_event(ERROR, "IPC", "PIPE_READ_FAIL", log_buffer);
            return -1;
        } 
        else if (result == 0) {
            // EOF detected (Writer closed the pipe)
            if (bytes_read == 0) {
                // Clean EOF: No data was pending, normal stream termination
                return 1; // Distinct value for EOF
            } else {
                // Unexpected EOF: Connection closed while reading an integer (partial data)
                snprintf(log_buffer, sizeof(log_buffer), 
                         "Unexpected EOF on pipe ID %d (Partial read: %zu/%zu bytes)", 
                         pipe_id, bytes_read, total_to_read);
                log_event(ERROR, "IPC", "PIPE_READ_PARTIAL", log_buffer);
                return -1;
            }
        }

        bytes_read += result;
    }

    return 0;
}

int send_pipe_string(int pipe_id, const char *str) {
    if (pipe_id < 0 || pipe_id >= NUM_PIPES) return -1;
    int fd = pipe_fds[pipe_id][WRITE_END];
    if (fd < 0) return -1;

    int len = (int)strlen(str) + 1; // Include null terminator
    
    // Write length
    if (write(fd, &len, sizeof(int)) != sizeof(int)) return -1;
    
    // Write string
    size_t total_written = 0;
    size_t to_write = len;
    while (total_written < to_write) {
        ssize_t res = write(fd, str + total_written, to_write - total_written);
        if (res == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        total_written += res;
    }
    return 0;
}

int read_pipe_string(int pipe_id, char *buffer, size_t size) {
    if (pipe_id < 0 || pipe_id >= NUM_PIPES) return -1;
    int fd = pipe_fds[pipe_id][READ_END];
    if (fd < 0) return -1;

    int len;
    ssize_t bytes = read(fd, &len, sizeof(int));
    if (bytes == 0) return 1; // EOF
    if (bytes == -1) return -1;
    if (bytes != sizeof(int)) return -1; // Partial read of length?

    if (len > (int)size) {
        // Buffer too small. We must consume the data to keep pipe consistent, 
        // but we can't return it all. For now, treat as error.
        // In a real system, we might discard or realloc.
        // Let's read what we can and discard the rest.
        // Actually, let's just return error but try to read it out to clear pipe?
        // Simpler: assume buffer is big enough (256+).
        return -1; 
    }

    size_t total_read = 0;
    size_t to_read = len;
    while (total_read < to_read) {
        ssize_t res = read(fd, buffer + total_read, to_read - total_read);
        if (res == 0) return 1; // Unexpected EOF
        if (res == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        total_read += res;
    }
    
    return 0;
}

int notify_manager_from_signal(int signal_number) {
    int fd = pipe_fds[PIPE_SIGNAL][WRITE_END];
    
    // Attempt to write the signal number to the pipe
    ssize_t bytes_written = write(fd, &signal_number, sizeof(int));

    // Return 0 on success (all bytes written), -1 on failure
    return (bytes_written == sizeof(int)) ? 0 : -1;
}

int destroy_all_pipes(void) {
    int i, j;
    int status = 0;
    char log_msg[256];

    for (i = 0; i < NUM_PIPES; i++) {
        for (j = 0; j < 2; j++) {
            int fd = pipe_fds[i][j];

            if (fd != -1) {
                if (close(fd) == -1) {
                    // Update global status to indicate at least one failure occurred
                    status = -1;

                    snprintf(log_msg, sizeof(log_msg), 
                             "Failed to close pipe index %d end %s (FD %d): %s", 
                             i, (j == READ_END ? "READ" : "WRITE"), fd, strerror(errno));
                    
                    log_event(WARNING, "IPC", "PIPE_DESTROY_FAIL", log_msg);
                }
                pipe_fds[i][j] = -1;
            }
        }
    }

    if (status == 0) {
        log_event(INFO, "IPC", "PIPE_DESTROY", "All pipe descriptors closed successfully");
    }

    return status;
}

int get_pipe_read_end(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= NUM_PIPES) return -1;
    return pipe_fds[pipe_id][READ_END];
}

int get_pipe_write_end(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= NUM_PIPES) return -1;
    return pipe_fds[pipe_id][WRITE_END];
}