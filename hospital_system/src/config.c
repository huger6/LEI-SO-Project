#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/config.h"
#include "../include/log.h"

// Global system config ptr
system_config_t *config = NULL;

// Trim whitespace
static void trim(char *str) {
    char *start = str;
    char *end;

    // 1. Skip leading whitespace and store the new start in 'start'
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
    }

    // Handle case where the string is all whitespace
    if (*start == '\0') {
        *str = '\0'; // Make the original string empty
        return;
    }

    // 2. Shift the string to remove leading whitespace
    // memmove is needed if the source and destination overlap (which they do)
    if (start != str) {
        // Source is 'start', Destination is 'str'
        // strlen(start) + 1 to include the null terminator
        memmove(str, start, strlen(start) + 1);
    }

    // 3. Trim trailing whitespace/newlines
    // 'str' now points to the start of the shifted string
    end = str + strlen(str) - 1;
    while (end >= str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }

    // 4. Null-terminate the string after the last non-space character
    *(end + 1) = '\0';
}

static int parse_config_line(char *line, config_param_t *param) {
    trim(line);
    if (line[0] == '#' || line[0] == '\0') {
        return 0; // Skip comments and empty lines
    }

    char *equals = strchr(line, '=');
    if (!equals) {
        return 0; // Invalid line
    }

    *equals = '\0';
    char *key = line;
    char *value = equals + 1;

    trim(key);
    trim(value);

    strncpy(param->key, key, sizeof(param->key) - 1);
    param->key[sizeof(param->key) - 1] = '\0';

    strncpy(param->value, value, sizeof(param->value) - 1);
    param->value[sizeof(param->value) - 1] = '\0';

    return 1;
}

