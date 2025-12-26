#!/bin/bash
# ============================================================================
# run_lab_pharm_test.sh - Dedicated lab/pharmacy stress test runner
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/run_test.sh" lab_pharm "$@"
