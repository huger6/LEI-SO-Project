#!/bin/bash
# ============================================================================
# run_test.sh - Generic test runner for hospital simulation stress tests
# 
# Usage: ./run_test.sh <generator_name> [options]
#
# Examples:
#   ./run_test.sh triage          # Run triage stress test
#   ./run_test.sh surgery         # Run surgery stress test
#   ./run_test.sh lab_pharm       # Run lab/pharmacy stress test
#   ./run_test.sh global          # Run global chaos test
#   ./run_test.sh all             # Run all tests sequentially
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTS_DIR="$(dirname "$SCRIPT_DIR")"
PROJECT_DIR="$(dirname "$TESTS_DIR")"
GENERATORS_DIR="$TESTS_DIR/generators"
GENERATED_DIR="$TESTS_DIR/generated"
INPUT_PIPE="$PROJECT_DIR/input_pipe"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored message
print_msg() {
    local color=$1
    local msg=$2
    echo -e "${color}[TEST] ${msg}${NC}"
}

# Check if input_pipe exists
check_pipe() {
    if [ ! -p "$INPUT_PIPE" ]; then
        print_msg "$RED" "ERROR: input_pipe not found at $INPUT_PIPE"
        print_msg "$YELLOW" "Make sure the hospital_system is running first!"
        exit 1
    fi
    print_msg "$GREEN" "Found input_pipe: $INPUT_PIPE"
}

# Run a specific generator and inject commands
run_generator() {
    local gen_name=$1
    local gen_script="$GENERATORS_DIR/gen_stress_${gen_name}.py"
    local output_file="$GENERATED_DIR/stress_${gen_name}.txt"
    
    if [ ! -f "$gen_script" ]; then
        print_msg "$RED" "ERROR: Generator not found: $gen_script"
        return 1
    fi
    
    print_msg "$BLUE" "Running generator: gen_stress_${gen_name}.py"
    
    # Ensure generated directory exists
    mkdir -p "$GENERATED_DIR"
    
    # Run the Python generator
    cd "$TESTS_DIR"
    python3 "$gen_script"
    
    if [ ! -f "$output_file" ]; then
        print_msg "$RED" "ERROR: Generated file not found: $output_file"
        return 1
    fi
    
    # Count commands
    local cmd_count=$(wc -l < "$output_file")
    print_msg "$GREEN" "Generated $cmd_count commands"
    
    # Inject commands into the pipe
    print_msg "$BLUE" "Injecting commands into input_pipe..."
    cat "$output_file" > "$INPUT_PIPE"
    
    print_msg "$GREEN" "✓ $gen_name stress test commands injected successfully"
    echo ""
}

# Display usage
usage() {
    echo "Usage: $0 <test_type> [options]"
    echo ""
    echo "Test types:"
    echo "  triage           - Triage module stress test (Emergency + Appointments)"
    echo "  surgery          - Surgery module stress test (Operating rooms saturation)"
    echo "  lab_pharm        - Lab and Pharmacy stress test (High volume requests)"
    echo "  pharmacy_restock - Pharmacy stock depletion test (auto_restock OFF)"
    echo "  global           - Global chaos test (All command types mixed)"
    echo "  all              - Run all stress tests sequentially"
    echo ""
    echo "Options:"
    echo "  --no-check  - Skip input_pipe existence check"
    echo "  --dry-run   - Generate files but don't inject into pipe"
    echo ""
    echo "Example:"
    echo "  # First, start the hospital system in another terminal:"
    echo "  cd $PROJECT_DIR && ./hospital_system"
    echo ""
    echo "  # Then run a stress test:"
    echo "  $0 global"
}

# Main
main() {
    local test_type="${1:-}"
    local skip_check=false
    local dry_run=false
    
    # Parse options
    for arg in "$@"; do
        case $arg in
            --no-check)
                skip_check=true
                ;;
            --dry-run)
                dry_run=true
                ;;
        esac
    done
    
    if [ -z "$test_type" ]; then
        usage
        exit 1
    fi
    
    echo ""
    print_msg "$BLUE" "Hospital Simulation Stress Test Runner"
    print_msg "$BLUE" "======================================"
    echo ""
    
    # Check pipe unless skipped or dry run
    if [ "$dry_run" = false ] && [ "$skip_check" = false ]; then
        check_pipe
    fi
    
    echo ""
    
    case "$test_type" in
        triage)
            run_generator "triage"
            ;;
        surgery)
            run_generator "surgery"
            ;;
        lab_pharm)
            run_generator "lab_pharm"
            ;;
        pharmacy_restock)
            run_generator "pharmacy_restock"
            ;;
        global)
            run_generator "global"
            ;;
        all)
            print_msg "$YELLOW" "Running ALL stress tests..."
            echo ""
            run_generator "triage"
            sleep 2
            run_generator "surgery"
            sleep 2
            run_generator "lab_pharm"
            sleep 2
            run_generator "global"
            print_msg "$GREEN" "✓ All stress tests completed!"
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            print_msg "$RED" "Unknown test type: $test_type"
            usage
            exit 1
            ;;
    esac
    
    if [ "$dry_run" = true ]; then
        print_msg "$YELLOW" "(Dry run - commands not injected into pipe)"
    fi
}

main "$@"
