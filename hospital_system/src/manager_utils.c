#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>

#include "../include/manager_utils.h"
#include "../include/pipes.h"

// Generic handler that writes to the self-pipe
static void generic_signal_handler(int sig) {
    notify_manager_from_signal(sig);
}

// Setup all signal handlers
void setup_signal_handlers(void) {
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

// Returns 1 if valid, 0 if invalid
int validate_patient_id(const char *id) {
    if (!id) return 0;
    
    size_t len = strlen(id);

    // 1. Check strict length constraint from PDF (5-15 chars)
    if (len < 5 || len > 15) return 0;

    // 2. Check for "PAC" prefix (Optional, based on your preference)
    if (strncmp(id, "PAC", 3) != 0) return 0;

    // 3. Check that the rest are digits
    for (size_t i = 3; i < len; i++) {
        if (!isdigit((unsigned char)id[i])) return 0;
    }

    return 1;
}
