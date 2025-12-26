#!/usr/bin/env python3
"""
gen_stress_pharmacy_restock.py - Pharmacy stock depletion stress test.

This test generates a high volume of PHARMACY_REQUEST commands targeting
the same medications repeatedly to deplete stock and observe whether
the system properly handles stock exhaustion when auto_restock is OFF.

Purpose:
    - Test pharmacy behavior when stock runs out
    - Verify that auto_restock=OFF actually prevents automatic restocking
    - Observe how pending requests are handled when stock is insufficient
    - Check if manual RESTOCK commands work correctly after depletion

Expected behavior with auto_restock=OFF:
    - Requests should fail or queue when stock is depleted
    - No automatic restocking should occur
    - Manual RESTOCK commands should replenish stock
"""

import sys
import os
import random

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.hospital_utils import (
    generate_pharmacy_request, generate_restock,
    random_request_id, generate_time_sequence,
    write_commands_to_file, MEDICATIONS
)


# Target medications to deplete - we'll focus on a subset to ensure depletion
TARGET_MEDICATIONS = [
    "ANALGESICO_A",
    "ANTIBIOTICO_B", 
    "ANESTESICO_C",
    "SEDATIVO_D",
    "ANTIINFLAMATORIO_E"
]


def generate_pharmacy_restock_stress_commands(
    num_requests: int = 80,
    qty_per_request: int = 5,
    include_manual_restocks: bool = True,
    restock_delay: int = 50,
    output_file: str = None
) -> list:
    """
    Generate pharmacy stress test commands designed to deplete stock.
    
    Args:
        num_requests: Total number of PHARMACY_REQUEST commands
        qty_per_request: Quantity of each medication per request (higher = faster depletion)
        include_manual_restocks: If True, include some manual RESTOCK commands at the end
        restock_delay: Time gap before manual restocks
        output_file: Optional file to write commands to
    
    Returns:
        List of command strings
    """
    commands = []
    
    # Phase 1: Initial burst of requests to deplete stock quickly
    # Use small time gaps to create pressure on the pharmacy
    times = generate_time_sequence(start=0, count=num_requests, max_gap=2)
    
    print(f"=== Pharmacy Stock Depletion Stress Test ===")
    print(f"Target medications: {TARGET_MEDICATIONS}")
    print(f"Total requests: {num_requests}")
    print(f"Quantity per medication: {qty_per_request}")
    print(f"Expected behavior: Stock should run out, requests should fail/queue")
    print()
    
    # Phase 1: First wave - URGENT priority to process immediately
    phase1_count = num_requests // 3
    print(f"Phase 1: {phase1_count} URGENT requests (rapid stock depletion)")
    
    for i in range(phase1_count):
        request_id = random_request_id(i + 1)
        init_time = times[i]
        
        # Target 1-2 specific medications per request with high quantity
        num_meds = random.randint(1, 2)
        meds = random.sample(TARGET_MEDICATIONS, num_meds)
        items = [(med, qty_per_request) for med in meds]
        
        cmd = generate_pharmacy_request(
            request_id=request_id,
            init_time=init_time,
            priority="URGENT",
            items=items
        )
        commands.append(cmd)
    
    # Phase 2: Second wave - HIGH priority 
    phase2_count = num_requests // 3
    phase2_start = phase1_count
    print(f"Phase 2: {phase2_count} HIGH priority requests (continued depletion)")
    
    for i in range(phase2_count):
        idx = phase2_start + i
        request_id = random_request_id(idx + 1)
        init_time = times[idx] if idx < len(times) else times[-1] + i * 2
        
        # Target same medications
        num_meds = random.randint(1, 3)
        meds = random.sample(TARGET_MEDICATIONS, num_meds)
        items = [(med, qty_per_request) for med in meds]
        
        cmd = generate_pharmacy_request(
            request_id=request_id,
            init_time=init_time,
            priority="HIGH",
            items=items
        )
        commands.append(cmd)
    
    # Phase 3: Third wave - NORMAL priority (these should mostly fail if stock depleted)
    phase3_count = num_requests - phase1_count - phase2_count
    phase3_start = phase1_count + phase2_count
    print(f"Phase 3: {phase3_count} NORMAL priority requests (expected failures if depleted)")
    
    for i in range(phase3_count):
        idx = phase3_start + i
        request_id = random_request_id(idx + 1)
        init_time = times[idx] if idx < len(times) else times[-1] + i * 2
        
        # Target same medications
        num_meds = random.randint(1, 2)
        meds = random.sample(TARGET_MEDICATIONS, num_meds)
        items = [(med, qty_per_request) for med in meds]
        
        cmd = generate_pharmacy_request(
            request_id=request_id,
            init_time=init_time,
            priority="NORMAL",
            items=items
        )
        commands.append(cmd)
    
    # Add STATUS command to see pharmacy state after depletion
    last_request_time = times[-1] if times else num_requests * 2
    commands.append(f"STATUS PHARMACY")
    
    # Phase 4: Optional manual restocks to test recovery
    if include_manual_restocks:
        print(f"Phase 4: Manual RESTOCK commands after delay")
        restock_time = last_request_time + restock_delay
        
        for med in TARGET_MEDICATIONS:
            # Restock with a reasonable amount
            cmd = generate_restock(
                medication=med,
                quantity=50
            )
            commands.append(cmd)
        
        # Add more requests after restock to verify recovery
        print(f"Phase 5: Post-restock requests to verify recovery")
        for i in range(10):
            request_id = random_request_id(num_requests + i + 1)
            init_time = restock_time + 10 + i * 3
            
            med = random.choice(TARGET_MEDICATIONS)
            items = [(med, 3)]  # Small quantity
            
            cmd = generate_pharmacy_request(
                request_id=request_id,
                init_time=init_time,
                priority="NORMAL",
                items=items
            )
            commands.append(cmd)
        
        # Final status check
        commands.append(f"STATUS PHARMACY")
    
    print()
    print(f"Total commands generated: {len(commands)}")
    
    # Write to file
    if output_file:
        write_commands_to_file(commands, output_file)
    
    return commands


def main():
    """Generate the pharmacy restock stress test file."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    tests_dir = os.path.dirname(script_dir)
    output_dir = os.path.join(tests_dir, "generated")
    os.makedirs(output_dir, exist_ok=True)
    
    output_file = os.path.join(output_dir, "stress_pharmacy_restock.txt")
    
    # Generate with settings that should quickly deplete stock
    commands = generate_pharmacy_restock_stress_commands(
        num_requests=80,           # 80 pharmacy requests
        qty_per_request=5,         # 5 units per medication per request
        include_manual_restocks=True,
        restock_delay=50,
        output_file=output_file
    )
    
    print(f"\nOutput written to: {output_file}")
    print("\n=== How to interpret results ===")
    print("With auto_restock=OFF:")
    print("  - Early requests should succeed")
    print("  - Later requests should fail with 'insufficient stock'")
    print("  - No automatic restocking should occur")
    print("  - Manual RESTOCK commands should restore stock")
    print("  - Post-restock requests should succeed")
    print("\nWith auto_restock=ON:")
    print("  - Stock should be automatically replenished")
    print("  - Most/all requests should eventually succeed")
    

if __name__ == "__main__":
    main()
