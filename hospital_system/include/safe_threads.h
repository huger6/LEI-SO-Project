#ifndef SAFE_THREADS_H
#define SAFE_THREADS_H

#include <pthread.h>

// Threads
int safe_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg);
int safe_pthread_join(pthread_t thread, void **retval);
int safe_pthread_detach(pthread_t thread);

// Mutexes
int safe_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int safe_pthread_mutex_lock(pthread_mutex_t *mutex);
int safe_pthread_mutex_unlock(pthread_mutex_t *mutex);
int safe_pthread_mutex_destroy(pthread_mutex_t *mutex);

// Condition Variables
int safe_pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int safe_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int safe_pthread_cond_signal(pthread_cond_t *cond);
int safe_pthread_cond_broadcast(pthread_cond_t *cond);
int safe_pthread_cond_destroy(pthread_cond_t *cond);

#endif
