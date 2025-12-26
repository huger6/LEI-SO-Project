#!/usr/bin/env python3
"""
gen_stress_triage.py - Generate stress test commands for the Triage module.

Creates 50+ mixed Emergency and Appointment commands with tight timestamps
to saturate the triage simultaneous patient handling.
"""

import sys
import os
import random

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.hospital_utils import (
    generate_emergency, generate_appointment,
    random_patient_id, generate_time_sequence,
    write_commands_to_file, random_bool
)


def generate_triage_stress_commands(
    num_commands: int = 60,
    emergency_ratio: float = 0.6,
    tight_timing: bool = True,
    output_file: str = None
) -> list:
    """
    Generate stress test commands for the triage module.
    
    Args:
        num_commands: Total number of commands to generate
        emergency_ratio: Ratio of emergency vs appointment commands (0.0-1.0)
        tight_timing: If True, use very close timestamps to stress concurrency
        output_file: Optional file to write commands to
    
    Returns:
        List of command strings
    """
    commands = []
    
    # Generate timestamps - tight timing means small gaps
    max_gap = 2 if tight_timing else 10
    times = generate_time_sequence(start=0, count=num_commands, max_gap=max_gap)
    
    patient_counter = 1
    
    for i in range(num_commands):
        patient_id = random_patient_id(patient_counter)
        init_time = times[i]
        
        if random_bool(emergency_ratio):
            # Generate EMERGENCY - vary triage levels to test priority handling
            triage_level = random.choice([1, 1, 2, 2, 3, 4, 5])  # Bias toward urgent
            stability = random.randint(100, 800)  # Some near critical threshold
            
            cmd = generate_emergency(
                patient_id=patient_id,
                init_time=init_time,
                triage=triage_level,
                stability=stability
            )
        else:
            # Generate APPOINTMENT - scheduled for various future times
            scheduled_offset = random.randint(30, 150)
            
            cmd = generate_appointment(
                patient_id=patient_id,
                init_time=init_time,
                scheduled_time=init_time + scheduled_offset
            )
        
        commands.append(cmd)
        patient_counter += 1
    
    # Shuffle slightly to mix emergencies and appointments
    # But keep general time ordering
    if num_commands > 10:
        for i in range(0, num_commands - 5, 5):
            chunk = commands[i:i+5]
            random.shuffle(chunk)
            commands[i:i+5] = chunk
    
    if output_file:
        write_commands_to_file(commands, output_file)
    
    return commands


def main():
    """Generate triage stress test file."""
    output_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output_file = os.path.join(output_dir, "generated", "stress_triage.txt")
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    
    commands = generate_triage_stress_commands(
        num_commands=60,
        emergency_ratio=0.65,
        tight_timing=True,
        output_file=output_file
    )
    
    print(f"Triage stress test: {len(commands)} commands")
    print(f"Output: {output_file}")
    
    return output_file


if __name__ == "__main__":
    main()
