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
#include "../include/config.h"
#include "../include/manager_utils.h"


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
        log_event(ERROR, "IPC", "MQ_FAIL", "Failed to generate key for message queue");
        return -1;
    }

    // IPC_CREAT | IPC_EXCL makes creation fail if already exists
    int mq_id = msgget(key, IPC_CREAT | IPC_EXCL | 0666);
    if (mq_id == -1) {
        if (errno == EEXIST) {
            // Q already exists, we try to get ID
            mq_id = msgget(key, 0666);
            if (mq_id == -1) {
                log_event(ERROR, "IPC", "MQ_FAIL", name);
            }
        } 
        else {
            char desc[128];
            snprintf(desc, sizeof(desc), "Failed to create %s: %s", name, strerror(errno));
            log_event(ERROR, "IPC", "MQ_FAIL", desc);
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

    return 0;
}

int remove_all_message_queues() {
    int result = 0;

    if (mq_triage_id != -1 && msgctl(mq_triage_id, IPC_RMID, NULL) == -1) result = -1;
    if (mq_surgery_id != -1 && msgctl(mq_surgery_id, IPC_RMID, NULL) == -1) result = -1;
    if (mq_pharmacy_id != -1 && msgctl(mq_pharmacy_id, IPC_RMID, NULL) == -1) result = -1;
    if (mq_lab_id != -1 && msgctl(mq_lab_id, IPC_RMID, NULL) == -1) result = -1;
    if (mq_responses_id != -1 && msgctl(mq_responses_id, IPC_RMID, NULL) == -1) result = -1;

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
        log_event(ERROR, "IPC", "MSG_FAIL", "Failed to send message");
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
        // EINTR: signal interruption - not an error
        if (errno != EINTR) {
            log_event(ERROR, "IPC", "MSG_FAIL", "Failed to receive message");
        }
        return -1;
    }
    // Success, result > 0 is the size of the received message
    return 0;
}

/**
 * Receives a specific message type from a Message Queue.
 * @param mq_id Queue ID.
 * @param msg_buffer Pointer to the buffer where the message will be stored.
 * @param total_struct_size sizeof(MESSAGE_TYPE).
 * @param message_type The specific message type (mtype) to accept.
 * @return 0 on success, -1 on failure.
 */
int receive_specific_message(int mq_id, void *msg_buffer, size_t total_struct_size, long message_type) {
    size_t payload_size = MSG_SIZE_CALC(total_struct_size);
    
    // msgrcv with positive msgtyp: Reads specific message type
    ssize_t result = msgrcv(mq_id, msg_buffer, payload_size, message_type, 0);

    if (result == -1) {
        // EINTR: signal interruption - not an error
        if (errno != EINTR) {
            log_event(ERROR, "IPC", "MSG_FAIL", "Failed to receive specific message");
        }
        return -1;
    }
    return 0;
}

/**
 * Receives any message with mtype <= max_type from a Message Queue.
 * Useful for receiving messages in an operation_id range.
 * @param mq_id Queue ID.
 * @param msg_buffer Pointer to the buffer where the message will be stored.
 * @param total_struct_size sizeof(MESSAGE_TYPE).
 * @param max_type The maximum mtype to accept. Receives first message with mtype <= max_type.
 * @return 0 on success, -1 on failure.
 */
int receive_message_up_to_type(int mq_id, void *msg_buffer, size_t total_struct_size, long max_type) {
    size_t payload_size = MSG_SIZE_CALC(total_struct_size);
    
    // msgrcv with negative msgtyp: Reads first message with mtype <= |msgtyp|
    // This allows receiving any message in a range (e.g., operation_ids 1000-1002)
    ssize_t result = msgrcv(mq_id, msg_buffer, payload_size, -max_type, 0);

    if (result == -1) {
        // EINTR: signal interruption - not an error
        if (errno != EINTR) {
            log_event(ERROR, "IPC", "MSG_FAIL", "Failed to receive ranged message");
        }
        return -1;
    }
    return 0;
}

