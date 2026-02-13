# Soundboard Scripts

This directory contains utility scripts for the soundboard project.

## Code Metrics Script

**File:** `code_metrics.py`

Generates comprehensive code quality reports including:
- Lines of Code (SLOC) by language and module
- Cyclomatic Complexity analysis
- High-complexity function identification
- Historical metrics tracking

### Requirements

```bash
# System package
sudo apt install cloc

# Python package
pip install lizard
```

### Usage

**Terminal output (with colors):**
```bash
python3 scripts/code_metrics.py
```

**Save to file:**
```bash
python3 scripts/code_metrics.py metrics_report.txt
```

### Metrics Tracked

- **Total Code Lines:** Source lines of code (excluding comments/blanks)
- **Comment Ratio:** Percentage of comment lines vs code lines
- **Average CCN:** Average cyclomatic complexity across all functions
- **High Complexity Count:** Number of functions with CCN > 15

### Output Sections

1. **Lines of Code Summary** - Breakdown by language (C, C++, Headers)
2. **Top 15 Largest Files** - Sorted by code lines
3. **SLOC by Module** - Separate metrics for core, player, and usb modules
4. **Top 10 Most Complex Functions** - Functions with highest cyclomatic complexity
5. **High Complexity Functions** - All functions with CCN > 15
6. **Average Complexity by Module** - Per-module complexity statistics
7. **Code Quality Metrics** - Overall quality assessment
8. **Metrics History** - Historical trend (last 5 runs)

### Quality Thresholds

- ✓ **Comment ratio ≥20%**: Good documentation
- ✓ **Average CCN ≤10**: Acceptable complexity
- ⚠ **Functions with CCN >15**: Consider refactoring

### Metrics History

Snapshots are saved in `.metrics/snapshot_YYYYMMDD_HHMMSS.txt` and tracked over time.

### Current Project Stats

- **~5,900 lines of code** (C/C++)
- **184 functions**
- **Average CCN: 6.0** (good)
- **Comment ratio: 39.6%** (excellent)
- **20 high-complexity functions** (monitor)

### High-Complexity Functions to Watch

| Function | CCN | Module |
|----------|-----|--------|
| `mapper_handle_event` | 36 | player/mapper.c |
| `display_task` | 23 | core/display.cpp |
| `cache_file_internal` | 22 | player/provider.c |
| `msc_print_status` | 22 | usb/msc.c |
| `input_scanner_init` | 21 | core/input_scanner.c |

These functions work correctly but might benefit from refactoring if they need modification.
