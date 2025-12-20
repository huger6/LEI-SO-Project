#ifndef CONSOLE_INPUT_H
#define CONSOLE_INPUT_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> 

// --- Function Headers ---

void process_console_input(void);

// Helpers
int get_med_id(const char *name);
int get_test_id(const char *name);
int get_specialty_id(const char *name);
int get_urgency_id(const char *name);
int parse_list_ids(char *str, int *ids, int max_count, int (*map_func)(const char*));
int parse_med_qty_list(char *str, int *ids, int *qtys, int max_count);




#endif