#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../include/pipes.h"
#include "../include/log.h"

// --- Static File Descriptors ---

// Signal self-pipe (anonymous pipe for signal handler -> main loop communication)
static int signal_pipe[2] = {-1, -1};  // [0]=read, [1]=write

// Input named pipe (FIFO) file descriptor
static int input_pipe_fd = -1;

// Flag to track if we created the named pipe (for cleanup)
static int input_pipe_created = 0;


// --- Initialization ---

int init_pipes(void) {
    char log_msg[256];

    // 1. Create the signal self-pipe (anonymous pipe)
    if (pipe(signal_pipe) == -1) {
        snprintf(log_msg, sizeof(log_msg), 
                 "Failed to create signal self-pipe: %s", strerror(errno));
        log_event(ERROR, "IPC", "PIPE_FAIL", log_msg);
        return -1;
    }

    // Set signal pipe to non-blocking for the write end (signal handler safety)
    int flags = fcntl(signal_pipe[1], F_GETFL, 0);
    if (flags != -1) {
        fcntl(signal_pipe[1], F_SETFL, flags | O_NONBLOCK);
    }

    // 2. Create the named input pipe (FIFO)
    // Remove existing pipe if present (stale from previous run)
    unlink(INPUT_PIPE_PATH);

    if (mkfifo(INPUT_PIPE_PATH, 0666) == -1) {
        snprintf(log_msg, sizeof(log_msg), 
                 "Failed to create input pipe: %s", strerror(errno));
        log_event(ERROR, "IPC", "PIPE_FAIL", log_msg);
        
        // Cleanup signal pipe on failure
        close(signal_pipe[0]);
        close(signal_pipe[1]);
        signal_pipe[0] = signal_pipe[1] = -1;
        return -1;
    }
    input_pipe_created = 1;

    // Open the FIFO for reading (non-blocking initially to avoid blocking on open)
    // We use O_RDWR to prevent EOF when no writers are connected
    input_pipe_fd = open(INPUT_PIPE_PATH, O_RDWR | O_NONBLOCK);
    if (input_pipe_fd == -1) {
        snprintf(log_msg, sizeof(log_msg), 
                 "Failed to open input pipe: %s", strerror(errno));
        log_event(ERROR, "IPC", "PIPE_FAIL", log_msg);
        
        unlink(INPUT_PIPE_PATH);
        input_pipe_created = 0;
        close(signal_pipe[0]);
        close(signal_pipe[1]);
        signal_pipe[0] = signal_pipe[1] = -1;
        return -1;
    }

    return 0;
}


// --- Cleanup ---

int cleanup_pipes(void) {
    int status = 0;

    // Close signal pipe
    if (signal_pipe[0] != -1) {
        if (close(signal_pipe[0]) == -1) status = -1;
        signal_pipe[0] = -1;
    }
    
    if (signal_pipe[1] != -1) {
        if (close(signal_pipe[1]) == -1) status = -1;
        signal_pipe[1] = -1;
    }

    // Close and remove named pipe
    if (input_pipe_fd != -1) {
        if (close(input_pipe_fd) == -1) status = -1;
        input_pipe_fd = -1;
    }

    if (input_pipe_created) {
        if (unlink(INPUT_PIPE_PATH) == -1 && errno != ENOENT) status = -1;
        input_pipe_created = 0;
    }

    return status;
}


// --- Signal Self-Pipe ---

int notify_signal(int signal_number) {
    if (signal_pipe[1] == -1) return -1;
    
    // Write is non-blocking, safe to call from signal handler
    ssize_t bytes_written = write(signal_pipe[1], &signal_number, sizeof(int));
    
    return (bytes_written == sizeof(int)) ? 0 : -1;
}

int get_signal_read_fd(void) {
    return signal_pipe[0];
}


// --- Input Pipe ---

int get_input_pipe_fd(void) {
    return input_pipe_fd;
}

/**
 * Read a line from the input pipe (up to newline or buffer limit)
 * This function reads available data and looks for newline.
 * Uses a static buffer to handle cases where data arrives in chunks.
 * Returns: 0 on success (complete line), 1 on no data/incomplete, -1 on error
 */
int read_input_line(char *buffer, size_t size) {
    static char internal_buf[512];
    static size_t buf_pos = 0;
    
    if (input_pipe_fd == -1 || buffer == NULL || size == 0) {
        return -1;
    }

    // First check if we already have a complete line in the buffer
    char *newline = memchr(internal_buf, '\n', buf_pos);
    if (newline != NULL) {
        size_t line_len = newline - internal_buf;
        if (line_len >= size) line_len = size - 1;
        memcpy(buffer, internal_buf, line_len);
        buffer[line_len] = '\0';
        
        // Remove the line (including newline) from internal buffer
        size_t remaining = buf_pos - (line_len + 1);
        if (remaining > 0) {
            memmove(internal_buf, newline + 1, remaining);
        }
        buf_pos = remaining;
        
        // Skip empty lines
        if (line_len == 0) {
            return 1;
        }
        return 0;
    }

    // Try to read more data into internal buffer
    size_t space_left = sizeof(internal_buf) - buf_pos - 1;
    if (space_left > 0) {
        ssize_t result = read(input_pipe_fd, internal_buf + buf_pos, space_left);
        
        if (result == -1) {
            if (errno == EINTR) {
                return 1;  // Try again later
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;  // No data available
            }
            return -1;  // Actual error
        }
        
        if (result == 0) {
            // EOF/No writers - for O_RDWR this shouldn't happen, but handle it
            return 1;
        }

        buf_pos += result;
    }

    // Check again for complete line after read
    newline = memchr(internal_buf, '\n', buf_pos);
    if (newline != NULL) {
        size_t line_len = newline - internal_buf;
        if (line_len >= size) line_len = size - 1;
        memcpy(buffer, internal_buf, line_len);
        buffer[line_len] = '\0';
        
        // Remove the line from internal buffer
        size_t remaining = buf_pos - (line_len + 1);
        if (remaining > 0) {
            memmove(internal_buf, newline + 1, remaining);
        }
        buf_pos = remaining;
        
        // Skip empty lines
        if (line_len == 0) {
            return 1;
        }
        return 0;
    }

    // No complete line yet
    return 1;
}