#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#include "../include/stats.h"
#include "../include/log.h"
#include "../include/safe_threads.h"
#include "../include/config.h"

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
    safe_pthread_mutex_lock(&stats->mutex);
    
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
        
    double triage_total_capacity_seconds = (double)elapsed_seconds * config->triage_simultaneous_patients; 
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
    
    if (sim_time > 0) {
        // Utilization = Occupied time / (Simulation time * capacity)
        util_lab1 = (stats->total_lab1_time / (sim_time * config->max_simultaneous_tests_lab1)) * 100.0;
    }

    // --- Lab 2 ---
    double avg_time_lab2 = 0.0;
    double util_lab2 = 0.0;
    
    if (stats->total_lab_tests_lab2 > 0) {
        avg_time_lab2 = stats->total_lab2_time / stats->total_lab_tests_lab2;
    }
    
    if (sim_time > 0) {
        // Utilization = Occupied time / (Simulation time * capacity)
        util_lab2 = (stats->total_lab2_time / (sim_time * config->max_simultaneous_tests_lab2)) * 100.0;
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

    safe_pthread_mutex_unlock(&stats->mutex);
}

void save_statistics_snapshot(global_statistics_shm_t *stats) {
    if (!stats) return;

    log_event(INFO, "STATS", "SNAPSHOT", "Saving statistics snapshot");

    // 1. Create File
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename), "results/stats_snapshots/stats_snapshot_%04d%02d%02d_%02d%02d%02d.txt",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        log_event(ERROR, "STATS", "FILE_ERROR", "Failed to create snapshot file");
        return;
    }

    safe_pthread_mutex_lock(&stats->mutex);

    if (!config) {
        log_event(ERROR, "STATS", "CONFIG_ERROR", "Configuration not available for stats snapshot");
        safe_pthread_mutex_unlock(&stats->mutex);
        fclose(fp);
        return;
    }

    // 2. Calculate Metrics
    long elapsed_seconds = now - stats->system_start_time;
    double elapsed_minutes = (elapsed_seconds > 0) ? elapsed_seconds / 60.0 : 1.0;
    double sim_time = (stats->simulation_time_units > 0) ? (double)stats->simulation_time_units : 1.0;

    // Triage
    double avg_wait_em = (stats->completed_emergencies > 0) ? 
        stats->total_emergency_wait_time / stats->completed_emergencies : 0.0;
    double avg_wait_app = (stats->completed_appointments > 0) ? 
        stats->total_appointment_wait_time / stats->completed_appointments : 0.0;
    
    double triage_total_capacity_seconds = (double)elapsed_seconds * config->triage_simultaneous_patients; 
    double triage_occupancy_rate = 0.0;
    if (triage_total_capacity_seconds > 0) {
        triage_occupancy_rate = (stats->total_triage_usage_time / triage_total_capacity_seconds) * 100.0;
    }

    // Surgery
    double bo1_util_pct = (stats->bo1_utilization_time / sim_time) * 100.0;
    double bo2_util_pct = (stats->bo2_utilization_time / sim_time) * 100.0;
    double bo3_util_pct = (stats->bo3_utilization_time / sim_time) * 100.0;

    // Pharmacy
    med_sort_t sorted_meds[15];
    for(int i = 0; i < 15; i++) {
        sorted_meds[i].id = i;
        sorted_meds[i].count = stats->medication_usage[i];
    }
    qsort(sorted_meds, 15, sizeof(med_sort_t), compare_meds);

    // Labs - use simulation time units (same as surgery)
    double util_lab1 = 0.0;
    if (sim_time > 0) {
        util_lab1 = (stats->total_lab1_time / (sim_time * config->max_simultaneous_tests_lab1)) * 100.0;
    }
    double util_lab2 = 0.0;
    if (sim_time > 0) {
        util_lab2 = (stats->total_lab2_time / (sim_time * config->max_simultaneous_tests_lab2)) * 100.0;
    }
    int total_lab_tests = stats->total_lab_tests_lab1 + stats->total_lab_tests_lab2;
    double global_lab_avg = (total_lab_tests > 0) ? 
        stats->total_lab_turnaround_time / total_lab_tests : 0.0;

    // 3. Write to File
    fprintf(fp, "==========================================\n");
    fprintf(fp, "HOSPITAL SYSTEM STATISTICS SNAPSHOT\n");
    fprintf(fp, "==========================================\n");
    char time_buf[30];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", t);
    fprintf(fp, "Timestamp: %s\n", time_buf);
    fprintf(fp, "System Uptime: %ld seconds (%.2f minutes)\n\n", elapsed_seconds, elapsed_minutes);

    fprintf(fp, "--- TRIAGE STATS ---\n");
    fprintf(fp, "Total Emergencies: %d\n", stats->total_emergency_patients);
    fprintf(fp, "Total Appointments: %d\n", stats->total_appointments);
    fprintf(fp, "Avg Wait Time (Emerg): %.2f tu\n", avg_wait_em);
    fprintf(fp, "Avg Wait Time (Appt): %.2f tu\n", avg_wait_app);
    fprintf(fp, "Rejected Patients: %d\n", stats->rejected_patients);
    fprintf(fp, "Occupancy Rate: %.2f%%\n\n", triage_occupancy_rate);

    fprintf(fp, "--- SURGERY STATS ---\n");
    fprintf(fp, "BO1 (Cardiology): %d surgeries | Utilization: %.2f%%\n", stats->total_surgeries_bo1, bo1_util_pct);
    fprintf(fp, "BO2 (Orthopedics): %d surgeries | Utilization: %.2f%%\n", stats->total_surgeries_bo2, bo2_util_pct);
    fprintf(fp, "BO3 (Neurology): %d surgeries | Utilization: %.2f%%\n", stats->total_surgeries_bo3, bo3_util_pct);
    fprintf(fp, "Cancelled Surgeries: %d\n\n", stats->cancelled_surgeries);

    fprintf(fp, "--- PHARMACY STATS ---\n");
    fprintf(fp, "Total Requests: %d\n", stats->total_pharmacy_requests);
    fprintf(fp, "Stock Depletions: %d\n", stats->stock_depletions);
    fprintf(fp, "Top 3 Medications:\n");
    for(int i = 0; i < 3; i++) {
        fprintf(fp, "  %d. %s (%d units)\n", i+1, MEDICATION_NAMES[sorted_meds[i].id], sorted_meds[i].count);
    }
    fprintf(fp, "\n");

    fprintf(fp, "--- LABORATORY STATS ---\n");
    fprintf(fp, "Lab 1 Tests: %d | Utilization: %.2f%%\n", stats->total_lab_tests_lab1, util_lab1);
    fprintf(fp, "Lab 2 Tests: %d | Utilization: %.2f%%\n", stats->total_lab_tests_lab2, util_lab2);
    fprintf(fp, "Avg Turnaround Time: %.2f tu\n\n", global_lab_avg);

    // 4. Comparative Charts
    fprintf(fp, "--- COMPARATIVE CHARTS ---\n\n");

    // Chart 1: Triage Wait Times
    fprintf(fp, "1. Average Wait Times (Triage)\n");
    double max_wait = (avg_wait_em > avg_wait_app) ? avg_wait_em : avg_wait_app;
    if (max_wait < 1.0) max_wait = 1.0; 

    int width_em = (int)((avg_wait_em / max_wait) * 40);
    int width_app = (int)((avg_wait_app / max_wait) * 40);

    fprintf(fp, "Emergency   [%5.1f tu]: ", avg_wait_em);
    for(int k=0; k<width_em; k++) fprintf(fp, "*");
    fprintf(fp, "\n");

    fprintf(fp, "Appointment [%5.1f tu]: ", avg_wait_app);
    for(int k=0; k<width_app; k++) fprintf(fp, "*");
    fprintf(fp, "\n\n");

    // Chart 2: Surgery Room Utilization
    fprintf(fp, "2. Surgery Room Utilization (%%)\n");
    int width_bo1 = (int)(bo1_util_pct / 2.0);
    int width_bo2 = (int)(bo2_util_pct / 2.0);
    int width_bo3 = (int)(bo3_util_pct / 2.0);

    fprintf(fp, "BO1 (Cardio) [%5.1f%%]: ", bo1_util_pct);
    for(int k=0; k<width_bo1; k++) fprintf(fp, "*");
    fprintf(fp, "\n");

    fprintf(fp, "BO2 (Ortho)  [%5.1f%%]: ", bo2_util_pct);
    for(int k=0; k<width_bo2; k++) fprintf(fp, "*");
    fprintf(fp, "\n");

    fprintf(fp, "BO3 (Neuro)  [%5.1f%%]: ", bo3_util_pct);
    for(int k=0; k<width_bo3; k++) fprintf(fp, "*");
    fprintf(fp, "\n\n");

    // Chart 3: Lab Utilization
    fprintf(fp, "3. Laboratory Utilization (%%)\n");
    int width_lab1 = (int)(util_lab1 / 2.0);
    int width_lab2 = (int)(util_lab2 / 2.0);

    fprintf(fp, "Lab 1        [%5.1f%%]: ", util_lab1);
    for(int k=0; k<width_lab1; k++) fprintf(fp, "*");
    fprintf(fp, "\n");

    fprintf(fp, "Lab 2        [%5.1f%%]: ", util_lab2);
    for(int k=0; k<width_lab2; k++) fprintf(fp, "*");
    fprintf(fp, "\n\n\n");

    safe_pthread_mutex_unlock(&stats->mutex);
    fclose(fp);
    
    log_event(INFO, "STATS", "SNAPSHOT", "Statistics snapshot saved successfully");
}

// Initialize stats to the default value
void init_stats_default(global_statistics_shm_t *stats, pthread_mutexattr_t *attr) {
    if (!stats) return;

    memset(stats, 0, sizeof(*stats));
    safe_pthread_mutex_init(&stats->mutex, attr);

    // Lock to ensure safe initialization
    safe_pthread_mutex_lock(&stats->mutex);

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

    safe_pthread_mutex_unlock(&stats->mutex);
}

