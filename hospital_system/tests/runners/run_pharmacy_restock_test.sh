#!/bin/bash
# ============================================================================
# run_pharmacy_restock_test.sh - Pharmacy stock depletion stress test runner
#
# Tests pharmacy behavior when auto_restock is OFF:
#   - Stock depletion under high request volume
#   - Failure handling when stock runs out
#   - Manual RESTOCK command functionality
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/run_test.sh" pharmacy_restock "$@"
