#ifndef MANAGER_UTILS_H
#define MANAGER_UTILS_H

#include <signal.h>
#include <sys/types.h>

extern pid_t pid_console_input;
extern volatile sig_atomic_t g_stop_child;

void setup_signal_handlers(void);
void setup_child_signals(void);
int validate_patient_id(const char *id);

void child_cleanup();
void manager_cleanup();

#endif
