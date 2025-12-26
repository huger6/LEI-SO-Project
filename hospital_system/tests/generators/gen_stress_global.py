"""
gen_stress_global.py - Generate chaotic mixed stress test for system stability.

Creates a high-volume mix of ALL command types to test system-wide concurrency,
inter-module communication, and overall stability under load.
"""

import sys
import os
import random

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.hospital_utils import (
    generate_emergency, generate_appointment, generate_surgery,
    generate_lab_request, generate_pharmacy_request, generate_restock,
    generate_status, random_patient_id, random_lab_id, random_request_id,
    generate_time_sequence, write_commands_to_file, random_bool
)


# Command type weights for distribution
COMMAND_WEIGHTS = {
    'EMERGENCY': 0.20,
    'APPOINTMENT': 0.15,
    'SURGERY': 0.15,
    'LAB_REQUEST': 0.20,
    'PHARMACY_REQUEST': 0.20,
    'RESTOCK': 0.05,
    'STATUS': 0.05
}


def select_command_type() -> str:
    """Select a random command type based on weights."""
    types = list(COMMAND_WEIGHTS.keys())
    weights = list(COMMAND_WEIGHTS.values())
    return random.choices(types, weights=weights)[0]


def generate_global_stress_commands(
    num_commands: int = 150,
    chaos_level: float = 0.7,
    output_file: str = None
) -> list:
    """
    Generate a chaotic mix of all command types.
    
    Args:
        num_commands: Total number of commands to generate
        chaos_level: 0.0-1.0, higher = tighter timing and more concurrent load
        output_file: Optional file to write commands to
    
    Returns:
        List of command strings
    """
    commands = []
    
    # Tighter timing with higher chaos
    max_gap = max(1, int(10 * (1 - chaos_level)))
    times = generate_time_sequence(start=0, count=num_commands, max_gap=max_gap)
    
    # Counters for IDs
    patient_counter = 1
    lab_counter = 1
    pharm_counter = 1
    
    for i in range(num_commands):
        init_time = times[i]
        cmd_type = select_command_type()
        
        if cmd_type == 'EMERGENCY':
            patient_id = random_patient_id(patient_counter)
            # Under chaos, more critical patients
            triage = random.randint(1, 3) if random_bool(chaos_level) else random.randint(1, 5)
            cmd = generate_emergency(
                patient_id=patient_id,
                init_time=init_time,
                triage=triage
            )
            patient_counter += 1
            
        elif cmd_type == 'APPOINTMENT':
            patient_id = random_patient_id(patient_counter)
            # Tighter scheduling under chaos
            scheduled_offset = random.randint(20, 100) if random_bool(chaos_level) else random.randint(50, 200)
            cmd = generate_appointment(
                patient_id=patient_id,
                init_time=init_time,
                scheduled_time=init_time + scheduled_offset
            )
            patient_counter += 1
            
        elif cmd_type == 'SURGERY':
            patient_id = random_patient_id(patient_counter)
            # More HIGH urgency under chaos
            urgency = "HIGH" if random_bool(chaos_level * 0.5) else None
            scheduled_offset = random.randint(50, 150) if random_bool(chaos_level) else random.randint(100, 300)
            cmd = generate_surgery(
                patient_id=patient_id,
                init_time=init_time,
                scheduled_time=init_time + scheduled_offset,
                urgency=urgency
            )
            patient_counter += 1
            
        elif cmd_type == 'LAB_REQUEST':
            lab_id = random_lab_id(lab_counter)
            # More URGENT under chaos
            priority = "URGENT" if random_bool(chaos_level * 0.4) else None
            cmd = generate_lab_request(
                lab_id=lab_id,
                init_time=init_time,
                priority=priority
            )
            lab_counter += 1
            
        elif cmd_type == 'PHARMACY_REQUEST':
            request_id = random_request_id(pharm_counter)
            # More URGENT/HIGH under chaos
            if random_bool(chaos_level * 0.3):
                priority = "URGENT"
            elif random_bool(chaos_level * 0.4):
                priority = "HIGH"
            else:
                priority = None
            cmd = generate_pharmacy_request(
                request_id=request_id,
                init_time=init_time,
                priority=priority
            )
            pharm_counter += 1
            
        elif cmd_type == 'RESTOCK':
            # Larger restocks under chaos (system needs more resources)
            quantity = random.randint(50, 150) if random_bool(chaos_level) else None
            cmd = generate_restock(quantity=quantity)
            
        elif cmd_type == 'STATUS':
            cmd = generate_status()
            
        else:
            # Fallback to emergency
            patient_id = random_patient_id(patient_counter)
            cmd = generate_emergency(patient_id=patient_id, init_time=init_time)
            patient_counter += 1
        
        commands.append(cmd)
    
    if output_file:
        write_commands_to_file(commands, output_file)
    
    return commands


def main():
    """Generate global stress test file."""
    output_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output_file = os.path.join(output_dir, "generated", "stress_global.txt")
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    
    commands = generate_global_stress_commands(
        num_commands=150,
        chaos_level=0.7,
        output_file=output_file
    )
    
    print(f"Global stress test: {len(commands)} commands")
    print(f"  - Chaos level: 0.7")
    print(f"Output: {output_file}")
    
    return output_file


if __name__ == "__main__":
    main()