// Verify loading was successful and logically consistent
static int validate_config() {
    if (!config) return 0;

    int valid = 1;
    char buffer[256];

    // --- Global Settings ---
    if (config->time_unit_ms <= 0) {
        snprintf(buffer, sizeof(buffer), "TIME_UNIT_MS must be > 0. Found: %d", config->time_unit_ms);
        log_event(ERROR, "CONFIG", "VALIDATION", buffer);
        valid = 0;
    }
    if (config->max_emergency_patients <= 0) {
        snprintf(buffer, sizeof(buffer), "MAX_EMERGENCY_PATIENTS must be > 0. Found: %d", config->max_emergency_patients);
        log_event(ERROR, "CONFIG", "VALIDATION", buffer);
        valid = 0;
    }
    if (config->max_appointments <= 0) {
        snprintf(buffer, sizeof(buffer), "MAX_APPOINTMENTS must be > 0. Found: %d", config->max_appointments);
        log_event(ERROR, "CONFIG", "VALIDATION", buffer);
        valid = 0;
    }
    if (config->max_surgeries_pending <= 0) {
        snprintf(buffer, sizeof(buffer), "MAX_SURGERIES_PENDING must be > 0. Found: %d", config->max_surgeries_pending);
        log_event(ERROR, "CONFIG", "VALIDATION", buffer);
        valid = 0;
    }

    // --- Triage ---
    if (config->triage_simultaneous_patients <= 0) {
        log_event(ERROR, "CONFIG", "VALIDATION", "TRIAGE_SIMULTANEOUS_PATIENTS must be > 0.");
        valid = 0;
    }
    // Stability is likely 0-100 scale
    if (config->triage_critical_stability < 0 || config->triage_critical_stability > 100) {
        snprintf(buffer, sizeof(buffer), "TRIAGE_CRITICAL_STABILITY must be 0-100. Found: %d", config->triage_critical_stability);
        log_event(ERROR, "CONFIG", "VALIDATION", buffer);
        valid = 0;
    }
    if (config->triage_emergency_duration <= 0) {
        log_event(ERROR, "CONFIG", "VALIDATION", "TRIAGE_EMERGENCY_DURATION must be > 0.");
        valid = 0;
    }
    if (config->triage_appointment_duration <= 0) {
        log_event(ERROR, "CONFIG", "VALIDATION", "TRIAGE_APPOINTMENT_DURATION must be > 0.");
        valid = 0;
    }

    // --- Operating Blocks (Durations) ---
    // Helper macro to check min/max logic and non-negativity
    #define CHECK_TIME_RANGE(min, max, name) \
        if (min < 0) { \
            snprintf(buffer, sizeof(buffer), "%s Min Duration cannot be negative (%d).", name, min); \
            log_event(ERROR, "CONFIG", "VALIDATION", buffer); \
            valid = 0; \
        } \
        if (max <= 0) { \
            snprintf(buffer, sizeof(buffer), "%s Max Duration must be > 0 (%d).", name, max); \
            log_event(ERROR, "CONFIG", "VALIDATION", buffer); \
            valid = 0; \
        } \
        if (min > max) { \
            snprintf(buffer, sizeof(buffer), "%s Min (%d) > Max (%d).", name, min, max); \
            log_event(ERROR, "CONFIG", "VALIDATION", buffer); \
            valid = 0; \
        }

    CHECK_TIME_RANGE(config->bo1_min_duration, config->bo1_max_duration, "BO1");
    CHECK_TIME_RANGE(config->bo2_min_duration, config->bo2_max_duration, "BO2");
    CHECK_TIME_RANGE(config->bo3_min_duration, config->bo3_max_duration, "BO3");
    CHECK_TIME_RANGE(config->cleanup_min_time, config->cleanup_max_time, "Cleanup");

    if (config->max_medical_teams <= 0) {
        log_event(ERROR, "CONFIG", "VALIDATION", "MAX_MEDICAL_TEAMS must be > 0.");
        valid = 0;
    }

    // --- Pharmacy ---
    CHECK_TIME_RANGE(config->pharmacy_prep_time_min, config->pharmacy_prep_time_max, "Pharmacy Prep");

    if (config->auto_restock_enabled != 0 && config->auto_restock_enabled != 1) {
        snprintf(buffer, sizeof(buffer), "AUTO_RESTOCK_ENABLED must be 0 or 1. Found: %d", config->auto_restock_enabled);
        log_event(ERROR, "CONFIG", "VALIDATION", buffer);
        valid = 0;
    }
    
    // Multiplier cannot be negative or zero (multiplying by 0 kills restock logic)
    if (config->restock_qty_multiplier <= 0) {
        snprintf(buffer, sizeof(buffer), "RESTOCK_QUANTITY_MULTIPLIER must be > 0. Found: %d", config->restock_qty_multiplier);
        log_event(ERROR, "CONFIG", "VALIDATION", buffer);
        valid = 0;
    }

    // --- Laboratories ---
    CHECK_TIME_RANGE(config->lab1_min_duration, config->lab1_max_duration, "LAB1");
    CHECK_TIME_RANGE(config->lab2_min_duration, config->lab2_max_duration, "LAB2");

    if (config->max_simultaneous_tests_lab1 <= 0) {
        log_event(ERROR, "CONFIG", "VALIDATION", "MAX_SIMULTANEOUS_TESTS_LAB1 must be > 0.");
        valid = 0;
    }
    if (config->max_simultaneous_tests_lab2 <= 0) {
        log_event(ERROR, "CONFIG", "VALIDATION", "MAX_SIMULTANEOUS_TESTS_LAB2 must be > 0.");
        valid = 0;
    }

    // --- Medications ---
    if (config->med_count == 0) {
        log_event(ERROR, "CONFIG", "VALIDATION", "No medications loaded.");
        valid = 0;
    } else {
        for(int i = 0; i < config->med_count; i++) {
            // Stock can be 0 (out of stock), but not negative
            if (config->medications[i].initial_stock < 0) {
                snprintf(buffer, sizeof(buffer), "Medication %s has negative initial stock (%d).", 
                        config->medications[i].name, config->medications[i].initial_stock);
                log_event(ERROR, "CONFIG", "VALIDATION", buffer);
                valid = 0;
            }
            // Threshold can be 0, but usually not negative
            if (config->medications[i].threshold < 0) {
                snprintf(buffer, sizeof(buffer), "Medication %s has negative threshold (%d).", 
                        config->medications[i].name, config->medications[i].threshold);
                log_event(ERROR, "CONFIG", "VALIDATION", buffer);
                valid = 0;
            }
            // Logical check: Warn if stock starts below threshold
            if (config->medications[i].initial_stock < config->medications[i].threshold) {
                snprintf(buffer, sizeof(buffer), "Medication %s starts below threshold (Stock: %d, Threshold: %d).", 
                        config->medications[i].name, config->medications[i].initial_stock, config->medications[i].threshold);
                log_event(WARNING, "CONFIG", "VALIDATION", buffer);
                // We don't set valid=0 here, just a warning, as this might be intended for testing
            }
        }
    }

    return valid;
}

