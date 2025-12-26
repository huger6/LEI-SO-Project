#ifndef MANAGER_UTILS_H
#define MANAGER_UTILS_H

#include <signal.h>
#include <sys/types.h>

extern pid_t pid_triage, pid_surgery, pid_pharmacy, pid_lab;

extern volatile sig_atomic_t g_stop_child;

int check_shutdown();
void set_shutdown();

void setup_signal_handlers(void);
void setup_child_signals(void);

// ID validation types
typedef enum {
    ID_TYPE_PATIENT,    // PAC{number} - for EMERGENCY, APPOINTMENT, SURGERY
    ID_TYPE_LAB,        // LAB{number} - for LAB_REQUEST
    ID_TYPE_PHARMACY    // REQ{number} - for PHARMACY_REQUEST
} id_type_t;

int validate_id(const char *id, id_type_t type);
int validate_patient_id(const char *id);  // Legacy: same as validate_id with ID_TYPE_PATIENT

// Command format printing helpers
void print_emergency_format(void);
void print_appointment_format(void);
void print_surgery_format(void);
void print_pharmacy_format(void);
void print_lab_format(void);
void print_status_format(void);
void print_restock_format(void);

void child_cleanup();
void manager_cleanup();

void poison_pill_triage();
void poison_pill_surgery();

#endif
