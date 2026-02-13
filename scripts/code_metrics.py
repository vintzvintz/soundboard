#!/usr/bin/env python3
"""
Code Metrics Report Generator for Soundboard Project
Generates SLOC and cyclomatic complexity metrics using cloc and lizard

Usage: python scripts/code_metrics.py [output_file]
"""

import subprocess
import sys
import os
import re
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Tuple

# ANSI color codes
class Colors:
    BLUE = '\033[0;34m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    BOLD = '\033[1m'
    NC = '\033[0m'

    @classmethod
    def disable(cls):
        cls.BLUE = cls.GREEN = cls.YELLOW = cls.BOLD = cls.NC = ''


def check_tools():
    """Verify required tools are installed."""
    for tool in ['cloc', 'lizard']:
        if subprocess.run(['which', tool], capture_output=True).returncode != 0:
            print(f"Error: {tool} is not installed", file=sys.stderr)
            if tool == 'cloc':
                print("Install with: sudo apt install cloc", file=sys.stderr)
            else:
                print("Install with: pip install lizard", file=sys.stderr)
            sys.exit(1)


def header(text: str):
    """Print section header."""
    print(f"\n{Colors.BOLD}{Colors.BLUE}{text}{Colors.NC}")
    print("=" * len(text))


def success(text: str):
    """Print success message."""
    print(f"{Colors.GREEN}✓ {text}{Colors.NC}")


def warning(text: str):
    """Print warning message."""
    print(f"{Colors.YELLOW}⚠ {text}{Colors.NC}")


def run_cloc(path: Path, extra_args: List[str] = None) -> str:
    """Run cloc and return output."""
    cmd = ['cloc', str(path), '--quiet']
    if extra_args:
        cmd.extend(extra_args)
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.stdout


def parse_cloc_summary(output: str) -> Dict[str, int]:
    """Parse cloc output and extract summary metrics."""
    lines = output.strip().split('\n')
    for line in lines:
        if line.startswith('SUM:'):
            parts = line.split()
            return {
                'files': int(parts[1]),
                'blank': int(parts[2]),
                'comment': int(parts[3]),
                'code': int(parts[4])
            }
    return {'files': 0, 'blank': 0, 'comment': 0, 'code': 0}


def run_lizard(path: Path) -> Tuple[str, List[Dict]]:
    """Run lizard and return output plus parsed functions."""
    cmd = ['lizard', str(path), '-l', 'cpp', '--sort', 'cyclomatic_complexity']
    result = subprocess.run(cmd, capture_output=True, text=True)

    functions = []
    for line in result.stdout.split('\n'):
        # Match function lines: "  NLOC  CCN  token  PARAM  length  location"
        match = re.match(r'\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(.+)$', line)
        if match:
            nloc, ccn, token, param, length, location = match.groups()
            functions.append({
                'nloc': int(nloc),
                'ccn': int(ccn),
                'token': int(token),
                'param': int(param),
                'length': int(length),
                'location': location
            })

    return result.stdout, functions


def parse_lizard_summary(output: str) -> Dict[str, float]:
    """Parse lizard summary line.
    Format:
    Total nloc   Avg.NLOC  AvgCCN  Avg.token   Fun Cnt  Warning cnt   Fun Rt   nloc Rt
    ------------------------------------------------------------------------------------------
         5701      27.2     6.0      166.2      184           10      0.05    0.16
    """
    lines = output.split('\n')
    for i, line in enumerate(lines):
        if '----' in line and i > 0:
            # Check if previous line had "Total nloc" header
            if 'Total nloc' in lines[i-1]:
                # Data line is right after the dashes
                if i+1 < len(lines):
                    parts = lines[i+1].split()
                    if len(parts) >= 5:
                        try:
                            return {
                                'avg_nloc': float(parts[1]),
                                'avg_ccn': float(parts[2]),
                                'avg_token': float(parts[3]),
                                'fun_cnt': int(parts[4])
                            }
                        except (ValueError, IndexError):
                            pass
    return {'avg_nloc': 0, 'avg_ccn': 0, 'avg_token': 0, 'fun_cnt': 0}


def save_snapshot(metrics_dir: Path, metrics: Dict):
    """Save current metrics to snapshot file."""
    metrics_dir.mkdir(exist_ok=True)

    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    snapshot_file = metrics_dir / f"snapshot_{timestamp}.txt"

    with open(snapshot_file, 'w') as f:
        f.write(f"timestamp={timestamp}\n")
        for key, value in metrics.items():
            f.write(f"{key}={value}\n")

    return snapshot_file


def load_snapshots(metrics_dir: Path, limit: int = 5) -> List[Dict]:
    """Load recent snapshot files."""
    if not metrics_dir.exists():
        return []

    snapshots = sorted(metrics_dir.glob("snapshot_*.txt"), reverse=True)[:limit]
    results = []

    for snapshot in snapshots:
        data = {}
        with open(snapshot) as f:
            for line in f:
                if '=' in line:
                    key, value = line.strip().split('=', 1)
                    # Try to convert to numeric types
                    if value.replace('.', '').replace('-', '').isdigit():
                        try:
                            data[key] = int(value) if '.' not in value else float(value)
                        except ValueError:
                            data[key] = value
                    else:
                        data[key] = value
        results.append(data)

    return results


def main():
    # Setup
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    main_dir = project_root / "main"
    metrics_dir = project_root / ".metrics"

    # Check if we should use colors
    use_colors = sys.stdout.isatty() and len(sys.argv) == 1
    if not use_colors:
        Colors.disable()

    # Redirect output if file specified
    if len(sys.argv) > 1:
        sys.stdout = open(sys.argv[1], 'w')

    # Check tools
    check_tools()

    # Generate report
    header("Soundboard Project - Code Metrics Report")
    print(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Project: {project_root}")

    # 1. SLOC Summary
    header("Lines of Code Summary (by Language)")
    cloc_output = run_cloc(main_dir)
    print(cloc_output)
    cloc_metrics = parse_cloc_summary(cloc_output)

    # 2. Top 15 Largest Files
    header("Top 15 Largest Files")
    files_output = run_cloc(main_dir, ['--by-file'])
    file_lines = [l for l in files_output.split('\n') if '.c' in l or '.cpp' in l or '.h' in l]
    for line in file_lines[:15]:
        print(line)

    # 3. SLOC by Module
    header("SLOC by Module")
    for module in ['core', 'player', 'usb']:
        module_dir = main_dir / module
        if module_dir.exists():
            print(f"\n{module.capitalize()} Module:")
            print(run_cloc(module_dir))

    # 4. Top 10 Most Complex Functions
    header("Top 10 Most Complex Functions")
    print()
    print(f"{'NLOC':<6}  {'CCN':<4}  Function@Location")
    print(f"{'-'*6}  {'-'*4}  {'-'*60}")

    lizard_output, functions = run_lizard(main_dir)
    for func in functions[:10]:
        print(f"{func['nloc']:<6}  {func['ccn']:<4}  {func['location']}")

    # 5. High Complexity Functions
    header("High Complexity Functions (CCN > 15)")
    print()

    high_complexity = [f for f in functions if f['ccn'] > 15]

    if high_complexity:
        print(f"{'NLOC':<6}  {'CCN':<4}  Function")
        print(f"{'-'*6}  {'-'*4}  {'-'*60}")
        for func in high_complexity:
            # Extract just the function name and file
            loc = func['location'].split('@')[-1]  # Get file path part
            print(f"{func['nloc']:<6}  {func['ccn']:<4}  {loc}")
        print(f"\nTotal: {len(high_complexity)} functions")
    else:
        print("None - all functions have CCN ≤ 15")

    # 6. Average Complexity by Module
    header("Average Complexity by Module")
    print()
    print(f"{'Module':<20}  {'Avg CCN':>8}  {'Avg NLOC':>8}  {'Functions':>10}")
    print(f"{'-'*20}  {'-'*8}  {'-'*8}  {'-'*10}")

    for module in ['core', 'player', 'usb', 'main']:
        if module == 'main':
            files = list(main_dir.glob('*.c'))
        else:
            module_dir = main_dir / module
            if not module_dir.exists():
                continue
            files = list(module_dir.glob('*.c')) + list(module_dir.glob('*.cpp'))

        if files:
            # Run lizard on these specific files
            cmd = ['lizard'] + [str(f) for f in files] + ['-l', 'cpp']
            result = subprocess.run(cmd, capture_output=True, text=True)
            summary = parse_lizard_summary(result.stdout)

            if summary['fun_cnt'] > 0:
                print(f"{module:<20}  {summary['avg_ccn']:>8.1f}  "
                      f"{summary['avg_nloc']:>8.1f}  {summary['fun_cnt']:>10}")

    # 7. Code Quality Metrics
    header("Code Quality Metrics")
    print()

    lizard_summary = parse_lizard_summary(lizard_output)

    total_code = cloc_metrics['code']
    total_comment = cloc_metrics['comment']
    total_blank = cloc_metrics['blank']
    avg_ccn = lizard_summary['avg_ccn']
    total_functions = lizard_summary['fun_cnt']
    high_count = len(high_complexity)

    print(f"Total Code Lines:         {total_code}")
    print(f"Total Comment Lines:      {total_comment}")
    print(f"Total Blank Lines:        {total_blank}")
    print(f"Total Functions:          {total_functions}")
    print(f"Average CCN:              {avg_ccn:.1f}")
    print(f"High Complexity (>15):    {high_count}")
    print()

    # Quality assessment
    if total_code > 0:
        comment_pct = (total_comment * 100.0) / total_code
        print(f"Comment ratio:            {comment_pct:.1f}%")

        if comment_pct < 20:
            warning("Comment ratio below 20%")
        else:
            success("Good comment ratio (≥20%)")

    if avg_ccn > 10:
        warning("Average complexity above 10")
    else:
        success("Average complexity acceptable (≤10)")

    if high_count > 10:
        warning(f"{high_count} functions with high complexity - consider refactoring")
    elif high_count > 0:
        print(f"Note: {high_count} functions with CCN > 15 (monitor these)")
    else:
        success("No functions with CCN > 15")

    # 8. Save and show metrics history
    header("Metrics History")

    snapshot_metrics = {
        'total_code': total_code,
        'total_comment': total_comment,
        'total_functions': total_functions,
        'avg_ccn': avg_ccn,
        'high_count': high_count
    }

    snapshot_file = save_snapshot(metrics_dir, snapshot_metrics)
    print(f"\nSnapshot saved: .metrics/{snapshot_file.name}")

    # Show trend
    snapshots = load_snapshots(metrics_dir)
    if len(snapshots) > 1:
        print("\nRecent History:")
        print()
        print(f"{'Date':<18}  {'Code':>6}  {'Funcs':>6}  {'Avg CCN':>8}  {'High CCN':>8}")
        print(f"{'-'*18}  {'-'*6}  {'-'*6}  {'-'*8}  {'-'*8}")

        for snap in snapshots:
            ts = str(snap.get('timestamp', ''))
            # Format: YYYYMMDD_HHMMSS -> YYYYMMDD HH:MM
            if len(ts) >= 13:
                date_str = f"{ts[:8]} {ts[9:11]}:{ts[11:13]}"
            else:
                date_str = ts

            # Safely convert to numeric types (skip invalid snapshots)
            try:
                code = int(snap.get('total_code', 0))
                funcs = int(snap.get('total_functions', 0))
                ccn = float(snap.get('avg_ccn', 0))
                high = int(snap.get('high_count', 0))
                print(f"{date_str:<18}  {code:>6}  {funcs:>6}  {ccn:>8.1f}  {high:>8}")
            except (ValueError, TypeError):
                # Skip corrupted snapshots
                continue

    header("Report Complete")
    if len(sys.argv) > 1:
        print(f"Saved to: {sys.argv[1]}")


if __name__ == '__main__':
    main()
