#!/usr/bin/env python3
"""
Generate printable and browsable HTML sheets from soundboard/mappings.csv.

Outputs three files:
- mapping_sheet_print.html:  A4 landscape, optimized for print/PDF
- mapping_sheet.html:        Desktop browser, 3-column layout
- mapping_sheet_mobile.html: Mobile browser, single-column layout
"""

from pathlib import Path
import html


# Button layout: row 3 (top) = buttons 10,11,12 ... row 0 (bottom) = buttons 1,2,3
ROWS = [
    (10, 11, 12),  # row 3 (top)
    (7, 8, 9),     # row 2
    (4, 5, 6),     # row 1
    (1, 2, 3),     # row 0 (bottom)
]


def parse_mappings(filepath):
    """Parse mappings.csv, return dict: page_id -> {button: filename}."""
    pages = {}
    page_order = []

    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue

            fields = line.split(',')
            if len(fields) < 4:
                continue

            page_id = fields[0].strip()
            try:
                button = int(fields[1].strip())
            except ValueError:
                continue

            filename = fields[4].strip() if len(fields) > 4 else None

            if page_id not in pages:
                pages[page_id] = {}
                page_order.append(page_id)

            if filename and button not in pages[page_id]:
                name = filename
                if name.lower().endswith('.wav'):
                    name = name[:-4]
                pages[page_id][button] = name

    return pages, page_order


def build_page_block(page_id, mapping):
    """Build HTML for a single page table block."""
    t = f'<div class="page-block">\n'
    t += f'  <div class="page-title">{html.escape(page_id)}</div>\n'
    t += '  <table>\n'
    for row in ROWS:
        t += '    <tr>\n'
        for btn in row:
            label = html.escape(mapping.get(btn, ''))
            cell_class = ' class="empty"' if not label else ''
            t += f'      <td{cell_class}>{label}</td>\n'
        t += '    </tr>\n'
    t += '  </table>\n'
    t += '</div>'
    return t


def build_grid(tables_html, columns, reverse):
    """Arrange table blocks into rows. Optionally reverse row order."""
    items = list(tables_html)

    # Pad to multiple of columns
    while len(items) % columns != 0:
        items.append('<div class="page-block placeholder"></div>')

    # Group into rows
    rows = []
    for i in range(0, len(items), columns):
        rows.append(items[i:i + columns])

    if reverse:
        rows.reverse()

    out = ''
    for row in rows:
        out += '<div class="table-row">\n'
        for cell in row:
            out += cell + '\n'
        out += '</div>\n'
    return out


def generate_print_html(pages, page_order):
    """A4 landscape, fixed widths, optimized for print/PDF."""
    tables = [build_page_block(pid, pages[pid]) for pid in page_order]
    grid = build_grid(tables, columns=3, reverse=True)

    return f"""<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<title>Soundboard Mapping Sheet (Print)</title>
<style>
@page {{
    size: A4 landscape;
    margin: 10mm;
}}

* {{ margin: 0; padding: 0; box-sizing: border-box; }}

body {{
    font-family: "Segoe UI", Arial, Helvetica, sans-serif;
    font-size: 11px;
    color: #222;
    background: #fff;
}}

.table-row {{
    display: flex;
    justify-content: center;
    gap: 16px;
    margin-bottom: 16px;
}}

.page-block {{
    flex: 0 0 auto;
    width: 250px;
}}

.page-block.placeholder {{ visibility: hidden; }}

.page-title {{
    text-align: center;
    font-weight: bold;
    font-size: 14px;
    margin-bottom: 4px;
    padding: 3px 0;
    background: #333;
    color: #fff;
    border-radius: 4px 4px 0 0;
}}

table {{
    width: 100%;
    border-collapse: collapse;
    table-layout: fixed;
}}

td {{
    border: 1px solid #888;
    text-align: center;
    vertical-align: middle;
    padding: 6px 3px;
    height: 36px;
    font-size: 10px;
    word-break: break-word;
    overflow: hidden;
}}

td.empty {{ background: #f0f0f0; }}

@media print {{
    body {{
        -webkit-print-color-adjust: exact;
        print-color-adjust: exact;
    }}
}}
</style>
</head>
<body>
{grid}
</body>
</html>
"""


