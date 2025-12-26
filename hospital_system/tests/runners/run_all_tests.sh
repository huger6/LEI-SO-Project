#!/bin/bash
# ============================================================================
# run_all_tests.sh - Run all stress tests sequentially
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/run_test.sh" all "$@"