// Load configs from config.txt
int load_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        log_event(ERROR, "CONFIG", "LOADING", "config.txt file not found");
        return -1;
    }

    char line[256];
    char buffer[256];
    config->med_count = 0;
    config_param_t param;

    while (fgets(line, sizeof(line), file)) {
        if (parse_config_line(line, &param)) {
            int is_standard = 1;
            if (strcmp(param.key, "TIME_UNIT_MS") == 0) config->time_unit_ms = atoi(param.value);
            else if (strcmp(param.key, "MAX_EMERGENCY_PATIENTS") == 0) config->max_emergency_patients = atoi(param.value);
            else if (strcmp(param.key, "MAX_APPOINTMENTS") == 0) config->max_appointments = atoi(param.value);
            else if (strcmp(param.key, "MAX_SURGERIES_PENDING") == 0) config->max_surgeries_pending = atoi(param.value);
            else if (strcmp(param.key, "TRIAGE_SIMULTANEOUS_PATIENTS") == 0) config->triage_simultaneous_patients = atoi(param.value);
            else if (strcmp(param.key, "TRIAGE_CRITICAL_STABILITY") == 0) config->triage_critical_stability = atoi(param.value);
            else if (strcmp(param.key, "TRIAGE_EMERGENCY_DURATION") == 0) config->triage_emergency_duration = atoi(param.value);
            else if (strcmp(param.key, "TRIAGE_APPOINTMENT_DURATION") == 0) config->triage_appointment_duration = atoi(param.value);
            else if (strcmp(param.key, "BO1_MIN_DURATION") == 0) config->bo1_min_duration = atoi(param.value);
            else if (strcmp(param.key, "BO1_MAX_DURATION") == 0) config->bo1_max_duration = atoi(param.value);
            else if (strcmp(param.key, "BO2_MIN_DURATION") == 0) config->bo2_min_duration = atoi(param.value);
            else if (strcmp(param.key, "BO2_MAX_DURATION") == 0) config->bo2_max_duration = atoi(param.value);
            else if (strcmp(param.key, "BO3_MIN_DURATION") == 0) config->bo3_min_duration = atoi(param.value);
            else if (strcmp(param.key, "BO3_MAX_DURATION") == 0) config->bo3_max_duration = atoi(param.value);
            else if (strcmp(param.key, "CLEANUP_MIN_TIME") == 0) config->cleanup_min_time = atoi(param.value);
            else if (strcmp(param.key, "CLEANUP_MAX_TIME") == 0) config->cleanup_max_time = atoi(param.value);
            else if (strcmp(param.key, "MAX_MEDICAL_TEAMS") == 0) config->max_medical_teams = atoi(param.value);
            else if (strcmp(param.key, "PHARMACY_PREPARATION_TIME_MIN") == 0) config->pharmacy_prep_time_min = atoi(param.value);
            else if (strcmp(param.key, "PHARMACY_PREPARATION_TIME_MAX") == 0) config->pharmacy_prep_time_max = atoi(param.value);
            else if (strcmp(param.key, "AUTO_RESTOCK_ENABLED") == 0) config->auto_restock_enabled = atoi(param.value);
            else if (strcmp(param.key, "RESTOCK_QUANTITY_MULTIPLIER") == 0) config->restock_qty_multiplier = atoi(param.value);
            else if (strcmp(param.key, "LAB1_TEST_MIN_DURATION") == 0) config->lab1_min_duration = atoi(param.value);
            else if (strcmp(param.key, "LAB1_TEST_MAX_DURATION") == 0) config->lab1_max_duration = atoi(param.value);
            else if (strcmp(param.key, "MAX_SIMULTANEOUS_TESTS_LAB1") == 0) config->max_simultaneous_tests_lab1 = atoi(param.value);
            else if (strcmp(param.key, "LAB2_TEST_MIN_DURATION") == 0) config->lab2_min_duration = atoi(param.value);
            else if (strcmp(param.key, "LAB2_TEST_MAX_DURATION") == 0) config->lab2_max_duration = atoi(param.value);
            else if (strcmp(param.key, "MAX_SIMULTANEOUS_TESTS_LAB2") == 0) config->max_simultaneous_tests_lab2 = atoi(param.value);
            else {
                is_standard = 0;
                // Check if it's a medication (contains ':')
                char *colon = strchr(param.value, ':');
                if (colon && config->med_count < 15) {
                    snprintf(buffer, sizeof(buffer), "%s=%s", param.key, param.value);
                    log_event(INFO, "CONFIG", "PARAM_LOADED", buffer);

                    *colon = '\0';
                    int stock = atoi(param.value);
                    int threshold = atoi(colon + 1);
                    
                    strncpy(config->medications[config->med_count].name, param.key, 49);
                    config->medications[config->med_count].name[49] = '\0';
                    config->medications[config->med_count].initial_stock = stock;
                    config->medications[config->med_count].threshold = threshold;
                    config->med_count++;
                }
            }

            if (is_standard) {
                snprintf(buffer, sizeof(buffer), "%s=%s", param.key, param.value);
                log_event(INFO, "CONFIG", "PARAM_LOADED", buffer);
            }
        }
    }

    fclose(file);
    return validate_config(config) ? 0 : -1;
}

