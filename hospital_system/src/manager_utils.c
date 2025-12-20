#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stddef.h>
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
