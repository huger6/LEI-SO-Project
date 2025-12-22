#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stddef.h>

// Adds an event to the scheduler, sorted by init_time
void add_scheduled_event(int init_time, int mq_id, void *msg, size_t size);

// Processes all events scheduled for <= current_time
void process_scheduled_events(int current_time);

// Returns the time of the next scheduled event, or -1 if no events are pending
int get_next_scheduled_time(void);

// Returns 1 if there are scheduled events, 0 otherwise
int has_scheduled_events(void);

// Frees all scheduled events
void cleanup_scheduler(void);

#endif
