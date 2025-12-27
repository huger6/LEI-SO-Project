#!/usr/bin/env python3
"""
hospital_utils.py - Shared utility module for hospital simulation testing.

Provides data constants and helper functions for generating valid/invalid
test commands based on the system's configuration.
"""

import random
from typing import List, Optional

# ============================================================================
# SYSTEM DATA CONSTANTS (from config.cfg and command_handler.c)
# ============================================================================

# Valid medications (must match config.cfg and pharmacy SHM)
MEDICATIONS = [
    "ANALGESICO_A", "ANTIBIOTICO_B", "ANESTESICO_C", "SEDATIVO_D",
    "ANTIINFLAMATORIO_E", "CARDIOVASCULAR_F", "NEUROLOGICO_G", "ORTOPEDICO_H",
    "HEMOSTATIC_I", "ANTICOAGULANTE_J", "INSULINA_K", "ANALGESICO_FORTE_L",
    "ANTIBIOTICO_FORTE_M", "VITAMINA_N", "SUPLEMENTO_O"
]

# Lab tests by laboratory capability
TESTS_LAB1 = ["HEMO", "GLIC"]               # Hematology lab
TESTS_LAB2 = ["COLEST", "RENAL", "HEPAT"]   # Biochemistry lab
TESTS_ALL = TESTS_LAB1 + TESTS_LAB2 + ["PREOP"]  # PREOP requires BOTH labs

# Doctor specialties / Surgery types (used by SURGERY and APPOINTMENT)
SPECIALTIES = ["CARDIO", "ORTHO", "NEURO"]

# Urgency levels for surgery
URGENCY_LEVELS = ["LOW", "MEDIUM", "HIGH"]

# Priority levels
PHARMACY_PRIORITIES = ["URGENT", "HIGH", "NORMAL"]
LAB_PRIORITIES = ["URGENT", "NORMAL"]

# Lab identifiers
LAB_TYPES = ["LAB1", "LAB2", "BOTH"]

# Status command components
STATUS_COMPONENTS = ["ALL", "TRIAGE", "SURGERY", "PHARMACY", "LAB"]


# ============================================================================
# RANDOM DATA GENERATORS
# ============================================================================

def random_bool(probability: float = 0.5) -> bool:
    """Return True with the given probability (0.0 to 1.0)."""
    return random.random() < probability


def random_patient_id(index: int) -> str:
    """Generate a valid patient ID: PAC{number}"""
    return f"PAC{index:03d}"


def random_request_id(index: int) -> str:
    """Generate a valid pharmacy request ID: REQ{number}"""
    return f"REQ{index:03d}"


def random_lab_id(index: int) -> str:
    """Generate a valid lab request ID: LAB{number}"""
    return f"LAB{index:03d}"


def random_medications(count: Optional[int] = None, min_count: int = 0, max_count: int = 5) -> List[str]:
    """
    Generate a random list of valid medications.
    
    Args:
        count: Exact count (overrides min/max if provided)
        min_count: Minimum number of medications
        max_count: Maximum number of medications
    
    Returns:
        List of medication names
    """
    if count is not None:
        k = min(count, len(MEDICATIONS))
    else:
        k = random.randint(min_count, min(max_count, len(MEDICATIONS)))
    
    return random.sample(MEDICATIONS, k) if k > 0 else []


def random_tests(
    count: Optional[int] = None,
    min_count: int = 0,
    max_count: int = 3,
    force_preop: bool = False,
    exclude_preop: bool = False,
    lab_compatible: Optional[str] = None
) -> List[str]:
    """
    Generate a random list of valid tests.
    
    Args:
        count: Exact count (overrides min/max if provided)
        min_count: Minimum number of tests
        max_count: Maximum number of tests
        force_preop: Always include PREOP test
        exclude_preop: Never include PREOP test
        lab_compatible: Restrict to tests compatible with LAB1/LAB2/BOTH
    
    Returns:
        List of test names
    """
    # Determine available pool
    if lab_compatible == "LAB1":
        pool = TESTS_LAB1.copy()
    elif lab_compatible == "LAB2":
        pool = TESTS_LAB2.copy()
    else:
        pool = TESTS_ALL.copy()
    
    if exclude_preop and "PREOP" in pool:
        pool.remove("PREOP")
    
    # Determine count
    if count is not None:
        k = min(count, len(pool))
    else:
        k = random.randint(min_count, min(max_count, len(pool)))
    
    tests = random.sample(pool, k) if k > 0 else []
    
    if force_preop and "PREOP" not in tests:
        tests.append("PREOP")
    
    return tests


def random_med_quantities(
    count: Optional[int] = None,
    min_count: int = 1,
    max_count: int = 5,
    min_qty: int = 1,
    max_qty: int = 10
) -> List[tuple]:
    """
    Generate random medication:quantity pairs.
    
    Returns:
        List of (medication_name, quantity) tuples
    """
    meds = random_medications(count, min_count, max_count)
    return [(med, random.randint(min_qty, max_qty)) for med in meds]


