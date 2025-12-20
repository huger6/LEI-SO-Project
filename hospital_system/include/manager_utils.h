#ifndef MANAGER_UTILS_H
#define MANAGER_UTILS_H

// Setup all signal handlers for the manager process
void setup_signal_handlers(void);

// Returns 1 if valid, 0 if invalid
int validate_patient_id(const char *id);

#endif
