#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/config.h"

// Trim whitespace
void trim(char *str) {
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

int parse_config_line(char *line, config_param_t *param) {
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

// Load configs from config.txt
int load_config(const char *filename, system_config_t *config) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Config file not found");
        return -1;
    }

    char line[256];
    config->med_count = 0;
    config_param_t param;

    while (fgets(line, sizeof(line), file)) {
        if (parse_config_line(line, &param)) {
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
                // Check if it's a medication (contains ':')
                char *colon = strchr(param.value, ':');
                if (colon && config->med_count < 15) {
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
        }
    }

    fclose(file);
    return 0;
}

// Verify loading was successful
int validate_config(system_config_t *config) {
    if (config->time_unit_ms <= 0) return 0;
    if (config->max_medical_teams <= 0) return 0;
    if (config->med_count == 0) return 0; // Ensure we loaded at least some meds
    return 1; // Config is valid
}

void print_configs(system_config_t *config) {
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

    // Operating Blocks (BO)
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
