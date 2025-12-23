#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include "../include/command_handler.h"
#include "../include/mq.h"
#include "../include/shm.h"
#include "../include/log.h"
#include "../include/console_input.h"
#include "../include/scheduler.h"
#include "../include/stats.h"
#include "../include/manager_utils.h"
#include "../include/safe_threads.h"

void handle_command(char *cmd_buf, int current_time) {
    char *saveptr;
    char *cmd = strtok_r(cmd_buf, " ", &saveptr);
    if (!cmd) return;

    if (strcasecmp(cmd, "SHUTDOWN") == 0) {
        kill(getpid(), SIGINT);
    }
    else if (strcasecmp(cmd, "STATUS") == 0) {
        char *component = strtok_r(NULL, " ", &saveptr);
        if (!component) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "STATUS: Missing component");
            printf("Format: STATUS <component>\n");
            printf("  <component>: ALL | TRIAGE | SURGERY | PHARMACY | LAB\n");
            return;
        }
        
        if (strcasecmp(component, "ALL") != 0 &&
            strcasecmp(component, "TRIAGE") != 0 &&
            strcasecmp(component, "SURGERY") != 0 &&
            strcasecmp(component, "PHARMACY") != 0 &&
            strcasecmp(component, "LAB") != 0) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "STATUS: Invalid component");
            printf("Format: STATUS <component>\n");
            printf("  <component>: ALL | TRIAGE | SURGERY | PHARMACY | LAB\n");
            return;
        }
        
        display_statistics_console(shm_hospital->shm_stats, component);
    }
    else if (strcasecmp(cmd, "EMERGENCY") == 0) {
        char *code = strtok_r(NULL, " ", &saveptr);
        if (!code) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "EMERGENCY: Missing code");
            printf("Format: EMERGENCY <patient_id> init: <time> triage: <1-5> stability: <value> [tests: <test1,test2,...>] [meds: <med1,med2,...>]\n");
            return;
        }
        
        if (!validate_patient_id(code)) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "EMERGENCY: Invalid patient ID format");
            printf("Format: EMERGENCY <patient_id> init: <time> triage: <1-5> stability: <value> [tests: <test1,test2,...>] [meds: <med1,med2,...>]\n");
            return;
        }
        
        msg_new_emergency_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.hdr.mtype = 1;
        msg.hdr.kind = MSG_NEW_EMERGENCY;
        strncpy(msg.hdr.patient_id, code, sizeof(msg.hdr.patient_id) - 1);
        
        int init = -1;
        int triage = -1;
        int stability = -1;
        int has_init = 0, has_triage = 0, has_stability = 0;

        char *token;
        while ((token = strtok_r(NULL, " ", &saveptr))) {
            if (strcmp(token, "init:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { init = atoi(val); has_init = 1; }
            } else if (strcmp(token, "triage:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { triage = atoi(val); has_triage = 1; }
            } else if (strcmp(token, "stability:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { stability = atoi(val); has_stability = 1; }
            } else if (strcmp(token, "tests:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) msg.tests_count = parse_list_ids(val, msg.tests_id, 3, get_test_id);
            } else if (strcmp(token, "meds:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) msg.meds_count = parse_list_ids(val, msg.meds_id, 5, get_med_id);
            }
        }
        
        if (!has_init || init < 0) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "EMERGENCY: Invalid/Missing init time");
            printf("Format: EMERGENCY <patient_id> init: <time> triage: <1-5> stability: <value> [tests: <test1,test2,...>] [meds: <med1,med2,...>]\n");
            return;
        }
        if (!has_triage || triage < 1 || triage > 5) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "EMERGENCY: Invalid/Missing triage (1-5)");
            printf("Format: EMERGENCY <patient_id> init: <time> triage: <1-5> stability: <value> [tests: <test1,test2,...>] [meds: <med1,med2,...>]\n");
            return;
        }
        if (!has_stability || stability < 100) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "EMERGENCY: Invalid/Missing stability (>=100)");
            printf("Format: EMERGENCY <patient_id> init: <time> triage: <1-5> stability: <value> [tests: <test1,test2,...>] [meds: <med1,med2,...>]\n");
            return;
        }

        msg.hdr.timestamp = current_time + init;
        msg.triage_level = triage;
        msg.stability = stability;
        
        if (msg.hdr.timestamp <= current_time) {
            send_generic_message(mq_triage_id, &msg, sizeof(msg));
            log_event(INFO, "MANAGER", "EMERGENCY_SENT", "Emergency sent to triage");
        } else {
            add_scheduled_event((int)msg.hdr.timestamp, mq_triage_id, &msg, sizeof(msg));
        }
    }
    else if (strcasecmp(cmd, "APPOINTMENT") == 0) {
        char *code = strtok_r(NULL, " ", &saveptr);
        if (!code) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "APPOINTMENT: Missing code");
            printf("Format: APPOINTMENT <patient_id> init: <time> scheduled: <time> doctor: <specialty> [tests: <test1,test2,...>]\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            return;
        }
        
        if (!validate_patient_id(code)) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "APPOINTMENT: Invalid patient ID format");
            printf("Format: APPOINTMENT <patient_id> init: <time> scheduled: <time> doctor: <specialty> [tests: <test1,test2,...>]\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            return;
        }
        
        msg_new_appointment_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.hdr.mtype = 1;
        msg.hdr.kind = MSG_NEW_APPOINTMENT;
        strncpy(msg.hdr.patient_id, code, sizeof(msg.hdr.patient_id) - 1);
        
        int init = -1;
        int scheduled = -1;
        int doctor_id = -1;
        int has_init = 0, has_scheduled = 0, has_doctor = 0;

        char *token;
        while ((token = strtok_r(NULL, " ", &saveptr))) {
            if (strcmp(token, "init:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { init = atoi(val); has_init = 1; }
            } else if (strcmp(token, "scheduled:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { scheduled = atoi(val); has_scheduled = 1; }
            } else if (strcmp(token, "doctor:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { 
                    doctor_id = get_specialty_id(val); 
                    if (doctor_id != -1) has_doctor = 1;
                }
            } else if (strcmp(token, "tests:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) msg.tests_count = parse_list_ids(val, msg.tests_id, 3, get_test_id);
            }
        }
        
        if (!has_init || init < 0) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "APPOINTMENT: Invalid/Missing init time");
            printf("Format: APPOINTMENT <patient_id> init: <time> scheduled: <time> doctor: <specialty> [tests: <test1,test2,...>]\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            return;
        }
        if (!has_scheduled || scheduled <= current_time + init) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "APPOINTMENT: Invalid/Missing scheduled time (> init + current_time)");
            printf("Format: APPOINTMENT <patient_id> init: <time> scheduled: <time> doctor: <specialty> [tests: <test1,test2,...>]\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            return;
        }
        if (!has_doctor) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "APPOINTMENT: Invalid/Missing doctor (CARDIO/ORTHO/NEURO)");
            printf("Format: APPOINTMENT <patient_id> init: <time> scheduled: <time> doctor: <specialty> [tests: <test1,test2,...>]\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            return;
        }

        msg.hdr.timestamp = current_time + init;
        msg.scheduled_time = scheduled;
        msg.doctor_specialty = doctor_id;
        
        if (msg.hdr.timestamp <= current_time) {
            send_generic_message(mq_triage_id, &msg, sizeof(msg));
        } else {
            add_scheduled_event((int)msg.hdr.timestamp, mq_triage_id, &msg, sizeof(msg));
        }
    }
    else if (strcasecmp(cmd, "SURGERY") == 0) {
        char *code = strtok_r(NULL, " ", &saveptr);
        if (!code) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "SURGERY: Missing code");
            printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            printf("  <level>: LOW | MEDIUM | HIGH\n");
            printf("  Note: PREOP test is required\n");
            return;
        }
        
        if (!validate_patient_id(code)) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "SURGERY: Invalid patient ID format");
            printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            printf("  <level>: LOW | MEDIUM | HIGH\n");
            printf("  Note: PREOP test is required\n");
            return;
        }
        
        msg_new_surgery_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.hdr.mtype = 1;
        msg.hdr.kind = MSG_NEW_SURGERY;
        strncpy(msg.hdr.patient_id, code, sizeof(msg.hdr.patient_id) - 1);
        
        int init = -1;
        int scheduled = -1;
        int type_id = -1;
        int urgency_id = -1;
        int has_init = 0, has_scheduled = 0, has_type = 0, has_urgency = 0;

        char *token;
        while ((token = strtok_r(NULL, " ", &saveptr))) {
            if (strcmp(token, "init:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { init = atoi(val); has_init = 1; }
            } else if (strcmp(token, "type:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { 
                    type_id = get_specialty_id(val); 
                    if (type_id != -1) has_type = 1;
                }
            } else if (strcmp(token, "scheduled:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { scheduled = atoi(val); has_scheduled = 1; }
            } else if (strcmp(token, "urgency:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { 
                    urgency_id = get_urgency_id(val); 
                    if (urgency_id != -1) has_urgency = 1;
                }
            } else if (strcmp(token, "tests:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) msg.tests_count = parse_list_ids(val, msg.tests_id, 5, get_test_id);
            } else if (strcmp(token, "meds:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) msg.meds_count = parse_list_ids(val, msg.meds_id, 5, get_med_id);
            }
        }
        
        if (!has_init || init < 0) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "SURGERY: Invalid/Missing init time");
            printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            printf("  <level>: LOW | MEDIUM | HIGH\n");
            printf("  Note: PREOP test is required\n");
            return;
        }
        if (!has_scheduled || scheduled < init) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "SURGERY: Invalid/Missing scheduled time < init");
            printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            printf("  <level>: LOW | MEDIUM | HIGH\n");
            printf("  Note: PREOP test is required\n");
            return;
        }
        if (!has_type) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "SURGERY: Invalid/Missing type (CARDIO/ORTHO/NEURO)");
            printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            printf("  <level>: LOW | MEDIUM | HIGH\n");
            printf("  Note: PREOP test is required\n");
            return;
        }
        if (!has_urgency) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "SURGERY: Invalid/Missing urgency (LOW/MEDIUM/HIGH)");
            printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            printf("  <level>: LOW | MEDIUM | HIGH\n");
            printf("  Note: PREOP test is required\n");
            return;
        }
        
        // Check PREOP
        int preop_id = get_test_id("PREOP");
        int has_preop = 0;
        for (int i = 0; i < msg.tests_count; i++) {
            if (msg.tests_id[i] == preop_id) has_preop = 1;
        }
        if (!has_preop) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "SURGERY: Missing PREOP test");
            printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            printf("  <level>: LOW | MEDIUM | HIGH\n");
            printf("  Note: PREOP test is required\n");
            return;
        }
        
        // Check Meds
        
        if (msg.meds_count < 1) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "SURGERY: Missing medications");
            printf("Format: SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
            printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
            printf("  <level>: LOW | MEDIUM | HIGH\n");
            printf("  Note: PREOP test is required\n");
            return;
        }

        msg.hdr.timestamp = current_time + init;
        msg.surgery_type = type_id;
        msg.scheduled_time = scheduled;
        msg.urgency = urgency_id;
        
        /*
        printf("[DEBUG] SURGERY parsed parameters:\n");
        printf("  patient_id: %s\n", msg.hdr.patient_id);
        printf("  init: %d, scheduled: %d, type: %d, urgency: %d\n", 
               init, scheduled, type_id, urgency_id);
        printf("  tests_count: %d, tests_id: [", msg.tests_count);
        for (int i = 0; i < msg.tests_count; i++) {
            printf("%d%s", msg.tests_id[i], (i < msg.tests_count - 1) ? "," : "");
        }
        printf("]\n");
        printf("  meds_count: %d, meds_id: [", msg.meds_count);
        for (int i = 0; i < msg.meds_count; i++) {
            printf("%d%s", msg.meds_id[i], (i < msg.meds_count - 1) ? "," : "");
        }
        printf("]\n");
        */
        
        if (msg.hdr.timestamp <= current_time) {
            send_generic_message(mq_surgery_id, &msg, sizeof(msg));
        } else {
            add_scheduled_event((int)msg.hdr.timestamp, mq_surgery_id, &msg, sizeof(msg));
        }
    }
    else if (strcasecmp(cmd, "PHARMACY_REQUEST") == 0) {
        char *code = strtok_r(NULL, " ", &saveptr);
        if (!code) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "PHARMACY_REQUEST: Missing code");
            printf("Format: PHARMACY_REQUEST <patient_id> init: <time> priority: <priority> items: <med1:qty1,med2:qty2,...>\n");
            printf("  <priority>: URGENT | HIGH | NORMAL\n");
            return;
        }
        
        if (!validate_patient_id(code)) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "PHARMACY_REQUEST: Invalid patient ID format");
            printf("Format: PHARMACY_REQUEST <patient_id> init: <time> priority: <priority> items: <med1:qty1,med2:qty2,...>\n");
            printf("  <priority>: URGENT | HIGH | NORMAL\n");
            return;
        }
        
        msg_pharmacy_request_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.hdr.mtype = 1;
        msg.hdr.kind = MSG_PHARMACY_REQUEST;
        strncpy(msg.hdr.patient_id, code, sizeof(msg.hdr.patient_id) - 1);
        
        int init = -1;
        int priority = -1;
        int has_init = 0, has_priority = 0;

        char *token;
        while ((token = strtok_r(NULL, " ", &saveptr))) {
            if (strcmp(token, "init:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { init = atoi(val); has_init = 1; }
            } else if (strcmp(token, "priority:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) {
                    if (strcasecmp(val, "URGENT") == 0) priority = PRIORITY_URGENT;
                    else if (strcasecmp(val, "HIGH") == 0) priority = PRIORITY_HIGH;
                    else if (strcasecmp(val, "NORMAL") == 0) priority = PRIORITY_NORMAL;
                    
                    if (priority != -1) has_priority = 1;
                }
            } else if (strcmp(token, "items:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) msg.meds_count = parse_med_qty_list(val, msg.meds_id, msg.meds_qty, 8);
            }
        }
        
        if (!has_init || init < 0) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "PHARMACY_REQUEST: Invalid/Missing init time");
            printf("Format: PHARMACY_REQUEST <patient_id> init: <time> priority: <priority> items: <med1:qty1,med2:qty2,...>\n");
            printf("  <priority>: URGENT | HIGH | NORMAL\n");
            return;
        }
        if (!has_priority) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "PHARMACY_REQUEST: Invalid/Missing priority");
            printf("Format: PHARMACY_REQUEST <patient_id> init: <time> priority: <priority> items: <med1:qty1,med2:qty2,...>\n");
            printf("  <priority>: URGENT | HIGH | NORMAL\n");
            return;
        }

        msg.hdr.timestamp = current_time + init;
        msg.hdr.mtype = priority; // Map priority to mtype
        
        if (msg.hdr.timestamp <= current_time) {
            send_generic_message(mq_pharmacy_id, &msg, sizeof(msg));
        } else {
            add_scheduled_event((int)msg.hdr.timestamp, mq_pharmacy_id, &msg, sizeof(msg));
        }
    }
    else if (strcasecmp(cmd, "LAB_REQUEST") == 0) {
        char *code = strtok_r(NULL, " ", &saveptr);
        if (!code) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "LAB_REQUEST: Missing code");
            printf("Format: LAB_REQUEST <patient_id> init: <time> priority: <priority> lab: <lab> tests: <test1,test2,...>\n");
            printf("  <priority>: URGENT | NORMAL\n");
            printf("  <lab>: LAB1 | LAB2 | BOTH\n");
            return;
        }
        
        if (!validate_patient_id(code)) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "LAB_REQUEST: Invalid patient ID format");
            printf("Format: LAB_REQUEST <patient_id> init: <time> priority: <priority> lab: <lab> tests: <test1,test2,...>\n");
            printf("  <priority>: URGENT | NORMAL\n");
            printf("  <lab>: LAB1 | LAB2 | BOTH\n");
            return;
        }
        
        msg_lab_request_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.hdr.mtype = 1;
        msg.hdr.kind = MSG_LAB_REQUEST;
        strncpy(msg.hdr.patient_id, code, sizeof(msg.hdr.patient_id) - 1);
        
        int init = -1;
        int priority = -1;
        int has_init = 0, has_priority = 0, has_lab = 0;

        char *token;
        while ((token = strtok_r(NULL, " ", &saveptr))) {
            if (strcmp(token, "init:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) { init = atoi(val); has_init = 1; }
            } else if (strcmp(token, "priority:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) {
                    if (strcasecmp(val, "URGENT") == 0) priority = PRIORITY_URGENT;
                    else if (strcasecmp(val, "NORMAL") == 0) priority = PRIORITY_NORMAL;
                    
                    if (priority != -1) has_priority = 1;
                }
            } else if (strcmp(token, "lab:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) {
                    if (strcasecmp(val, "LAB1") == 0 || 
                        strcasecmp(val, "LAB2") == 0 || 
                        strcasecmp(val, "BOTH") == 0) {
                        has_lab = 1;
                    }
                }
            } else if (strcmp(token, "tests:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) msg.tests_count = parse_list_ids(val, msg.tests_id, 4, get_test_id);
            }
        }
        
        if (!has_init || init < 0) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "LAB_REQUEST: Invalid/Missing init time");
            printf("Format: LAB_REQUEST <patient_id> init: <time> priority: <priority> lab: <lab> tests: <test1,test2,...>\n");
            printf("  <priority>: URGENT | NORMAL\n");
            printf("  <lab>: LAB1 | LAB2 | BOTH\n");
            return;
        }
        if (!has_priority) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "LAB_REQUEST: Invalid/Missing priority (URGENT/NORMAL)");
            printf("Format: LAB_REQUEST <patient_id> init: <time> priority: <priority> lab: <lab> tests: <test1,test2,...>\n");
            printf("  <priority>: URGENT | NORMAL\n");
            printf("  <lab>: LAB1 | LAB2 | BOTH\n");
            return;
        }
        if (!has_lab) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "LAB_REQUEST: Invalid/Missing lab (LAB1/LAB2/BOTH)");
            printf("Format: LAB_REQUEST <patient_id> init: <time> priority: <priority> lab: <lab> tests: <test1,test2,...>\n");
            printf("  <priority>: URGENT | NORMAL\n");
            printf("  <lab>: LAB1 | LAB2 | BOTH\n");
            return;
        }

        msg.hdr.timestamp = current_time + init;
        msg.hdr.mtype = priority; // Map priority to mtype
        
        if (msg.hdr.timestamp <= current_time) {
            send_generic_message(mq_lab_id, &msg, sizeof(msg));
        } else {
            add_scheduled_event((int)msg.hdr.timestamp, mq_lab_id, &msg, sizeof(msg));
        }
    }
    else if (strcasecmp(cmd, "RESTOCK") == 0) {
        char *med_name = strtok_r(NULL, " ", &saveptr);
        if (!med_name) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "RESTOCK: Missing medication name");
            printf("Format: RESTOCK <medication_name> quantity: <amount>\n");
            return;
        }
        
        int med_id = get_med_id(med_name);
        if (med_id == -1) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "RESTOCK: Invalid medication name");
            printf("Format: RESTOCK <medication_name> quantity: <amount>\n");
            return;
        }
        
        int qty = -1;
        int has_qty = 0;
        
        char *token;
        while ((token = strtok_r(NULL, " ", &saveptr))) {
            if (strcmp(token, "quantity:") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) {
                    qty = atoi(val);
                    has_qty = 1;
                }
            }
        }
        
        if (!has_qty || qty <= 0) {
            log_event(WARNING, "MANAGER", "INVALID_CMD", "RESTOCK: Invalid/Missing quantity (>0)");
            printf("Format: RESTOCK <medication_name> quantity: <amount>\n");
            return;
        }
        
        safe_pthread_mutex_lock(&shm_hospital->shm_pharm->medications[med_id].mutex);
        shm_hospital->shm_pharm->medications[med_id].current_stock += qty;
        safe_pthread_mutex_unlock(&shm_hospital->shm_pharm->medications[med_id].mutex);
        
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Restocked %s with %d units", med_name, qty);
        log_event(INFO, "MANAGER", "RESTOCK", log_msg);
    }
    else if (strcasecmp(cmd, "HELP") == 0) {
        printf("\n=== HOSPITAL SYSTEM COMMANDS ===\n\n");
        printf("SHUTDOWN\n");
        printf("  Gracefully shuts down the hospital system.\n\n");
        printf("STATUS <component>\n");
        printf("  <component>: ALL | TRIAGE | SURGERY | PHARMACY | LAB\n\n");
        printf("EMERGENCY <patient_id> init: <time> triage: <1-5> stability: <value> [tests: <test1,test2,...>] [meds: <med1,med2,...>]\n");
        printf("  Registers a new emergency patient.\n\n");
        printf("APPOINTMENT <patient_id> init: <time> scheduled: <time> doctor: <specialty> [tests: <test1,test2,...>]\n");
        printf("  <specialty>: CARDIO | ORTHO | NEURO\n\n");
        printf("SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>\n");
        printf("  <specialty>: CARDIO | ORTHO | NEURO\n");
        printf("  <level>: LOW | MEDIUM | HIGH\n");
        printf("  Note: PREOP test is required.\n\n");
        printf("PHARMACY_REQUEST <patient_id> init: <time> priority: <priority> items: <med1:qty1,med2:qty2,...>\n");
        printf("  <priority>: URGENT | HIGH | NORMAL\n\n");
        printf("LAB_REQUEST <patient_id> init: <time> priority: <priority> lab: <lab> tests: <test1,test2,...>\n");
        printf("  <priority>: URGENT | NORMAL\n");
        printf("  <lab>: LAB1 | LAB2 | BOTH\n\n");
        printf("RESTOCK <medication_name> quantity: <amount>\n");
        printf("  Restocks a medication in the pharmacy.\n\n");
        printf("HELP\n");
        printf("  Displays this help message.\n\n");
    }
    else {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Unknown command: %s", cmd);
        log_event(WARNING, "MANAGER", "INVALID_CMD", log_msg);
        printf("Invalid command. For a list of commands, type 'HELP'\n");
    }
}
