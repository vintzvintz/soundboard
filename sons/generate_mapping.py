#!/usr/bin/env python3
"""
Generate mappings_generated.csv from existing mappings.csv and new .wav files.

This script:
1. Parses existing soundboard/mappings.csv preserving comments and empty lines
2. Validates each mapping line (format: page_id,button,event,action,file)
3. Checks for unreferenced sound files (missing WAV files)
4. Lists all WAV files in soundboard/
5. Warns about non-ASCII characters and long page names
6. Creates new mapping entries for unmapped WAV files
7. Outputs result as mappings_generated.csv

Supported actions: play, play_cut, play_lock (each takes a single file parameter)
"""

from dataclasses import dataclass
from datetime import datetime
from typing import Optional
from pathlib import Path

# Valid actions and their expected parameter counts
# Format: action_name -> (min_params, max_params, param_description)
VALID_ACTIONS = {
    "stop": (0, 0, "no parameters"),
    "play": (1, 1, "file"),
    "play_cut": (1, 1, "file"),
    "play_lock": (1, 1, "file"),
}

VALID_EVENTS = {"press", "long_press", "release"}
VALID_BUTTONS = set(range(1, 13))  # 1-12
MAX_PAGE_NAME_LENGTH = 31


@dataclass
class MappingLine:
    """Represents a line from the mappings file."""
    line_number: int
    raw_text: str
    line_type: str  # 'empty', 'comment', 'mapping', 'header'

    # For mapping lines only
    page_id: Optional[str] = None
    button: Optional[int] = None
    event: Optional[str] = None
    action: Optional[str] = None
    params: Optional[list] = None

    # Validation
    errors: Optional[list] = None
    warnings: Optional[list] = None

    def __post_init__(self):
        if self.errors is None:
            self.errors = []
        if self.warnings is None:
            self.warnings = []


def is_ascii_alphanumeric_safe(text: str) -> bool:
    """Check if text contains only 7-bit ASCII alphanumeric and common safe chars."""
    safe_chars = set(' _-.()')
    for char in text:
        if not (char.isascii() and (char.isalnum() or char in safe_chars)):
            return False
    return True


def get_unsafe_chars(text: str) -> list:
    """Return list of characters outside safe ASCII range."""
    safe_chars = set(' _-.()')
    unsafe = []
    for char in text:
        if not (char.isascii() and (char.isalnum() or char in safe_chars)):
            unsafe.append(char)
    return list(set(unsafe))


def parse_mapping_line(line: str, line_number: int) -> MappingLine:
    """Parse a single line from mappings.csv (v2.1 format only)."""
    stripped = line.strip()

    # Empty line
    if not stripped:
        return MappingLine(line_number=line_number, raw_text=line, line_type='empty')

    # Comment line
    if stripped.startswith('#'):
        return MappingLine(line_number=line_number, raw_text=line, line_type='comment')

    # Mapping line - parse CSV
    # Handle quoted fields properly
    fields = parse_csv_line(stripped)

    mapping = MappingLine(
        line_number=line_number,
        raw_text=line,
        line_type='mapping',
        params=[]
    )

    # New format: page_id,button,event,action[,param1,param2,...]
    min_fields = 4
    if len(fields) < min_fields:
        mapping.errors.append(f"Too few fields: expected at least {min_fields}, got {len(fields)}")
        return mapping

    mapping.page_id = fields[0].strip()
    try:
        mapping.button = int(fields[1].strip())
    except ValueError:
        mapping.errors.append(f"Invalid button number: '{fields[1]}'")
        return mapping

    mapping.event = fields[2].strip().lower()
    mapping.action = fields[3].strip().lower()

    if len(fields) > 4:
        mapping.params = [f.strip() for f in fields[4:]]

    # Validate parsed mapping
    validate_mapping(mapping)

    return mapping


def parse_csv_line(line: str) -> list:
    """Parse a CSV line handling quoted fields."""
    fields = []
    current = []
    in_quotes = False

    for char in line:
        if char == '"':
            in_quotes = not in_quotes
        elif char == ',' and not in_quotes:
            fields.append(''.join(current))
            current = []
        else:
            current.append(char)

    fields.append(''.join(current))
    return fields


