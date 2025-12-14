#ifndef SYNC_H
#define SYNC_H

#include <pthread.h>
#include <semaphore.h>

// Semaphores
#define SEM_BO1             "/sem_surgery_bo1"
#define SEM_BO2             "/sem_surgery_bo2"
#define SEM_BO3             "/sem_surgery_bo3"
#define SEM_MED_TEAMS       "/sem_medical_teams"
#define SEM_PHARM           "/sem_pharmacy_access"
#define SEM_LAB1            "/sem_lab1_equipment"
#define SEM_LAB2            "/sem_lab2_equipment"

// Global mutex's
extern pthread_mutex_t log_mutex;



void create_semaphores();
void cleanup_semaphores();

void mutex_lock(pthread_mutex_t *mutex);
void mutex_unlock(pthread_mutex_t *mutex);

void safe_sem_wait(sem_t *sem);
void safe_sem_post(sem_t *sem);

void init_shm_mutex(pthread_mutex_t *mutex);

#endif