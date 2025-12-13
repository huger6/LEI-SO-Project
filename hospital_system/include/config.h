#ifndef CONFIGS_H
#define CONFIGS_H

#include <pthread.h>
#include <time.h>

typedef struct {
    char key[50];
    char value[100];
} config_param_t;

// Medication configuration structure
typedef struct {
    char name[50];
    int initial_stock;
    int threshold;
} med_config_t;

// System configuration structure
typedef struct {
    // Globals
    int time_unit_ms;
    int max_emergency_patients;
    int max_appointments;
    int max_surgeries_pending;
    
    // Triage
    int triage_simultaneous_patients;
    int triage_critical_stability;
    int triage_emergency_duration;
    int triage_appointment_duration;
    
    // Surgery Block (BO)
    int bo1_min_duration; int bo1_max_duration;
    int bo2_min_duration; int bo2_max_duration;
    int bo3_min_duration; int bo3_max_duration;
    int cleanup_min_time; int cleanup_max_time;
    int max_medical_teams;
    
    // Pharmacy
    int pharmacy_prep_time_min; int pharmacy_prep_time_max;
    int auto_restock_enabled;
    int restock_qty_multiplier;
    
    // Labs
    int lab1_min_duration; int lab1_max_duration;
    int lab2_min_duration; int lab2_max_duration;
    int max_simultaneous_tests_lab1;
    int max_simultaneous_tests_lab2;
    
    // Medication list (15 items)
    med_config_t medications[15];
    int med_count;
} system_config_t;


int load_config(const char *filename, system_config_t *config);
int parse_config_line(char *line, config_param_t *param);
int validate_config(system_config_t *config);

void print_configs(system_config_t *config);

#endif