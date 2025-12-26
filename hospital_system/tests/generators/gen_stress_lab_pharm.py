#!/usr/bin/env python3
"""
gen_stress_lab_pharm.py - Generate stress test commands for Lab and Pharmacy modules.

Creates high volumes of direct LAB_REQUEST and PHARMACY_REQUEST commands
to test concurrent processing and resource management.
"""

import sys
import os
import random

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.hospital_utils import (
    generate_lab_request, generate_pharmacy_request, generate_restock,
    random_lab_id, random_request_id, generate_time_sequence,
    write_commands_to_file, LAB_TYPES, random_bool,
    MEDICATIONS
)


def generate_lab_pharm_stress_commands(
    num_lab_requests: int = 40,
    num_pharm_requests: int = 40,
    num_restocks: int = 10,
    include_urgent: bool = True,
    output_file: str = None
) -> list:
    """
    Generate stress test commands for lab and pharmacy modules.
    
    Args:
        num_lab_requests: Number of LAB_REQUEST commands
        num_pharm_requests: Number of PHARMACY_REQUEST commands  
        num_restocks: Number of RESTOCK commands to intersperse
        include_urgent: If True, include URGENT priority requests
        output_file: Optional file to write commands to
    
    Returns:
        List of command strings
    """
    commands = []
    
    total_commands = num_lab_requests + num_pharm_requests + num_restocks
    times = generate_time_sequence(start=0, count=total_commands, max_gap=3)
    
    lab_counter = 1
    pharm_counter = 1
    time_idx = 0
    
    # Generate LAB_REQUEST commands
    for _ in range(num_lab_requests):
        lab_id = random_lab_id(lab_counter)
        init_time = times[time_idx]
        
        # Distribute across lab types
        lab_type = random.choice(LAB_TYPES)
        
        # Priority distribution - more urgent under stress
        if include_urgent and random_bool(0.3):
            priority = "URGENT"
        else:
            priority = "NORMAL"
        
        cmd = generate_lab_request(
            lab_id=lab_id,
            init_time=init_time,
            priority=priority,
            lab_type=lab_type
        )
        
        commands.append(cmd)
        lab_counter += 1
        time_idx += 1
    
    # Generate PHARMACY_REQUEST commands
    for _ in range(num_pharm_requests):
        request_id = random_request_id(pharm_counter)
        init_time = times[time_idx] if time_idx < len(times) else times[-1] + random.randint(1, 5)
        
        # Priority distribution
        if include_urgent and random_bool(0.25):
            priority = "URGENT"
        elif random_bool(0.35):
            priority = "HIGH"
        else:
            priority = "NORMAL"
        
        cmd = generate_pharmacy_request(
            request_id=request_id,
            init_time=init_time,
            priority=priority
        )
        
        commands.append(cmd)
        pharm_counter += 1
        time_idx = min(time_idx + 1, len(times) - 1)
    
    # Generate RESTOCK commands (to replenish depleted stock)
    for _ in range(num_restocks):
        medication = random.choice(MEDICATIONS)
        quantity = random.randint(50, 200)  # Larger restocks for stress test
        
        cmd = generate_restock(medication=medication, quantity=quantity)
        commands.append(cmd)
    
    # Shuffle to interleave lab and pharmacy requests
    random.shuffle(commands)
    
    if output_file:
        write_commands_to_file(commands, output_file)
    
    return commands


def main():
    """Generate lab/pharmacy stress test file."""
    output_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output_file = os.path.join(output_dir, "generated", "stress_lab_pharm.txt")
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    
    commands = generate_lab_pharm_stress_commands(
        num_lab_requests=45,
        num_pharm_requests=45,
        num_restocks=15,
        include_urgent=True,
        output_file=output_file
    )
    
    print(f"Lab/Pharmacy stress test: {len(commands)} commands")
    print(f"Output: {output_file}")
    
    return output_file


if __name__ == "__main__":
    main()
