#ifndef IPC_H
#define IPC_H

#include <sys/types.h>


// IPC Keys
#define FTOK_PATH "/tmp"
#define MQ_URGENT_KEY 'U' // Urgent messages (surgeries, critical emergencies)
#define MQ_NORMAL_KEY 'N' // Normal messages
#define MQ_RESP_KEY   'R' // Responses and notifications

// Message Types
#define MSG_NEW_EMERGENCY       1
#define MSG_NEW_APPOINTMENT     2
#define MSG_NEW_SURGERY         3
#define MSG_PHARMACY_REQUEST    4
#define MSG_LAB_REQUEST         5
#define MSG_PHARMACY_READY      6  
#define MSG_LAB_RESULTS_READY   7
#define MSG_CRITICAL_STATUS     8
#define MSG_TRANSFER_PATIENT    9
#define MSG_REJECT_PATIENT      10

// Shared Memory Keys (Arbitrary chars for ftok)
#define SHM_STATS_KEY 'S'
#define SHM_SURG_KEY  'O'
#define SHM_PHARM_KEY 'P'
#define SHM_LAB_KEY   'L'
#define SHM_LOG_KEY   'G'

// Named Pipes
#define PIPE_INPUT    "input_pipe"
#define PIPE_TRIAGE   "triage_pipe"
#define PIPE_SURGERY  "surgery_pipe"
#define PIPE_PHARMACY "pharmacy_pipe"
#define PIPE_LAB      "lab_pipe"


// Message Queue structure
typedef struct {
    long msg_priority;  // 1=urgent, 2=high, 3=normal

    int msg_type;
    char source[20];
    char target[20];
    char patient_id[15];
    int operation_id;
    time_t timestamp;

    char data[512];
} hospital_message_t;


int send_message(int mqid, hospital_message_t *msg);
int receive_message_priority(int mqid, hospital_message_t *msg);
int create_all_queues();
void cleanup_message_queues();


#endif