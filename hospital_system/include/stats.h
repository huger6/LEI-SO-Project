#ifndef STATS_H
#define STATS_H

#include <pthread.h>
#include <time.h>

#include "../include/config.h"

// Global statistics (SHM1)
typedef struct {
    pthread_mutex_t mutex;
    pthread_mutexattr_t mutex_attr;
    
    // --- Triage ---
    int total_emergency_patients;
    int total_appointments;
    double total_emergency_wait_time;
    double total_appointment_wait_time;
    double total_triage_usage_time;
    int completed_emergencies;
    int completed_appointments;
    int critical_transfers;
    int rejected_patients;
    
    // --- BO ---
    int total_surgeries_bo1;
    int total_surgeries_bo2;
    int total_surgeries_bo3;
    double total_surgery_wait_time;
    int completed_surgeries;
    int cancelled_surgeries;
    double bo1_utilization_time;
    double bo2_utilization_time;
    double bo3_utilization_time;
    
    // --- Pharmacy ---
    int total_pharmacy_requests;
    int urgent_requests;
    int normal_requests;
    double total_pharmacy_response_time;
    int stock_depletions; // Stock reached 0
    int auto_restocks;
    int medication_usage[15]; 

    // --- Labs ---
    int total_lab_tests_lab1;
    int total_lab_tests_lab2;
    double total_lab1_time;
    double total_lab2_time;
    int total_preop_tests;
    double total_lab_turnaround_time;
    int urgent_lab_tests;

    // --- System ---
    int total_operations;
    int system_errors;
    time_t system_start_time;
    int simulation_time_units;
} global_statistics_t;

// --- Function Headers ---

void init_stats(system_config_t *sys_ptr);
void display_statistics_console(global_statistics_t *stats);
void save_statistics_snapshot(global_statistics_t *stats);
void init_stats_default(global_statistics_t *stats);

#endif