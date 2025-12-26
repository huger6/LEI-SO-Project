#!/bin/bash
# ============================================================================
# run_surgery_test.sh - Dedicated surgery stress test runner
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/run_test.sh" surgery "$@"
