#include <stdio.h>
#include <stdlib.h>

#include "../include/config.h"

int main(void) {
    system_config_t config;

    printf("--- Starting Config Loader Test ---\n");

    if (load_config("config/config.txt", &config) != 0) {
        fprintf(stderr, "Failed to open config.txt. Make sure the file exists!\n");
        return 1;
    }
    printf("Sucessfully loaded config.txt\n\n");

    print_configs(&config);

    // Validation Check
    if (validate_config(&config)) {
        printf("\n[OK] Configuration Logic Validated.\n");
    } 
    else {
        printf("\n[FAIL] Configuration Logic Invalid (Check 0 values).\n");
    }

    return 0;
}