def generate_desktop_html(pages, page_order):
    """Desktop browser, 3-column reversed layout, screen-friendly sizing."""
    tables = [build_page_block(pid, pages[pid]) for pid in page_order]
    grid = build_grid(tables, columns=3, reverse=True)

    return f"""<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<title>Soundboard Mapping Sheet</title>
<style>
* {{ margin: 0; padding: 0; box-sizing: border-box; }}

body {{
    font-family: "Segoe UI", Arial, Helvetica, sans-serif;
    font-size: 14px;
    color: #222;
    background: #f5f5f5;
    padding: 24px;
}}

.table-row {{
    display: flex;
    justify-content: center;
    gap: 24px;
    margin-bottom: 24px;
}}

.page-block {{
    flex: 0 0 auto;
    width: 300px;
    background: #fff;
    border-radius: 6px;
    box-shadow: 0 1px 4px rgba(0,0,0,0.1);
    overflow: hidden;
}}

.page-block.placeholder {{ visibility: hidden; }}

.page-title {{
    text-align: center;
    font-weight: bold;
    font-size: 16px;
    padding: 6px 0;
    background: #333;
    color: #fff;
}}

table {{
    width: 100%;
    border-collapse: collapse;
    table-layout: fixed;
}}

td {{
    border: 1px solid #ccc;
    text-align: center;
    vertical-align: middle;
    padding: 10px 5px;
    height: 48px;
    font-size: 13px;
    word-break: break-word;
    overflow: hidden;
}}

td.empty {{ background: #f8f8f8; color: #bbb; }}
</style>
</head>
<body>
{grid}
</body>
</html>
"""


def generate_mobile_html(pages, page_order):
    """Mobile browser, single column, natural top-down order."""
    tables = [build_page_block(pid, pages[pid]) for pid in page_order]
    grid = build_grid(tables, columns=1, reverse=False)

    return f"""<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Soundboard Mapping Sheet</title>
<style>
* {{ margin: 0; padding: 0; box-sizing: border-box; }}

body {{
    font-family: -apple-system, "Segoe UI", Arial, Helvetica, sans-serif;
    font-size: 16px;
    color: #222;
    background: #f5f5f5;
    padding: 12px;
}}

.table-row {{
    display: flex;
    justify-content: center;
    margin-bottom: 16px;
}}

.page-block {{
    width: 100%;
    max-width: 400px;
    background: #fff;
    border-radius: 8px;
    box-shadow: 0 1px 4px rgba(0,0,0,0.1);
    overflow: hidden;
}}

.page-title {{
    text-align: center;
    font-weight: bold;
    font-size: 18px;
    padding: 8px 0;
    background: #333;
    color: #fff;
}}

table {{
    width: 100%;
    border-collapse: collapse;
    table-layout: fixed;
}}

td {{
    border: 1px solid #ccc;
    text-align: center;
    vertical-align: middle;
    padding: 12px 6px;
    height: 56px;
    font-size: 14px;
    word-break: break-word;
    overflow: hidden;
}}

td.empty {{ background: #f8f8f8; color: #bbb; }}
</style>
</head>
<body>
{grid}
</body>
</html>
"""


def main():
    script_dir = Path(__file__).parent
    input_file = script_dir / 'soundboard' / 'mappings.csv'
    output_dir = script_dir / 'soundboard'

    if not input_file.exists():
        print(f"Error: {input_file} not found")
        return

    pages, page_order = parse_mappings(input_file)
    print(f"Parsed {len(page_order)} pages: {', '.join(page_order)}")
    for pid in page_order:
        print(f"  {pid}: {len(pages[pid])} buttons mapped")

    outputs = [
        ('mapping_sheet_print.html', generate_print_html),
        ('mapping_sheet.html', generate_desktop_html),
        ('mapping_sheet_mobile.html', generate_mobile_html),
    ]

    for filename, generator in outputs:
        path = output_dir / filename
        with open(path, 'w', encoding='utf-8') as f:
            f.write(generator(pages, page_order))
        print(f"Generated: {path}")


if __name__ == '__main__':
    main()
