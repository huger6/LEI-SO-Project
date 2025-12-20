#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>

#include "../include/mq.h"
#include "../include/log.h"


#define MSG_SIZE_CALC(size) (size - sizeof(long))

#define MAX_PRIORITY_TO_ACCEPT(max_type) (-(max_type))

// Priorities
#define PRIORITY_URGENT  1
#define PRIORITY_HIGH    2
#define PRIORITY_NORMAL  3


// Message Queues Id's
int mq_triage_id = -1;
int mq_surgery_id = -1;
int mq_pharmacy_id = -1;
int mq_lab_id = -1;
int mq_responses_id = -1;


// --- Message Queues ---

// Creates MQ (handles case where it already exists)
static int create_single_mq(int key_char, const char *name) {
    key_t key = ftok(FTOK_PATH, key_char);
    if (key == -1) {
        log_event(ERROR, "IPC", "FTOK_FAIL", "Failed to generate key for MQ");
        return -1;
    }

    // IPC_CREAT | IPC_EXCL makes creation fail if already exists
    int mq_id = msgget(key, IPC_CREAT | IPC_EXCL | 0666);
    if (mq_id == -1) {
        if (errno == EEXIST) {
            // Q already exists, we try to get ID
            mq_id = msgget(key, 0666);
            if (mq_id != -1) {
                log_event(WARNING, "IPC", "MQ_REUSE", name);
            } 
            else {
                log_event(ERROR, "IPC", "MQ_GET_FAIL", name);
            }
        } 
        else {
            char desc[128];
            snprintf(desc, sizeof(desc), "Failed to create %s: %s", name, strerror(errno));
            log_event(ERROR, "IPC", "MQ_CREATE_FAIL", desc);
        }
    }
    return mq_id;
}

int create_all_message_queues() {
    // 1. Triage Queue
    mq_triage_id = create_single_mq(KEY_TRIAGE, "MQ_TRIAGE");
    if (mq_triage_id == -1) return -1;

    // 2. Surgery Queue
    mq_surgery_id = create_single_mq(KEY_SURGERY, "MQ_SURGERY");
    if (mq_surgery_id == -1) return -1;

    // 3. Pharmacy Queue
    mq_pharmacy_id = create_single_mq(KEY_PHARMACY, "MQ_PHARMACY");
    if (mq_pharmacy_id == -1) return -1;

    // 4. Lab Queue
    mq_lab_id = create_single_mq(KEY_LAB, "MQ_LAB");
    if (mq_lab_id == -1) return -1;

    // 5. Responses Queue
    mq_responses_id = create_single_mq(KEY_RESPONSES, "MQ_RESPONSES");
    if (mq_responses_id == -1) return -1;

    log_event(INFO, "IPC", "MQ_CREATED", "All Message Queues successfully initialized");
    return 0;
}

int remove_all_message_queues() {
    int result = 0;

    if (mq_triage_id != -1 && msgctl(mq_triage_id, IPC_RMID, NULL) == -1) {
        log_event(ERROR, "IPC", "MQ_RM_FAIL", "Failed to remove MQ_TRIAGE");
        result = -1;
    }
    if (mq_surgery_id != -1 && msgctl(mq_surgery_id, IPC_RMID, NULL) == -1) {
        log_event(ERROR, "IPC", "MQ_RM_FAIL", "Failed to remove MQ_SURGERY");
        result = -1;
    }
    if (mq_pharmacy_id != -1 && msgctl(mq_pharmacy_id, IPC_RMID, NULL) == -1) {
        log_event(ERROR, "IPC", "MQ_RM_FAIL", "Failed to remove MQ_PHARMACY");
        result = -1;
    }
    if (mq_lab_id != -1 && msgctl(mq_lab_id, IPC_RMID, NULL) == -1) {
        log_event(ERROR, "IPC", "MQ_RM_FAIL", "Failed to remove MQ_LAB");
        result = -1;
    }
    if (mq_responses_id != -1 && msgctl(mq_responses_id, IPC_RMID, NULL) == -1) {
        log_event(ERROR, "IPC", "MQ_RM_FAIL", "Failed to remove MQ_RESPONSES");
        result = -1;
    }

    if (result == 0) {
        log_event(INFO, "IPC", "MQ_REMOVED", "All Message Queues successfully removed");
    }
    return result;
}

/**
 * Send a message to a MQ
 * @param mq_id Destin's Q ID
 * @param msg_ptr Ptr to the message's struct
 * @param total_struct_size sizeof(MSG_TYPE)
 * @return 0 success, -1 failure
 */
int send_generic_message(int mq_id, const void *msg_ptr, size_t total_struct_size) {
    size_t payload_size = MSG_SIZE_CALC(total_struct_size); 

    // The last argument 0 indicates BLOCKING behavior (waits if the queue is full)
    if (msgsnd(mq_id, msg_ptr, payload_size, 0) == -1) {
        char desc[128];
        snprintf(desc, sizeof(desc), "Failed to send message to MQ %d: %s", mq_id, strerror(errno));
        log_event(ERROR, "IPC", "MSG_SEND_FAIL", desc);
        return -1;
    }
    return 0;
}

/**
 * Receives a message from a Message Queue with priority support.
 * @param mq_id Queue ID.
 * @param msg_buffer Pointer to the buffer where the message will be stored.
 * @param total_struct_size sizeof(MESSAGE_TYPE).
 * @param max_priority_type The maximum message type (mtype) to accept. (1=CRITICAL, 2=HIGH,, 3=NORMAL)
 * We use this NEGATIVE value in msgrcv.
 * @return 0 on success, -1 on failure.
 */
int receive_generic_message(int mq_id, void *msg_buffer, size_t total_struct_size, long max_priority_type) {
    size_t payload_size = MSG_SIZE_CALC(total_struct_size);
    long msgtyp_priority = MAX_PRIORITY_TO_ACCEPT(max_priority_type);
    
    // msgrcv with negative msgtyp: Reads any message with mtype <= |msgtyp|
    // 0 in the last argument: BLOCKING behavior.
    ssize_t result = msgrcv(mq_id, msg_buffer, payload_size, msgtyp_priority, 0);

    if (result == -1) {
        // EINTR: signal interruption
        if (errno != EINTR) {
            char desc[128];
            snprintf(desc, sizeof(desc), "Failed to receive message from MQ %d: %s", mq_id, strerror(errno));
            log_event(ERROR, "IPC", "MSG_RCV_FAIL", desc);
        }
        return -1;
    }
    // Success, result > 0 is the size of the received message
    return 0;
}
