import random
import datetime
import os

# ============================================================================
# HOSPITAL COMMAND GENERATOR
# Generates random commands for testing, including both valid and invalid cases
# ============================================================================

# --- Valid Values (from command_handler.c) ---
MEDS = [
    "ANALGESICO_A", "ANTIBIOTICO_B", "ANESTESICO_C", "SEDATIVO_D",
    "ANTIINFLAMATORIO_E", "CARDIOVASCULAR_F", "NEUROLOGICO_G", "ORTOPEDICO_H",
    "HEMOSTATIC_I", "ANTICOAGULANTE_J", "INSULINA_K", "ANALGESICO_FORTE_L",
    "ANTIBIOTICO_FORTE_M", "VITAMINA_N", "SUPLEMENTO_O"
]

# Tests compatible with each lab
TESTS_LAB1 = ["HEMO", "GLIC"]              # Hematology
TESTS_LAB2 = ["COLEST", "RENAL", "HEPAT"]  # Biochemistry
TESTS_ALL = TESTS_LAB1 + TESTS_LAB2 + ["PREOP"]  # All tests including PREOP

SURGERY_TYPES = ["CARDIO", "ORTHO", "NEURO"]
URGENCY_LEVELS = ["LOW", "MEDIUM", "HIGH"]
PHARMACY_PRIORITIES = ["URGENT", "HIGH", "NORMAL"]
LAB_PRIORITIES = ["URGENT", "NORMAL"]
LAB_TYPES = ["LAB1", "LAB2", "BOTH"]
STATUS_COMPONENTS = ["ALL", "TRIAGE", "SURGERY", "PHARMACY", "LAB"]

# --- Invalid Values for Testing ---
INVALID_MEDS = ["ASPIRINA", "PARACETAMOL", "IBUPROFENO", "INVALID_MED", ""]
INVALID_TESTS = ["XRAY", "MRI", "ECG", "INVALID_TEST", ""]
INVALID_TYPES = ["PLASTIC", "GENERAL", "DENTAL", "INVALID", ""]
INVALID_URGENCY = ["CRITICAL", "EXTREME", "NONE", "INVALID", ""]
INVALID_PRIORITIES = ["SUPER", "EXTREME", "NONE", "INVALID", ""]
INVALID_ID_FORMATS = ["P001", "PATIENT001", "001", "ABC", "PAC", "PAC-001", ""]

# --- Probability of generating invalid commands (0.0 to 1.0) ---
INVALID_PROBABILITY = 0.15  # 15% chance of generating invalid command


def random_bool(probability=0.5):
    """Return True with the given probability"""
    return random.random() < probability


def get_random_meds(allow_empty=True, allow_invalid=False):
    """Generate a random list of medications"""
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        # Include some invalid medications
        k = random.randint(0, 4)
        meds = random.sample(MEDS + INVALID_MEDS, min(k, len(MEDS + INVALID_MEDS)))
        return "[" + ",".join(meds) + "]"
    
    k = random.randint(0 if allow_empty else 1, 5)
    if k == 0:
        return "[]"
    return "[" + ",".join(random.sample(MEDS, k)) + "]"


def get_random_tests(allow_empty=True, allow_invalid=False, force_preop=False, exclude_preop=False):
    """Generate a random list of tests"""
    available_tests = TESTS_ALL.copy()
    
    if exclude_preop and "PREOP" in available_tests:
        available_tests.remove("PREOP")
    
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        # Include some invalid tests
        k = random.randint(0, 3)
        tests = random.sample(available_tests + INVALID_TESTS, min(k, len(available_tests + INVALID_TESTS)))
        if force_preop and "PREOP" not in tests:
            tests.append("PREOP")
        return "[" + ",".join(tests) + "]"
    
    k = random.randint(0 if allow_empty else 1, 3)
    if k == 0 and not force_preop:
        return "[]"
    
    tests = random.sample(available_tests, min(k, len(available_tests)))
    
    if force_preop and "PREOP" not in tests:
        tests.append("PREOP")
    
    return "[" + ",".join(tests) + "]"


def get_lab_compatible_tests(lab_type, allow_invalid=False):
    """Generate tests compatible with a specific lab type"""
    if lab_type == "LAB1":
        pool = TESTS_LAB1
    elif lab_type == "LAB2":
        pool = TESTS_LAB2
    else:  # BOTH
        pool = TESTS_ALL
    
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        # Mix in incompatible tests to test validation
        all_pool = TESTS_ALL + INVALID_TESTS
        k = random.randint(1, 3)
        tests = random.sample(all_pool, min(k, len(all_pool)))
        return "[" + ",".join(tests) + "]"
    
    k = random.randint(1, min(3, len(pool)))
    tests = random.sample(pool, k)
    return "[" + ",".join(tests) + "]"


