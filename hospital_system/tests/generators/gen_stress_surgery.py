#!/usr/bin/env python3
"""
gen_stress_surgery.py - Generate stress test commands for the Surgery module.

Creates 30+ Surgery commands with valid types/meds to saturate the 
Operating Rooms and medical teams.
"""

import sys
import os
import random

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.hospital_utils import (
    generate_surgery, random_patient_id, generate_time_sequence,
    write_commands_to_file, SPECIALTIES, URGENCY_LEVELS,
    random_tests, random_medications
)


def generate_surgery_stress_commands(
    num_commands: int = 35,
    stagger_scheduling: bool = True,
    output_file: str = None
) -> list:
    """
    Generate stress test commands for the surgery module.
    
    Args:
        num_commands: Total number of surgery commands to generate
        stagger_scheduling: If True, vary scheduled times to create queue pressure
        output_file: Optional file to write commands to
    
    Returns:
        List of command strings
    """
    commands = []
    
    # Generate init timestamps with moderate gaps
    times = generate_time_sequence(start=0, count=num_commands, max_gap=8)
    
    patient_counter = 1
    
    # Track scheduled times to create contention
    base_scheduled = 100
    
    for i in range(num_commands):
        patient_id = random_patient_id(patient_counter)
        init_time = times[i]
        
        # Cycle through surgery types to test all operating rooms
        surgery_type = SPECIALTIES[i % len(SPECIALTIES)]
        
        # Vary urgency with bias toward higher urgency for stress
        urgency_weights = [0.4, 0.35, 0.25]  # HIGH, MEDIUM, LOW
        urgency = random.choices(URGENCY_LEVELS, weights=[0.25, 0.35, 0.40])[0]
        
        # Schedule surgeries with overlapping times to create queue pressure
        if stagger_scheduling:
            # Group surgeries to compete for same time slots
            scheduled_offset = random.randint(80, 200)
            if i % 3 == 0:
                # Some surgeries scheduled at same time
                scheduled_time = base_scheduled + (i // 3) * 50
            else:
                scheduled_time = init_time + scheduled_offset
        else:
            scheduled_time = init_time + random.randint(100, 300)
        
        # Always include PREOP + random additional tests
        tests = random_tests(min_count=1, max_count=3, force_preop=True)
        
        # Require medications for surgery
        meds = random_medications(min_count=2, max_count=5)
        
        cmd = generate_surgery(
            patient_id=patient_id,
            init_time=init_time,
            surgery_type=surgery_type,
            scheduled_time=scheduled_time,
            urgency=urgency,
            tests=tests,
            meds=meds
        )
        
        commands.append(cmd)
        patient_counter += 1
    
    if output_file:
        write_commands_to_file(commands, output_file)
    
    return commands


def main():
    """Generate surgery stress test file."""
    output_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output_file = os.path.join(output_dir, "generated", "stress_surgery.txt")
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    
    commands = generate_surgery_stress_commands(
        num_commands=35,
        stagger_scheduling=True,
        output_file=output_file
    )
    
    print(f"Surgery stress test: {len(commands)} commands")
    print(f"Output: {output_file}")
    
    return output_file


if __name__ == "__main__":
    main()
