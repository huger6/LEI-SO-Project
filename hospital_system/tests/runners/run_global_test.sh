#!/bin/bash
# ============================================================================
# run_global_test.sh - Dedicated global chaos stress test runner
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/run_test.sh" global "$@"
