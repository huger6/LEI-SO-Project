#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include <sys/types.h>
#include <semaphore.h>


// --- Sem declaration ---
#define SEM_NAME_BO1            "/sem_surgery_bo1"
#define SEM_NAME_BO2            "/sem_surgery_bo2"
#define SEM_NAME_BO3            "/sem_surgery_bo3"
#define SEM_NAME_TEAMS          "/sem_medical_teams"
#define SEM_NAME_LAB1           "/sem_lab1_equipment"
#define SEM_NAME_LAB2           "/sem_lab2_equipment"
#define SEM_NAME_PHARMACY       "/sem_pharmacy_access"


extern sem_t *sem_bo1;
extern sem_t *sem_bo2;
extern sem_t *sem_bo3;
extern sem_t *sem_medical_teams;
extern sem_t *sem_lab1;
extern sem_t *sem_lab2;
extern sem_t *sem_pharmacy;


// --- Function Headers ---

int init_all_semaphores(void);
void close_all_semaphores(void);
int destroy_all_semaphores(void);
int sem_wait_safe(sem_t *sem, const char *sem_name);
int sem_post_safe(sem_t *sem, const char *sem_name);



#endif