def validate_mapping(mapping: MappingLine):
    """Validate a mapping line and populate errors/warnings."""

    # Check page name length
    if mapping.page_id and len(mapping.page_id) > MAX_PAGE_NAME_LENGTH:
        mapping.warnings.append(f"Page name '{mapping.page_id}' exceeds {MAX_PAGE_NAME_LENGTH} characters")

    # Check page name for unsafe characters
    if mapping.page_id:
        unsafe = get_unsafe_chars(mapping.page_id)
        if unsafe:
            mapping.warnings.append(f"Page name contains unsupported characters for LCD: {unsafe}")

    # Validate button number
    if mapping.button is not None and mapping.button not in VALID_BUTTONS:
        mapping.errors.append(f"Invalid button number: {mapping.button} (must be 1-12)")

    # Validate event (for new format)
    if mapping.event and mapping.event not in VALID_EVENTS:
        mapping.errors.append(f"Invalid event: '{mapping.event}' (must be one of: {', '.join(VALID_EVENTS)})")

    # Validate action
    if mapping.action:
        if mapping.action not in VALID_ACTIONS:
            mapping.errors.append(f"Invalid action: '{mapping.action}' (must be one of: {', '.join(VALID_ACTIONS.keys())})")
        else:
            min_params, max_params, desc = VALID_ACTIONS[mapping.action]
            param_count = len(mapping.params) if mapping.params else 0

            if param_count < min_params:
                mapping.errors.append(f"Action '{mapping.action}' requires at least {min_params} parameter(s) ({desc}), got {param_count}")
            elif param_count > max_params:
                mapping.errors.append(f"Action '{mapping.action}' accepts at most {max_params} parameter(s) ({desc}), got {param_count}")



def parse_mappings_file(filepath: Path) -> list[MappingLine]:
    """Parse the entire mappings file"""
    if not filepath.exists():
        return []

    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    parsed_lines = []

    for i, line in enumerate(lines, start=1):
        parsed = parse_mapping_line(line.rstrip('\n\r'), i)
        parsed_lines.append(parsed)

    return parsed_lines


def get_referenced_files(mappings: list[MappingLine]) -> set:
    """Extract all referenced WAV files from mappings."""
    files = set()
    for m in mappings:
        if m.line_type == 'mapping' and m.params:
            for param in m.params:
                if param.endswith('.wav'):
                    files.add(param)
    return files


def get_wav_files(directory: Path) -> set:
    """Get all .wav files in directory."""
    if not directory.exists():
        return set()
    return {f.name for f in directory.glob('*.wav')}


