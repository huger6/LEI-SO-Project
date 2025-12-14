#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>

#include "../include/ipc.h" 
#include "../include/log.h"
#include "../include/hospital.h"


// External globals for IDs
extern int mq_urgent_id, mq_normal_id, mq_resp_id;
extern int shm_stats_id, shm_surgery_id, shm_pharm_id, shm_lab_id, shm_log_id;


// int create_message_queues() {
//     mq_triage = 
// }

int send_message(int mqid, hospital_message_t *msg) {

}

int receive_message_priority(int mqid, hospital_message_t *msg) {

}

void cleanup_message_queues() {

}