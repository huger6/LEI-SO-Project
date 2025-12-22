CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -pthread
LDFLAGS = -pthread

SRC_DIR = src
OBJ_DIR = obj

# List of source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
# List of object files
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# Name of the executable
EXEC = hospital_system

.PHONY: all clean debug

all: $(EXEC)

debug: CFLAGS += -DDEBUG -g
debug: all

$(EXEC): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(EXEC)
	[ -d results ] && find results -type f -delete || true
	rm -f logs/hospital_log.txt

ipc_clean:
	@echo "Removing IPC resources for user $$(whoami)"
	-@ipcs -m | grep $$(whoami) | awk '{print $$2}' | xargs -r ipcrm -m 2>/dev/null || true
	
	-@ipcs -s | grep $$(whoami) | awk '{print $$2}' | xargs -r ipcrm -s 2>/dev/null || true
	
	-@ipcs -q | grep $$(whoami) | awk '{print $$2}' | xargs -r ipcrm -q 2>/dev/null || true
	
	-@rm -f /dev/shm/* 2>/dev/null || true
	-@rm -f /dev/mqueue/* 2>/dev/null || true
	
	@echo "Cleanup completed."

# --- Tests ---

# Memory leaks
check_memory: $(EXEC)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --log-file=valgrind_mem.log ./$(EXEC)

# Race conditions
check_threads: $(EXEC)
	valgrind --tool=helgrind --log-file=valgrind_helgrind.log ./$(EXEC)

# Deadlocks
check_deadlock: $(EXEC)
	valgrind --tool=drd --log-file=valgrind_drd.log ./$(EXEC)