def random_specialty() -> str:
    """Return a random doctor specialty / surgery type."""
    return random.choice(SPECIALTIES)


def random_urgency() -> str:
    """Return a random urgency level."""
    return random.choice(URGENCY_LEVELS)


def random_pharmacy_priority() -> str:
    """Return a random pharmacy priority."""
    return random.choice(PHARMACY_PRIORITIES)


def random_lab_priority() -> str:
    """Return a random lab priority."""
    return random.choice(LAB_PRIORITIES)


def random_lab_type() -> str:
    """Return a random lab type."""
    return random.choice(LAB_TYPES)


def random_triage_level() -> int:
    """Return a valid triage priority (1-5, where 1 is most urgent)."""
    return random.randint(1, 5)


def random_stability(critical_threshold: int = 50) -> int:
    """
    Return a random stability value.
    Values below critical_threshold trigger critical patient handling.
    """
    return random.randint(100, 1000)


# ============================================================================
# COMMAND FORMATTERS
# ============================================================================

def format_list(items: List[str]) -> str:
    """Format a list for command syntax: [item1,item2,...]"""
    return "[" + ",".join(items) + "]"


def format_med_qty_list(items: List[tuple]) -> str:
    """Format medication:quantity list: [med1:qty1,med2:qty2,...]"""
    return "[" + ",".join(f"{med}:{qty}" for med, qty in items) + "]"


# ============================================================================
# COMMAND GENERATORS
# ============================================================================

def generate_emergency(
    patient_id: str,
    init_time: int,
    triage: Optional[int] = None,
    stability: Optional[int] = None,
    tests: Optional[List[str]] = None,
    meds: Optional[List[str]] = None
) -> str:
    """
    Generate an EMERGENCY command.
    
    Syntax: EMERGENCY <id> init: <int> triage: <1-5> stability: <int> 
            tests: <list> meds: <list>
    """
    triage = triage if triage is not None else random_triage_level()
    stability = stability if stability is not None else random_stability()
    tests = tests if tests is not None else random_tests(max_count=3, exclude_preop=True)
    meds = meds if meds is not None else random_medications(max_count=3)
    
    parts = [
        f"EMERGENCY {patient_id}",
        f"init: {init_time}",
        f"triage: {triage}",
        f"stability: {stability}",
        f"tests: {format_list(tests)}",
        f"meds: {format_list(meds)}"
    ]
    return " ".join(parts)


def generate_appointment(
    patient_id: str,
    init_time: int,
    scheduled_time: Optional[int] = None,
    doctor: Optional[str] = None,
    tests: Optional[List[str]] = None
) -> str:
    """
    Generate an APPOINTMENT command.
    
    Syntax: APPOINTMENT <id> init: <int> scheduled: <int> doctor: <type> 
            tests: <list>
    
    Note: scheduled_time must be > init_time (validation requirement).
    """
    # Ensure scheduled_time > init_time (command_handler validates: scheduled > current_time + init)
    if scheduled_time is None:
        scheduled_time = init_time + random.randint(50, 200)
    elif scheduled_time <= init_time:
        # Fix invalid scheduled_time by adding minimum offset
        scheduled_time = init_time + max(1, random.randint(20, 100))
    
    doctor = doctor if doctor is not None else random_specialty()
    tests = tests if tests is not None else random_tests(max_count=2, exclude_preop=True)
    
    parts = [
        f"APPOINTMENT {patient_id}",
        f"init: {init_time}",
        f"scheduled: {scheduled_time}",
        f"doctor: {doctor}",
        f"tests: {format_list(tests)}"
    ]
    return " ".join(parts)


def generate_surgery(
    patient_id: str,
    init_time: int,
    surgery_type: Optional[str] = None,
    scheduled_time: Optional[int] = None,
    urgency: Optional[str] = None,
    tests: Optional[List[str]] = None,
    meds: Optional[List[str]] = None
) -> str:
    """
    Generate a SURGERY command.
    
    Syntax: SURGERY <id> init: <int> type: <type> scheduled: <int> 
            urgency: <level> tests: <list> meds: <list>
    
    Note: PREOP test is required for valid surgeries.
    Note: scheduled_time must be >= init_time.
    Note: At least 1 medication is required.
    """
    surgery_type = surgery_type if surgery_type is not None else random_specialty()
    
    # Ensure scheduled_time >= init_time (command_handler validates: scheduled >= init)
    if scheduled_time is None:
        scheduled_time = init_time + random.randint(100, 300)
    elif scheduled_time < init_time:
        # Fix invalid scheduled_time by adding minimum offset
        scheduled_time = init_time + random.randint(50, 150)
    
    urgency = urgency if urgency is not None else random_urgency()
    
    # Always include PREOP test (required for surgery)
    if tests is None:
        tests = random_tests(min_count=1, max_count=3, force_preop=True)
    elif "PREOP" not in tests:
        tests = list(tests) + ["PREOP"]
    
    # Ensure at least 1 medication (required for surgery)
    if meds is None:
        meds = random_medications(min_count=1, max_count=4)
    elif len(meds) == 0:
        meds = random_medications(min_count=1, max_count=3)
    
    parts = [
        f"SURGERY {patient_id}",
        f"init: {init_time}",
        f"type: {surgery_type}",
        f"scheduled: {scheduled_time}",
        f"urgency: {urgency}",
        f"tests: {format_list(tests)}",
        f"meds: {format_list(meds)}"
    ]
    return " ".join(parts)


