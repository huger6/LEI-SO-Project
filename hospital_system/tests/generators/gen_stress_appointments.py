#!/usr/bin/env python3
"""
gen_stress_appointments.py - Generate stress test commands for Appointments only.

Creates a high volume of APPOINTMENT commands with tight scheduling to stress
the triage and consultation handling systems.
"""

import sys
import os
import random

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.hospital_utils import (
    generate_appointment, random_patient_id, generate_time_sequence,
    write_commands_to_file, SPECIALTIES, random_tests
)


def generate_appointment_stress_commands(
    num_commands: int = 75,
    tight_scheduling: bool = True,
    overlap_ratio: float = 0.4,
    output_file: str = None
) -> list:
    """
    Generate stress test commands for appointments only.
    
    Args:
        num_commands: Total number of appointment commands to generate
        tight_scheduling: If True, use very close timestamps to stress concurrency
        overlap_ratio: Ratio of appointments scheduled at overlapping times (0.0-1.0)
        output_file: Optional file to write commands to
    
    Returns:
        List of command strings
    """
    commands = []
    
    # Generate init timestamps - tight timing means small gaps
    max_gap = 3 if tight_scheduling else 10
    times = generate_time_sequence(start=0, count=num_commands, max_gap=max_gap)
    
    patient_counter = 1
    
    # Track scheduled times for creating overlaps
    scheduled_slots = []
    base_scheduled_offset = 50
    
    for i in range(num_commands):
        patient_id = random_patient_id(patient_counter)
        init_time = times[i]
        
        # Cycle through specialties to test all doctor types
        doctor = SPECIALTIES[i % len(SPECIALTIES)]
        
        # Determine scheduled time
        if random.random() < overlap_ratio and scheduled_slots:
            # Reuse an existing scheduled time to create contention
            scheduled_time = random.choice(scheduled_slots)
            # Ensure scheduled_time > init_time (validation requirement)
            if scheduled_time <= init_time:
                scheduled_time = init_time + random.randint(20, 80)
        else:
            # Create a new scheduled time slot
            # Command handler validates: scheduled > current_time + init
            # Since current_time starts at 0 and init is added, we need scheduled > init
            min_offset = 20  # Minimum gap between init and scheduled
            max_offset = 150 if tight_scheduling else 300
            scheduled_time = init_time + random.randint(min_offset, max_offset)
            
            # Add to slots for potential reuse
            if len(scheduled_slots) < 20:  # Limit number of tracked slots
                scheduled_slots.append(scheduled_time)
        
        # Generate tests (0-2, no PREOP for appointments)
        tests = random_tests(min_count=0, max_count=2, exclude_preop=True)
        
        cmd = generate_appointment(
            patient_id=patient_id,
            init_time=init_time,
            scheduled_time=scheduled_time,
            doctor=doctor,
            tests=tests
        )
        
        commands.append(cmd)
        patient_counter += 1
    
    if output_file:
        write_commands_to_file(commands, output_file)
    
    return commands


def main():
    """Generate appointment stress test file."""
    output_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output_file = os.path.join(output_dir, "generated", "stress_appointments.txt")
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    
    commands = generate_appointment_stress_commands(
        num_commands=75,
        tight_scheduling=True,
        overlap_ratio=0.4,
        output_file=output_file
    )
    
    print(f"Appointment stress test: {len(commands)} commands")
    print(f"  - Tight scheduling: enabled")
    print(f"  - Overlap ratio: 0.4 (40% appointments share time slots)")
    print(f"Output: {output_file}")
    
    return output_file


if __name__ == "__main__":
    main()
