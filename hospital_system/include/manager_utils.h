#ifndef MANAGER_UTILS_H
#define MANAGER_UTILS_H

#include <signal.h>
#include <sys/types.h>

extern pid_t pid_console_input;
extern pid_t pid_triage, pid_surgery, pid_pharmacy, pid_lab;

extern volatile sig_atomic_t g_stop_child;

int check_shutdown();
void set_shutdown();

void setup_signal_handlers(void);
void setup_child_signals(void);
int validate_patient_id(const char *id);

void child_cleanup();
void manager_cleanup();

void shutdown_triage();

#endif