// Print current system configs
void print_configs() {
    if (!config) return;

    // Global Settings
    printf("=== GLOBAL SETTINGS ===\n");
    printf("Time Unit (ms): %d\n", config->time_unit_ms);
    printf("Max Emergency Patients: %d\n", config->max_emergency_patients);
    printf("Max Appointments: %d\n", config->max_appointments);
    printf("Max Surgeries Pending: %d\n", config->max_surgeries_pending);

    // Triage Settings
    printf("\n=== TRIAGE ===\n");
    printf("Simultaneous Patients: %d\n", config->triage_simultaneous_patients);
    printf("Critical Stability Threshold: %d\n", config->triage_critical_stability);
    printf("Emergency Duration: %d\n", config->triage_emergency_duration);
    printf("Appointment Duration: %d\n", config->triage_appointment_duration);

    // Operating Blocks
    printf("\n=== OPERATING BLOCKS ===\n");
    printf("BO1 Duration: %d - %d\n", config->bo1_min_duration, config->bo1_max_duration);
    printf("BO2 Duration: %d - %d\n", config->bo2_min_duration, config->bo2_max_duration);
    printf("BO3 Duration: %d - %d\n", config->bo3_min_duration, config->bo3_max_duration);
    printf("Cleanup Time: %d - %d\n", config->cleanup_min_time, config->cleanup_max_time);
    printf("Max Medical Teams: %d\n", config->max_medical_teams);

    // Pharmacy & Labs
    printf("\n=== PHARMACY & LABS ===\n");
    printf("Pharmacy Prep Time: %d - %d\n", config->pharmacy_prep_time_min, config->pharmacy_prep_time_max);
    printf("Auto Restock: %s\n", config->auto_restock_enabled ? "ENABLED" : "DISABLED");
    printf("Lab1 Duration: %d - %d (Max Sim: %d)\n", config->lab1_min_duration, config->lab1_max_duration, config->max_simultaneous_tests_lab1);
    printf("Lab2 Duration: %d - %d (Max Sim: %d)\n", config->lab2_min_duration, config->lab2_max_duration, config->max_simultaneous_tests_lab2);

    // Medications
    printf("\n=== MEDICATIONS (Count: %d/15) ===\n", config->med_count);
    printf("%-25s | %-10s | %-10s\n", "Name", "Stock", "Threshold");
    printf("----------------------------------------------------\n");
    
    for (int i = 0; i < config->med_count; i++) {
        printf("%-25s | %-10d | %-10d\n", 
            config->medications[i].name, 
            config->medications[i].initial_stock, 
            config->medications[i].threshold);
    }
}