def get_random_med_qty_list(allow_empty=True, allow_invalid=False):
    """Generate medication list with quantities for pharmacy requests"""
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        k = random.randint(0, 4)
        items = []
        for _ in range(k):
            med = random.choice(MEDS + INVALID_MEDS)
            qty = random.choice([1, 2, 5, 10, -1, 0, 100])  # Include invalid quantities
            items.append(f"{med}:{qty}")
        return "[" + ",".join(items) + "]"
    
    k = random.randint(0 if allow_empty else 1, 5)
    if k == 0:
        return "[]"
    items = []
    for med in random.sample(MEDS, k):
        qty = random.randint(1, 10)
        items.append(f"{med}:{qty}")
    return "[" + ",".join(items) + "]"


def get_patient_id(index, allow_invalid=False):
    """Generate patient ID (PAC format)"""
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        return random.choice(INVALID_ID_FORMATS + [f"PAC{index:03d}"])
    return f"PAC{index:03d}"


def get_request_id(index, allow_invalid=False):
    """Generate request ID (REQ format for pharmacy)"""
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        return random.choice(["R001", "REQUEST001", "001", "REQ", "PAC001", ""])
    return f"REQ{index:03d}"


def get_lab_id(index, allow_invalid=False):
    """Generate lab ID (LAB format)"""
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        return random.choice(["L001", "LABORATORY001", "001", "LAB", "PAC001", ""])
    return f"LAB{index:03d}"


# ============================================================================
# COMMAND GENERATORS
# ============================================================================

def generate_emergency(index, time, allow_invalid=False):
    """
    EMERGENCY <patient_id> init: <time> triage: <1-5> stability: <value> 
              [tests: <test1,test2,...>] [meds: <med1,med2,...>]
    """
    patient_id = get_patient_id(index, allow_invalid)
    
    # Decide which fields to include/make invalid
    parts = [f"EMERGENCY {patient_id}"]
    
    # init: (required)
    if allow_invalid and random_bool(0.1):
        # Missing or invalid init
        if random_bool(0.5):
            pass  # Omit init entirely
        else:
            parts.append(f"init: {random.choice([-5, -1, 'abc'])}")
    else:
        parts.append(f"init: {time}")
    
    # triage: (required, 1-5)
    if allow_invalid and random_bool(0.1):
        triage = random.choice([0, 6, 10, -1, "HIGH"])
    else:
        triage = random.randint(1, 5)
    parts.append(f"triage: {triage}")
    
    # stability: (required, >= 100)
    if allow_invalid and random_bool(0.1):
        stability = random.choice([0, 50, 99, -1, "stable"])
    else:
        stability = random.randint(100, 1000)
    parts.append(f"stability: {stability}")
    
    # tests: (optional)
    if random_bool(0.7):
        parts.append(f"tests: {get_random_tests(allow_empty=True, allow_invalid=allow_invalid)}")
    
    # meds: (optional)
    if random_bool(0.7):
        parts.append(f"meds: {get_random_meds(allow_empty=True, allow_invalid=allow_invalid)}")
    
    return " ".join(parts)


def generate_appointment(index, time, allow_invalid=False):
    """
    APPOINTMENT <patient_id> init: <time> scheduled: <time> doctor: <specialty> 
                [tests: <test1,test2,...>]
    """
    patient_id = get_patient_id(index, allow_invalid)
    
    parts = [f"APPOINTMENT {patient_id}"]
    
    # init: (required)
    if allow_invalid and random_bool(0.1):
        if random_bool(0.5):
            pass  # Omit
        else:
            parts.append(f"init: {random.choice([-5, -1, 'abc'])}")
    else:
        parts.append(f"init: {time}")
    
    # scheduled: (required, must be > init + current_time)
    scheduled_time = time + random.randint(50, 200)
    if allow_invalid and random_bool(0.1):
        scheduled_time = random.choice([time - 10, time, 0, -1])
    parts.append(f"scheduled: {scheduled_time}")
    
    # doctor: (required)
    if allow_invalid and random_bool(0.1):
        doctor = random.choice(INVALID_TYPES)
    else:
        doctor = random.choice(SURGERY_TYPES)
    parts.append(f"doctor: {doctor}")
    
    # tests: (optional)
    if random_bool(0.6):
        parts.append(f"tests: {get_random_tests(allow_empty=True, allow_invalid=allow_invalid, exclude_preop=True)}")
    
    return " ".join(parts)


