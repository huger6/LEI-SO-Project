#!/bin/bash
# test_basic.sh - Teste de Operação Básica

# 1. Start System in background
./hospital_system &
SYSTEM_PID=$!
echo "[TEST] System started (PID: $SYSTEM_PID)"

# Wait for IPC initialization
sleep 2

# 2. Send Commands
echo "[TEST] Sending basic commands..."
cat <<EOF > input_pipe
EMERGENCY PAC001 init: 0 triage: 3 stability: 500 tests: [HEMO] meds: [ANALG_A]
APPOINTMENT PAC002 init: 5 scheduled: 50 doctor: CARDIO tests: []
SURGERY PAC003 init: 10 type: ORTHO scheduled: 100 urgency: LOW tests: [PREOP] meds: [ANESTESICO_C]
EOF

# 3. Wait for processing (adjust based on your time unit config)
echo "[TEST] Simulation running..."
sleep 15

# 4. Shutdown
echo "[TEST] Sending SHUTDOWN..."
echo "SHUTDOWN" > input_pipe

# Wait for graceful exit
wait $SYSTEM_PID
echo "[TEST] Test finished."