// Initiliaze the system with default configs
int init_default_config() {
    config = (system_config_t *) malloc(sizeof(system_config_t));
    if (!config) {
        perror("Error allocating memory for the system configs");
        return -1;
    }
    // Ensure there's no garbage
    memset(config, 0, sizeof(system_config_t));

    // --- GLOBAL SETTINGS ---
    config->time_unit_ms = 500;
    config->max_emergency_patients = 50;
    config->max_appointments = 100;
    config->max_surgeries_pending = 30;

    // --- TRIAGE CENTER ---
    config->triage_simultaneous_patients = 3;
    config->triage_critical_stability = 50;
    config->triage_emergency_duration = 15;
    config->triage_appointment_duration = 10;

    // --- OPERATING BLOCKS ---
    // BO1
    config->bo1_min_duration = 50;
    config->bo1_max_duration = 100;
    // BO2
    config->bo2_min_duration = 30;
    config->bo2_max_duration = 60;
    // BO3
    config->bo3_min_duration = 60;
    config->bo3_max_duration = 120;
    
    // Shared Resources
    config->cleanup_min_time = 10;
    config->cleanup_max_time = 20;
    config->max_medical_teams = 2;

    // --- PHARMACY CENTRAL ---
    config->pharmacy_prep_time_min = 5;
    config->pharmacy_prep_time_max = 10;
    config->auto_restock_enabled = 1;
    config->restock_qty_multiplier = 2;

    // --- LABORATORIES ---
    // Lab 1
    config->lab1_min_duration = 10;
    config->lab1_max_duration = 20;
    config->max_simultaneous_tests_lab1 = 2;
    // Lab 2
    config->lab2_min_duration = 15;
    config->lab2_max_duration = 30;
    config->max_simultaneous_tests_lab2 = 2;

    // --- INITIAL MEDICINE STOCK ---
    const med_config_t default_meds[15] = {
        {"ANALGESICO_A",        1000, 200},
        {"ANTIBIOTICO_B",       800,  150},
        {"ANESTESICO_C",        500,  100},
        {"SEDATIVO_D",          600,  120},
        {"ANTIINFLAMATORIO_E",  900,  180},
        {"CARDIOVASCULAR_F",    400,  80},
        {"NEUROLOGICO_G",       300,  60},
        {"ORTOPEDICO_H",        700,  140},
        {"HEMOSTATIC_I",        350,  70},
        {"ANTICOAGULANTE_J",    450,  90},
        {"INSULINA_K",          250,  50},
        {"ANALGESICO_FORTE_L",  550,  110},
        {"ANTIBIOTICO_FORTE_M", 650,  130},
        {"VITAMINA_N",          1200, 240},
        {"SUPLEMENTO_O",        1000, 200}
    };

    config->med_count = 15;
    
    // Copy meds into the config struct
    for (int i = 0; i < 15; i++) {
        // Safe string copy
        strncpy(config->medications[i].name, default_meds[i].name, sizeof(config->medications[i].name) - 1);
        config->medications[i].name[sizeof(config->medications[i].name) - 1] = '\0'; // Ensure null termination
        
        config->medications[i].initial_stock = default_meds[i].initial_stock;
        config->medications[i].threshold = default_meds[i].threshold;
    }

    return 0;
}

// Clean up and free config memory
void cleanup_config() {
    if (config) {
        free(config);
        config = NULL;
    }
}