def generate_surgery(index, time, allow_invalid=False):
    """
    SURGERY <patient_id> init: <time> type: <specialty> scheduled: <time> 
            urgency: <level> tests: <test1,test2,...> meds: <med1,med2,...>
    Note: PREOP test is required - we sometimes omit it to test validation
    """
    patient_id = get_patient_id(index, allow_invalid)
    
    parts = [f"SURGERY {patient_id}"]
    
    # init: (required)
    if allow_invalid and random_bool(0.1):
        if random_bool(0.5):
            pass
        else:
            parts.append(f"init: {random.choice([-5, -1, 'abc'])}")
    else:
        parts.append(f"init: {time}")
    
    # type: (required)
    if allow_invalid and random_bool(0.1):
        surgery_type = random.choice(INVALID_TYPES)
    else:
        surgery_type = random.choice(SURGERY_TYPES)
    parts.append(f"type: {surgery_type}")
    
    # scheduled: (required, must be >= init)
    scheduled_time = time + random.randint(100, 300)
    if allow_invalid and random_bool(0.1):
        scheduled_time = random.choice([time - 50, -1, 0])
    parts.append(f"scheduled: {scheduled_time}")
    
    # urgency: (required)
    if allow_invalid and random_bool(0.1):
        urgency = random.choice(INVALID_URGENCY)
    else:
        urgency = random.choice(URGENCY_LEVELS)
    parts.append(f"urgency: {urgency}")
    
    # tests: (required, PREOP must be included)
    # KEY: Sometimes omit PREOP to test validation
    include_preop = not (allow_invalid and random_bool(0.3))  # 30% chance to NOT include PREOP when invalid
    parts.append(f"tests: {get_random_tests(allow_empty=False, allow_invalid=allow_invalid, force_preop=include_preop)}")
    
    # meds: (required, at least 1)
    if allow_invalid and random_bool(0.15):
        parts.append(f"meds: {get_random_meds(allow_empty=True, allow_invalid=allow_invalid)}")  # Might be empty
    else:
        parts.append(f"meds: {get_random_meds(allow_empty=False, allow_invalid=allow_invalid)}")
    
    return " ".join(parts)


def generate_pharmacy_request(index, time, allow_invalid=False):
    """
    PHARMACY_REQUEST <request_id> init: <time> priority: <priority> 
                     items: <med1:qty1,med2:qty2,...>
    """
    request_id = get_request_id(index, allow_invalid)
    
    parts = [f"PHARMACY_REQUEST {request_id}"]
    
    # init: (required)
    if allow_invalid and random_bool(0.1):
        if random_bool(0.5):
            pass
        else:
            parts.append(f"init: {random.choice([-5, -1, 'abc'])}")
    else:
        parts.append(f"init: {time}")
    
    # priority: (required)
    if allow_invalid and random_bool(0.1):
        priority = random.choice(INVALID_PRIORITIES)
    else:
        priority = random.choice(PHARMACY_PRIORITIES)
    parts.append(f"priority: {priority}")
    
    # items: (optional but usually included)
    if random_bool(0.9):
        parts.append(f"items: {get_random_med_qty_list(allow_empty=False, allow_invalid=allow_invalid)}")
    
    return " ".join(parts)


def generate_lab_request(index, time, allow_invalid=False):
    """
    LAB_REQUEST <lab_id> init: <time> priority: <priority> lab: <lab> 
                tests: <test1,test2,...>
    """
    lab_id = get_lab_id(index, allow_invalid)
    
    parts = [f"LAB_REQUEST {lab_id}"]
    
    # init: (required)
    if allow_invalid and random_bool(0.1):
        if random_bool(0.5):
            pass
        else:
            parts.append(f"init: {random.choice([-5, -1, 'abc'])}")
    else:
        parts.append(f"init: {time}")
    
    # priority: (required)
    if allow_invalid and random_bool(0.1):
        priority = random.choice(INVALID_PRIORITIES + ["HIGH"])  # HIGH is invalid for LAB
    else:
        priority = random.choice(LAB_PRIORITIES)
    parts.append(f"priority: {priority}")
    
    # lab: (required)
    if allow_invalid and random_bool(0.1):
        lab_type = random.choice(["LAB3", "HEMATOLOGY", "INVALID", ""])
    else:
        lab_type = random.choice(LAB_TYPES)
    parts.append(f"lab: {lab_type}")
    
    # tests: (required, must be compatible with lab)
    if allow_invalid and random_bool(0.2):
        # Sometimes use incompatible tests
        parts.append(f"tests: {get_random_tests(allow_empty=False, allow_invalid=True)}")
    else:
        parts.append(f"tests: {get_lab_compatible_tests(lab_type, allow_invalid=allow_invalid)}")
    
    return " ".join(parts)


