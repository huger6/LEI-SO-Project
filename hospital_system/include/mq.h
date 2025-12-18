#ifndef MQ_H
#define MQ_H

#include <sys/types.h>
#include <time.h>

#define MSG_SIZE_CALC(size) (size - sizeof(long))

#define MAX_PRIORITY_TO_ACCEPT(max_type) (-(max_type))

// Priorities
#define PRIORITY_URGENT  1
#define PRIORITY_HIGH    2
#define PRIORITY_NORMAL  3

// FTOK (update mq if changed)
#define FTOK_PATH       "/tmp/hospital_key"

// Keys
#define KEY_TRIAGE      "T"
#define KEY_SURGERY     "S"
#define KEY_PHARMACY    "P"
#define KEY_LAB         "L"
#define KEY_RESPONSES   "R"

// --- MQ ---

// Message types
typedef enum {
    MSG_NEW_EMERGENCY = 1,
    MSG_NEW_APPOINTMENT,
    MSG_NEW_SURGERY,

    MSG_PHARMACY_REQUEST,
    MSG_PHARM_READY,

    MSG_LAB_REQUEST,
    MSG_LAB_RESULTS_READY,

    MSG_TRANSFER_PATIENT,
    MSG_REJECT_PATIENT,

    MSG_CRITICAL_STATUS,
    MSG_SHUTDOWN
} message_kind_t;

// Message header (for all)
typedef struct {
    long mtype;
    message_kind_t kind;
    char patient_id[20];
    int operation_id; // surgery id (0 if n/a)
    time_t timestamp;
} msg_header_t;

// --- Specific messages structs ---

typedef struct {
    msg_header_t hdr;
    int triage_level;
    int stability;
} msg_new_emergency_t;

typedef struct {
    msg_header_t hdr;
    int scheduled_time;
} msg_new_appointment_t;

typedef struct {
    msg_header_t hdr;
    int estimated_duration;
} msg_new_surgery_t;

typedef struct {
    msg_header_t hdr;
    int meds_count;
    int meds_id[8];
    int meds_qty[8];
} msg_pharmacy_request_t;

typedef struct {
    msg_header_t hdr;
    int success;
} msg_pharm_ready_t;

typedef struct {
    msg_header_t hdr;
    int tests_count;
    int tests_id[4];
} msg_lab_request_t;

typedef struct {
    msg_header_t hdr;
    int results_code;
} msg_lab_results_t;

typedef struct {
    msg_header_t hdr;
    int from_unit;
    int to_unit;
} msg_transfer_patient_t;

typedef struct {
    msg_header_t hdr;
    int reason_code;
} msg_reject_patient_t;

typedef struct {
    msg_header_t hdr;
    int severity;
    char description[256];
} msg_critical_status_t;


// --- Function Headers ---

// Message queues
int create_all_message_queues();
int remove_all_message_queues();
int send_generic_message(int mq_id, const void *msg_ptr, size_t total_struct_size);
int receive_generic_message(int mq_id, void *msg_buffer, size_t total_struct_size, long max_priority_type);


#endif