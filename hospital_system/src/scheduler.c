#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/scheduler.h"
#include "../include/mq.h"
#include "../include/log.h"

typedef struct ScheduledEvent {
    int init_time;
    int mq_id;
    void *msg_data;
    size_t msg_size;
    struct ScheduledEvent *next;
} ScheduledEvent;

static ScheduledEvent *scheduled_events_head = NULL;

void add_scheduled_event(int init_time, int mq_id, void *msg, size_t size) {
    ScheduledEvent *new_event = (ScheduledEvent *)malloc(sizeof(ScheduledEvent));
    if (!new_event) {
        log_event(ERROR, "SCHEDULER", "MALLOC_FAIL", "Failed to allocate memory for scheduled event");
        return;
    }

    new_event->init_time = init_time;
    new_event->mq_id = mq_id;
    new_event->msg_size = size;
    new_event->msg_data = malloc(size);
    if (!new_event->msg_data) {
        log_event(ERROR, "SCHEDULER", "MALLOC_FAIL", "Failed to allocate memory for message data");
        free(new_event);
        return;
    }
    memcpy(new_event->msg_data, msg, size);
    new_event->next = NULL;

    // Insert into linked list (sorted by init_time ascending)
    if (!scheduled_events_head || scheduled_events_head->init_time > init_time) {
        new_event->next = scheduled_events_head;
        scheduled_events_head = new_event;
    } 
    else {
        ScheduledEvent *current = scheduled_events_head;
        // Find the last event that is <= new_event->init_time to maintain stable sort for same times
        while (current->next && current->next->init_time <= init_time) {
            current = current->next;
        }
        new_event->next = current->next;
        current->next = new_event;
    }
    
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Event scheduled for time %d", init_time);
    log_event(DEBUG, "SCHEDULER", "EVENT_ADDED", log_msg);
}

void process_scheduled_events(int current_time) {
    while (scheduled_events_head && scheduled_events_head->init_time <= current_time) {
        ScheduledEvent *current = scheduled_events_head;
        
        // Execute event
        send_generic_message(current->mq_id, current->msg_data, current->msg_size);
        
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Executed event scheduled for %d at time %d", 
                 current->init_time, current_time);
        log_event(DEBUG, "SCHEDULER", "EVENT_EXEC", log_msg);

        // Remove from list (head)
        scheduled_events_head = current->next;
        
        free(current->msg_data);
        free(current);
    }
}

int get_next_scheduled_time(void) {
    if (scheduled_events_head) {
        return scheduled_events_head->init_time;
    }
    return -1;
}

int has_scheduled_events(void) {
    return (scheduled_events_head != NULL);
}