def generate_lab_request(
    lab_id: str,
    init_time: int,
    priority: Optional[str] = None,
    lab_type: Optional[str] = None,
    tests: Optional[List[str]] = None
) -> str:
    """
    Generate a LAB_REQUEST command.
    
    Syntax: LAB_REQUEST <id> init: <int> priority: <level> lab: <LAB1/LAB2/BOTH> 
            tests: <list>
    
    Note: Tests must be compatible with lab type:
      - LAB1: HEMO, GLIC only
      - LAB2: COLEST, RENAL, HEPAT only  
      - BOTH: any test including PREOP
    """
    priority = priority if priority is not None else random_lab_priority()
    lab_type = lab_type if lab_type is not None else random_lab_type()
    
    # Generate or validate tests for lab compatibility
    if tests is None:
        tests = random_tests(min_count=1, max_count=3, lab_compatible=lab_type)
    else:
        # Validate provided tests are compatible with lab_type
        if lab_type == "LAB1":
            valid_tests = [t for t in tests if t in TESTS_LAB1]
            if not valid_tests:
                valid_tests = random_tests(min_count=1, max_count=2, lab_compatible="LAB1")
            tests = valid_tests
        elif lab_type == "LAB2":
            valid_tests = [t for t in tests if t in TESTS_LAB2]
            if not valid_tests:
                valid_tests = random_tests(min_count=1, max_count=2, lab_compatible="LAB2")
            tests = valid_tests
        # BOTH accepts any tests
    
    # Ensure at least one test (required)
    if not tests:
        tests = random_tests(min_count=1, max_count=2, lab_compatible=lab_type)
    
    parts = [
        f"LAB_REQUEST {lab_id}",
        f"init: {init_time}",
        f"priority: {priority}",
        f"lab: {lab_type}",
        f"tests: {format_list(tests)}"
    ]
    return " ".join(parts)


def generate_pharmacy_request(
    request_id: str,
    init_time: int,
    priority: Optional[str] = None,
    items: Optional[List[tuple]] = None
) -> str:
    """
    Generate a PHARMACY_REQUEST command.
    
    Syntax: PHARMACY_REQUEST <id> init: <int> priority: <level> 
            items: <med1:qty1,med2:qty2,...>
    """
    priority = priority if priority is not None else random_pharmacy_priority()
    items = items if items is not None else random_med_quantities(min_count=1, max_count=5)
    
    parts = [
        f"PHARMACY_REQUEST {request_id}",
        f"init: {init_time}",
        f"priority: {priority}",
        f"items: {format_med_qty_list(items)}"
    ]
    return " ".join(parts)


def generate_restock(medication: Optional[str] = None, quantity: Optional[int] = None) -> str:
    """
    Generate a RESTOCK command.
    
    Syntax: RESTOCK <medication_name> quantity: <amount>
    """
    medication = medication if medication is not None else random.choice(MEDICATIONS)
    quantity = quantity if quantity is not None else random.randint(10, 100)
    
    return f"RESTOCK {medication} quantity: {quantity}"


def generate_status(component: Optional[str] = None) -> str:
    """
    Generate a STATUS command.
    
    Syntax: STATUS <component>
    """
    component = component if component is not None else random.choice(STATUS_COMPONENTS)
    return f"STATUS {component}"


# ============================================================================
# BATCH COMMAND UTILITIES
# ============================================================================

def write_commands_to_file(commands: List[str], filepath: str) -> None:
    """Write a list of commands to a file, one per line."""
    with open(filepath, 'w') as f:
        for cmd in commands:
            f.write(cmd + '\n')
    print(f"Generated {len(commands)} commands -> {filepath}")


def generate_time_sequence(start: int = 0, count: int = 100, max_gap: int = 5) -> List[int]:
    """
    Generate a sequence of init times with random gaps.
    
    Args:
        start: Starting time
        count: Number of timestamps to generate
        max_gap: Maximum gap between consecutive times
    
    Returns:
        List of incrementing timestamps
    """
    times = []
    current = start
    for _ in range(count):
        times.append(current)
        current += random.randint(0, max_gap)
    return times