def generate_restock(time, allow_invalid=False):
    """
    RESTOCK <medication_name> quantity: <amount>
    """
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        med = random.choice(MEDS + INVALID_MEDS)
    else:
        med = random.choice(MEDS)
    
    parts = [f"RESTOCK {med}"]
    
    # quantity: (required, > 0)
    if allow_invalid and random_bool(0.1):
        qty = random.choice([0, -5, -1, "abc"])
    else:
        qty = random.randint(10, 100)
    parts.append(f"quantity: {qty}")
    
    return " ".join(parts)


def generate_status(allow_invalid=False):
    """
    STATUS <component>
    """
    if allow_invalid and random_bool(INVALID_PROBABILITY):
        component = random.choice(STATUS_COMPONENTS + ["INVALID", "HOSPITAL", ""])
    else:
        component = random.choice(STATUS_COMPONENTS)
    
    if component:
        return f"STATUS {component}"
    else:
        return "STATUS"  # Missing component


def generate_unknown_command():
    """Generate completely unknown commands"""
    unknown_cmds = [
        "ADMIT PAC001",
        "DISCHARGE PAC001",
        "TRANSFER PAC001 TO SURGERY",
        "CANCEL SURGERY PAC001",
        "PATIENT_INFO PAC001",
        "LIST_PATIENTS",
        "INVALID_COMMAND",
        "RANDOM GARBAGE TEXT",
        "123456",
        "",
    ]
    return random.choice(unknown_cmds)


# ============================================================================
# MAIN GENERATOR
# ============================================================================

def main():
    # Create test directory if it doesn't exist
    if not os.path.exists("test_cases"):
        os.makedirs("test_cases")
    
    # Filename with timestamp
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"sample_cmds/commands_{timestamp}.txt"
    
    commands = []
    current_time = 0
    patient_counter = 1
    request_counter = 1
    lab_counter = 1
    
    # Generate 100 random commands (mix of valid and invalid)
    num_commands = 100
    
    for i in range(num_commands):
        # Decide if this command should be potentially invalid
        allow_invalid = random_bool(INVALID_PROBABILITY * 2)  # Higher chance for variety
        
        # Advance time occasionally
        current_time += random.randint(0, 5)
        
        # Choose command type randomly
        cmd_type = random.randint(1, 9)
        
        if cmd_type == 1:
            # EMERGENCY
            commands.append(generate_emergency(patient_counter, current_time, allow_invalid))
            patient_counter += 1
            
        elif cmd_type == 2:
            # APPOINTMENT
            commands.append(generate_appointment(patient_counter, current_time, allow_invalid))
            patient_counter += 1
            
        elif cmd_type == 3:
            # SURGERY
            commands.append(generate_surgery(patient_counter, current_time, allow_invalid))
            patient_counter += 1
            
        elif cmd_type == 4:
            # PHARMACY_REQUEST
            commands.append(generate_pharmacy_request(request_counter, current_time, allow_invalid))
            request_counter += 1
            
        elif cmd_type == 5:
            # LAB_REQUEST
            commands.append(generate_lab_request(lab_counter, current_time, allow_invalid))
            lab_counter += 1
            
        elif cmd_type == 6:
            # RESTOCK
            commands.append(generate_restock(current_time, allow_invalid))
            
        elif cmd_type == 7:
            # STATUS
            commands.append(generate_status(allow_invalid))
            
        elif cmd_type == 8:
            # Unknown command (for testing error handling)
            if random_bool(0.3):  # 30% chance
                commands.append(generate_unknown_command())
            else:
                # Default to EMERGENCY
                commands.append(generate_emergency(patient_counter, current_time, allow_invalid))
                patient_counter += 1
                
        else:
            # Random mix
            choice = random.choice([
                lambda: generate_emergency(patient_counter, current_time, allow_invalid),
                lambda: generate_appointment(patient_counter, current_time, allow_invalid),
                lambda: generate_surgery(patient_counter, current_time, allow_invalid),
            ])
            commands.append(choice())
            patient_counter += 1
    
    # Write to file
    with open(filename, "w") as f:
        for cmd in commands:
            f.write(cmd + "\n")
    
    print(f"FILE GENERATED: {filename}")
    print(f"Total commands: {len(commands)}")
    print(f"Invalid probability: {INVALID_PROBABILITY * 100}%")
    
    # Print filename for bash script capture
    print(filename)


if __name__ == "__main__":
    main()