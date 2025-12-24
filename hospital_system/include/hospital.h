#ifndef HOSPITAL_H
#define HOSPITAL_H

#include <time.h>
#include <pthread.h>

// SHM2: Surgery Block Status
typedef struct {
    int room_id;
    int status; // 0=FREE, 1=OCCUPIED, 2=CLEANING
    char current_patient[15];
    int surgery_start_time;
    int estimated_end_time;
    pthread_mutex_t mutex;
} surgery_room_t;

typedef struct {
    surgery_room_t rooms[3];
    int medical_teams_available; // Min: 0; Max: 2
    pthread_mutex_t teams_mutex;
} surgery_block_shm_t;


// SHM3: Pharmacy Stock
typedef struct {
    char name[30];
    int current_stock;
    int reserved;
    int threshold;
    int max_capacity;
    pthread_mutex_t mutex;
} medication_stock_t;

typedef struct {
    medication_stock_t medications[15];
    int total_active_requests;
    pthread_mutex_t global_mutex;
} pharmacy_shm_t;


//  SHM4: Lab Queues
typedef struct {
    char request_id[15];
    char patient_id[15];
    int test_type;
    int priority;
    int status; // 0=pending, 1=processing, 2=done
    time_t request_time;
    time_t completion_time;
} lab_request_entry_t;

typedef struct {
    lab_request_entry_t queue_lab1[50];
    lab_request_entry_t queue_lab2[50];
    int lab1_count;
    int lab2_count;
    int lab1_available_slots; // 0-2
    int lab2_available_slots; // 0-2
    pthread_mutex_t lab1_mutex;
    pthread_mutex_t lab2_mutex;
} lab_queue_shm_t;


#endif