def generate_mappings():
    """Main function to generate mappings."""
    script_dir = Path(__file__).parent
    soundboard_dir = script_dir / 'soundboard'
    input_file = soundboard_dir / 'mappings.csv'
    output_file = soundboard_dir / 'mappings_generated.csv'

    print(f"Soundboard directory: {soundboard_dir}")
    print(f"Input file: {input_file}")
    print(f"Output file: {output_file}")
    print()

    # Parse existing mappings (v2.1 format only)
    parsed_lines = parse_mappings_file(input_file)
    print("Format: page_id,button,event,action,file")
    print()

    # Get all WAV files
    wav_files = get_wav_files(soundboard_dir)
    print(f"Found {len(wav_files)} WAV files in soundboard/")

    # Get referenced files
    referenced_files = get_referenced_files(parsed_lines)
    print(f"Found {len(referenced_files)} file references in mappings.csv")
    print()

    # Check for errors and warnings
    errors_found = False
    warnings_found = False

    for m in parsed_lines:
        if m.errors:
            errors_found = True
            print(f"ERROR line {m.line_number}: {m.raw_text}")
            for err in m.errors:
                print(f"  -> {err}")
        if m.warnings:
            warnings_found = True
            print(f"WARNING line {m.line_number}: {m.raw_text}")
            for warn in m.warnings:
                print(f"  -> {warn}")

    if errors_found or warnings_found:
        print()

    # Check for unreferenced files (files in CSV but not on disk)
    missing_files = referenced_files - wav_files
    if missing_files:
        print("WARNING: Referenced files not found in soundboard/:")
        for f in sorted(missing_files):
            print(f"  -> {f}")
        print()

    # Find unmapped WAV files
    unmapped_files = wav_files - referenced_files
    if unmapped_files:
        print(f"Found {len(unmapped_files)} unmapped WAV files (will be added with page='new'):")
        for f in sorted(unmapped_files):
            print(f"  -> {f}")
        print()

    # Generate output
    output_lines = []

    # Add header comment with timestamp
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    output_lines.append("# Soundboard Mappings")
    output_lines.append("# Format: page_id,button,event,action,file")
    output_lines.append(f"# Generated by generate_mapping.py on {timestamp}")
    output_lines.append("")

    # Track files already warned about to avoid duplicates
    warned_files = set()

    # Track assigned page/button/event combinations to detect duplicates
    assigned_slots = {}  # key: (page_id, button, event) -> list of line numbers

    # First pass: collect all assignments to detect duplicates
    for m in parsed_lines:
        if m.line_type == 'mapping' and not m.errors:
            key = (m.page_id, m.button, m.event)
            if key not in assigned_slots:
                assigned_slots[key] = []
            assigned_slots[key].append(m.line_number)

    # Find duplicates
    duplicate_slots = {k: v for k, v in assigned_slots.items() if len(v) > 1}
    if duplicate_slots:
        print("WARNING: Duplicate page/button/event assignments found:")
        for (page, button, event), lines in sorted(duplicate_slots.items()):
            print(f"  -> {page}/{button}/{event} assigned on lines: {lines}")
        print()

    # Track pages and their assigned buttons
    page_buttons = {}  # page_id -> set of assigned buttons

    # Process existing lines
    for m in parsed_lines:
        if m.line_type == 'empty':
            output_lines.append("")
        elif m.line_type == 'comment':
            output_lines.append(m.raw_text)
        elif m.line_type == 'mapping':
            if m.errors:
                # Comment out invalid lines for user to fix later
                output_lines.append(f"# INVALID: {m.raw_text}")
                for err in m.errors:
                    output_lines.append(f"#   -> {err}")
            else:
                # Track page buttons
                if m.page_id not in page_buttons:
                    page_buttons[m.page_id] = set()
                page_buttons[m.page_id].add(m.button)

                # Add warnings before the mapping line
                if m.params:
                    for param in m.params:
                        if param.endswith('.wav'):
                            # Warning for missing file
                            if param in missing_files:
                                output_lines.append(f"# WARNING: file not found: {param}")
                            # Warning for non-ASCII characters (only once per file)
                            if param not in warned_files:
                                unsafe = get_unsafe_chars(param)
                                if unsafe:
                                    output_lines.append(f"# WARNING: unsupported LCD characters: {unsafe}")
                                    print(f"WARNING: filename contains unsupported LCD characters: '{param}' -> {unsafe}")
                                    warned_files.add(param)

                # Warning for duplicate assignment
                key = (m.page_id, m.button, m.event)
                if key in duplicate_slots:
                    output_lines.append(f"# WARNING: duplicate assignment for {m.page_id}/{m.button}/{m.event}")

                # Output valid mapping line
                params_str = ','.join(m.params) if m.params else ''
                if params_str:
                    line = f"{m.page_id},{m.button},{m.event},{m.action},{params_str}"
                else:
                    line = f"{m.page_id},{m.button},{m.event},{m.action}"
                output_lines.append(line)

    # Add new mappings for unmapped files
    if unmapped_files:
        output_lines.append("")
        output_lines.append("# ============================================================================")
        output_lines.append("# New unmapped files (page='new', adjust page_id and button manually)")
        output_lines.append("# ============================================================================")
        output_lines.append("")

        button_num = 1
        for wav_file in sorted(unmapped_files):
            # Warning for non-ASCII characters (only once per file)
            if wav_file not in warned_files:
                unsafe = get_unsafe_chars(wav_file)
                if unsafe:
                    output_lines.append(f"# WARNING: unsupported LCD characters: {unsafe}")
                    print(f"WARNING: filename contains unsupported LCD characters: '{wav_file}' -> {unsafe}")
                    warned_files.add(wav_file)

            output_lines.append(f"new,{button_num},press,play,{wav_file}")
            button_num += 1
            if button_num > 12:
                button_num = 1  # Wrap around, user will need to adjust pages

    # Add section for unassigned buttons on each page
    all_buttons = set(range(1, 13))
    pages_with_unassigned = {}
    for page_id, assigned in page_buttons.items():
        unassigned = all_buttons - assigned
        if unassigned:
            pages_with_unassigned[page_id] = sorted(unassigned)

    if pages_with_unassigned:
        output_lines.append("")
        output_lines.append("# ============================================================================")
        output_lines.append("# Unassigned buttons per page")
        output_lines.append("# ============================================================================")
        for page_id in sorted(pages_with_unassigned.keys()):
            buttons = pages_with_unassigned[page_id]
            output_lines.append(f"# {page_id}: buttons {', '.join(map(str, buttons))}")

    # Write output
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write('\n'.join(output_lines))
        f.write('\n')

    print(f"Generated: {output_file}")
    print(f"Total lines: {len(output_lines)}")

    # Summary
    mapping_count = sum(1 for m in parsed_lines if m.line_type == 'mapping' and not m.errors)
    new_count = len(unmapped_files)
    print(f"Existing mappings: {mapping_count}")
    print(f"New mappings added: {new_count}")

    if missing_files:
        print(f"\nACTION REQUIRED: {len(missing_files)} referenced file(s) not found!")
    if unmapped_files:
        print(f"\nACTION REQUIRED: Review new mappings and assign correct page_id and button numbers")


if __name__ == "__main__":
    generate_mappings()
