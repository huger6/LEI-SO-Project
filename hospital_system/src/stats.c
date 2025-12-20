#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#include "../include/stats.h"
#include "../include/log.h"

// Ptr to track some configs necessary for stats
static system_config_t *sys_config_ptr = NULL;

// Update config ptr
void init_stats(system_config_t *sys_ptr) {
    sys_config_ptr = sys_ptr;
}

// Helper to format time
static void get_time_str(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

// Translate meds names
static const char *MEDICATION_NAMES[15] = {
    "ANALGESICO_A", "ANTIBIOTICO_B", "ANESTESICO_C", "SEDATIVO_D", 
    "ANTIINFLAMATORIO_E", "CARDIOVASCULAR_F", "NEUROLOGICO_G", "ORTOPEDICO_H",
    "HEMOSTATIC_I", "ANTICOAGULANTE_J", "INSULINA_K", "ANALGESICO_FORTE_L",
    "ANTIBIOTICO_FORTE_M", "VITAMINA_N", "SUPLEMENTO_O"
};

// Helper struct for sorting medicines
typedef struct {
    int id;
    int count;
} med_sort_t;

// Comparator for qsort (Descending order)
static int compare_meds(const void *a, const void *b) {
    return ((med_sort_t *)b)->count - ((med_sort_t *)a)->count;
}

void display_statistics_console(global_statistics_shm_t *stats, const char *component) {
    if (!stats) return;

    log_event(INFO, "STATS", "DISPLAY", "Displaying statistic to the console");

    // Lock mutex for thread safety
    pthread_mutex_lock(&stats->mutex);
    
    // Calculate Time Metrics
    char time_buf[30];
    get_time_str(time_buf, sizeof(time_buf)); // Assuming get_time_str exists
    
    time_t now = time(NULL);
    long elapsed_seconds = now - stats->system_start_time;
    double elapsed_minutes = (elapsed_seconds > 0) ? elapsed_seconds / 60.0 : 1.0;
    
    // Prevent division by zero for simulation time units
    double sim_time = (stats->simulation_time_units > 0) ? (double)stats->simulation_time_units : 1.0;

    // Triage Calculations
    double avg_wait_em = (stats->completed_emergencies > 0) ? 
        stats->total_emergency_wait_time / stats->completed_emergencies : 0.0;
        
    double avg_wait_app = (stats->completed_appointments > 0) ? 
        stats->total_appointment_wait_time / stats->completed_appointments : 0.0;
        
    double triage_total_capacity_seconds = (double)elapsed_seconds * sys_config_ptr->triage_simultaneous_patients; 
    double triage_occupancy_rate = 0.0;
    
    if (triage_total_capacity_seconds > 0) {
        triage_occupancy_rate = (stats->total_triage_usage_time / triage_total_capacity_seconds) * 100.0;
    }

    // Surgery Calculations
    double bo1_avg_time = (stats->total_surgeries_bo1 > 0) ? 
        stats->bo1_utilization_time / stats->total_surgeries_bo1 : 0.0;
    double bo1_util_pct = (stats->bo1_utilization_time / sim_time) * 100.0;
    
    double bo2_avg_time = (stats->total_surgeries_bo2 > 0) ? 
        stats->bo2_utilization_time / stats->total_surgeries_bo2 : 0.0;
    double bo2_util_pct = (stats->bo2_utilization_time / sim_time) * 100.0;

    double bo3_avg_time = (stats->total_surgeries_bo3 > 0) ? 
        stats->bo3_utilization_time / stats->total_surgeries_bo3 : 0.0;
    double bo3_util_pct = (stats->bo3_utilization_time / sim_time) * 100.0;

    double avg_surgery_wait = (stats->completed_surgeries > 0) ?
        stats->total_surgery_wait_time / stats->completed_surgeries : 0.0;

    // Pharmacy Calculations
    double avg_pharm_resp = (stats->total_pharmacy_requests > 0) ?
        stats->total_pharmacy_response_time / stats->total_pharmacy_requests : 0.0;

    // Sort medicines to find top 3
    med_sort_t sorted_meds[15];
    for(int i = 0; i < 15; i++) {
        sorted_meds[i].id = i;
        sorted_meds[i].count = stats->medication_usage[i];
    }
    qsort(sorted_meds, 15, sizeof(med_sort_t), compare_meds);

    // 5. Lab Calculations
    // --- Lab 1 ---
    double avg_time_lab1 = 0.0;
    double util_lab1 = 0.0;
    
    if (stats->total_lab_tests_lab1 > 0) {
        avg_time_lab1 = stats->total_lab1_time / stats->total_lab_tests_lab1;
    }
    
    if (elapsed_seconds > 0) {
        // Utilization = Occupied time / (Elapsed time * capacity)
        util_lab1 = (stats->total_lab1_time / ((double)elapsed_seconds * sys_config_ptr->max_simultaneous_tests_lab1)) * 100.0;
    }

    // --- Lab 2 ---
    double avg_time_lab2 = 0.0;
    double util_lab2 = 0.0;
    
    if (stats->total_lab_tests_lab2 > 0) {
        avg_time_lab2 = stats->total_lab2_time / stats->total_lab_tests_lab2;
    }
    
    if (elapsed_seconds > 0) {
        util_lab2 = (stats->total_lab2_time / ((double)elapsed_seconds * sys_config_ptr->max_simultaneous_tests_lab2)) * 100.0;
    }

    // Turnaround
    int total_lab_tests = stats->total_lab_tests_lab1 + stats->total_lab_tests_lab2;
    double global_lab_avg = (total_lab_tests > 0) ? 
        stats->total_lab_turnaround_time / total_lab_tests : 0.0;
    
    // Global Metrics
    double throughput = (elapsed_minutes > 0) ? stats->total_operations / elapsed_minutes : 0.0;
    double success_rate = (stats->total_operations > 0) ?
        ((double)(stats->total_operations - stats->system_errors) / stats->total_operations) * 100.0 : 100.0;

    // ================= DISPLAY =================
    int show_all = (component == NULL || strcasecmp(component, "ALL") == 0);
    int show_triage = (show_all || strcasecmp(component, "TRIAGE") == 0);
    int show_surgery = (show_all || strcasecmp(component, "SURGERY") == 0);
    int show_pharmacy = (show_all || strcasecmp(component, "PHARMACY") == 0);
    int show_lab = (show_all || strcasecmp(component, "LAB") == 0);

    printf("\n==========================================\n");
    printf("HOSPITAL SYSTEM STATISTICS\n");
    printf("==========================================\n");
    printf("Timestamp: %s\n", time_buf);
    printf("Operation Time: %ld seconds (%.0f minutes)\n", elapsed_seconds, elapsed_minutes);
    
    if (show_triage) {
        printf("TRIAGE CENTER ------------------\n");
        printf("Total Emergencies: %d\n", stats->total_emergency_patients);
        printf("Total Appointments: %d\n", stats->total_appointments);
        printf("Avg Wait Time (Emerg.): %.1f tu\n", avg_wait_em);
        printf("Avg Wait Time (Appt.): %.1f tu\n", avg_wait_app);
        printf("Transferred Patients: %d\n", stats->critical_transfers);
        printf("Rejected Patients: %d\n", stats->rejected_patients);
        printf("Occupancy Rate: %.1f%%\n", triage_occupancy_rate);
    }

    if (show_surgery) {
        printf("OPERATING BLOCKS ------------------\n");
        printf("BO1 (Cardiology):\n");
        printf("  Surgeries: %d | Avg Time: %.1f tu | Utilization: %.1f%%\n", 
            stats->total_surgeries_bo1, bo1_avg_time, bo1_util_pct);
        printf("BO2 (Orthopedics):\n");
        printf("  Surgeries: %d | Avg Time: %.1f tu | Utilization: %.1f%%\n", 
            stats->total_surgeries_bo2, bo2_avg_time, bo2_util_pct);
        printf("BO3 (Neurology):\n");
        printf("  Surgeries: %d | Avg Time: %.1f tu | Utilization: %.1f%%\n", 
            stats->total_surgeries_bo3, bo3_avg_time, bo3_util_pct);
        printf("Cancelled Surgeries: %d\n", stats->cancelled_surgeries);
        printf("Avg Wait Time: %.1f tu\n", avg_surgery_wait);
    }

    if (show_pharmacy) {
        printf("CENTRAL PHARMACY ----------------\n");
        printf("Total Requests: %d\n", stats->total_pharmacy_requests);
        printf("Urgent Requests: %d\n", stats->urgent_requests);
        printf("Avg Response Time: %.1f tu\n", avg_pharm_resp);
        printf("Stock Restocks: %d\n", stats->auto_restocks);
        printf("Depletions: %d\n", stats->stock_depletions);
        printf("Top Medicines:\n");
        for(int i = 0; i < 3; i++) {
            printf("  %d. %s: %d units\n", 
                i+1, 
                MEDICATION_NAMES[sorted_meds[i].id], 
                sorted_meds[i].count);
        }
    }

    if (show_lab) {
        printf("LABORATORIES ------------\n");
        printf("LAB1: %d tests | Avg Time: %.1f tu | Utilization: %.1f%%\n", 
            stats->total_lab_tests_lab1, avg_time_lab1, util_lab1);
            
        printf("LAB2: %d tests | Avg Time: %.1f tu | Utilization: %.1f%%\n", 
            stats->total_lab_tests_lab2, avg_time_lab2, util_lab2);
            
        printf("Urgent Tests: %d\n", stats->urgent_lab_tests);
        printf("Global Avg Turnaround: %.1f tu\n", global_lab_avg);
    }

    if (show_all) {
        printf("GLOBALS -------\n");
        printf("Total Operations: %d\n", stats->total_operations);
        printf("Throughput: %.1f ops/min\n", throughput);
        printf("System Errors: %d\n", stats->system_errors);
        printf("Success Rate: %.1f%%\n", success_rate);
    }
    printf("==========================================\n");

    pthread_mutex_unlock(&stats->mutex);
}

void save_statistics_snapshot(global_statistics_shm_t *stats) {
    if (!stats) return;

    log_event(INFO, "STATS", "SNAPSHOT", "Saving statistics snapshot");
    
}

// Initialize stats to the default value
void init_stats_default(global_statistics_shm_t *stats, pthread_mutexattr_t *attr) {
    if (!stats) return;

    memset(stats, 0, sizeof(*stats));
    pthread_mutex_init(&stats->mutex, attr);

    // Lock to ensure safe initialization
    pthread_mutex_lock(&stats->mutex);

    // --- Triage ---
    stats->total_emergency_patients = 0;
    stats->total_appointments = 0;
    stats->total_emergency_wait_time = 0.0;
    stats->total_appointment_wait_time = 0.0;
    stats->total_triage_usage_time = 0.0;
    stats->completed_emergencies = 0;
    stats->completed_appointments = 0;
    stats->critical_transfers = 0;
    stats->rejected_patients = 0;

    // --- BO ---
    stats->total_surgeries_bo1 = 0;
    stats->total_surgeries_bo2 = 0;
    stats->total_surgeries_bo3 = 0;
    stats->total_surgery_wait_time = 0.0;
    stats->completed_surgeries = 0;
    stats->cancelled_surgeries = 0;
    stats->bo1_utilization_time = 0.0;
    stats->bo2_utilization_time = 0.0;
    stats->bo3_utilization_time = 0.0;

    // --- Pharmacy ---
    stats->total_pharmacy_requests = 0;
    stats->urgent_requests = 0;
    stats->normal_requests = 0;
    stats->total_pharmacy_response_time = 0.0;
    stats->stock_depletions = 0;
    stats->auto_restocks = 0;
    memset(stats->medication_usage, 0, sizeof(stats->medication_usage));

    // --- Labs ---
    stats->total_lab_tests_lab1 = 0;
    stats->total_lab_tests_lab2 = 0;
    stats->total_lab1_time = 0.0;
    stats->total_lab2_time = 0.0;
    stats->total_preop_tests = 0;
    stats->total_lab_turnaround_time = 0.0;
    stats->urgent_lab_tests = 0;

    // --- System ---
    stats->total_operations = 0;
    stats->system_errors = 0;
    stats->system_start_time = time(NULL);
    stats->simulation_time_units = 0;

    pthread_mutex_unlock(&stats->mutex);
}

