#!/usr/bin/env python3
"""Generate deterministic 170x320 UI references for the seven firmware pages."""

from pathlib import Path
from xml.sax.saxutils import escape

W, H = 170, 320
OUT = Path(__file__).resolve().parents[1] / "docs" / "assets" / "ui-v3"

C = {
    "bg": "#000000", "panel": "#0e1116", "panel2": "#101a20",
    "text": "#f5f7fa", "dim": "#8a93a3", "idle": "#4a5260",
    "guide": "#25313e", "cyan": "#37c4d6", "blue": "#3fa9f5",
    "lime": "#c8e63c", "green": "#36d27e", "amber": "#ffc800",
    "orange": "#ff7a1a", "red": "#ff4057",
}


def text(x, y, value, size=8, color="text", anchor="start", weight=600):
    return (f'<text x="{x}" y="{y}" fill="{C[color]}" font-family="SFMono-Regular,Menlo,monospace" '
            f'font-size="{size}" font-weight="{weight}" text-anchor="{anchor}">{escape(value)}</text>')


def panel(x, y, w, h, stroke="guide", fill="panel", radius=7):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{C[fill]}" stroke="{C[stroke]}"/>'


def curve(color="cyan", y=170, dashed=False, fill=False):
    path = f'M15 {y+20} C34 {y+15},42 {y+27},59 {y+17} S88 {y+4},104 {y+13} S132 {y+28},155 {y+7}'
    area = f'<path d="{path} L155 {y+30} L15 {y+30}Z" fill="{C[color]}" opacity=".12"/>' if fill else ""
    dash = ' stroke-dasharray="4 4"' if dashed else ""
    return area + f'<path d="{path}" fill="none" stroke="{C[color]}" stroke-width="2"{dash}/>'


def header(title, status="LIVE", accent="cyan"):
    return [text(8, 17, title, 12), '<circle cx="126" cy="13" r="2" fill="%s"/>' % C["green"],
            text(162, 16, status, 7, "green", "end"),
            f'<rect x="8" y="24" width="154" height="2" rx="1" fill="{C[accent]}" opacity=".65"/>']


def footer(index, name):
    out = [text(8, 302, name, 7)]
    for i in range(7):
        x = 101 + i * 9
        out.append(f'<rect x="{x-4}" y="296" width="8" height="4" rx="2" fill="{C["text"]}"/>' if i == index
                   else f'<circle cx="{x}" cy="298" r="1.2" fill="{C["idle"]}"/>')
    return out


def value_unit(value, y=82, color="text"):
    return [text(8, y + 34, value, 37, color), text(162, y + 34, "Mb/s", 8, "dim", "end")]


def state_band(label, color):
    return [f'<rect x="8" y="30" width="154" height="32" rx="6" fill="{C[color]}" opacity=".09" stroke="{C[color]}"/>',
            text(85, 51, label, 12, color, "middle", 700)]


def tiles(items, y=207):
    out = []
    width = 73 if len(items) == 2 else 46
    xs = [8, 89] if len(items) == 2 else [8, 62, 116]
    for x, (name, value, color) in zip(xs, items):
        out += [panel(x, y, width, 38, color, "panel2"),
                f'<rect x="{x+6}" y="{y+6}" width="2" height="9" rx="1" fill="{C[color]}"/>',
                text(x+12, y+14, name, 6, "dim"), text(x+7, y+31, value, 12, color)]
    return out


def trend(label, color="cyan", dashed=False, fill=True):
    return [panel(8, 134, 154, 63), text(15, 148, label, 7, "dim"),
            curve("idle" if dashed else color, 154, dashed, fill)]


def page_overview():
    a = header("FNOS", accent="cyan") + [text(8, 38, "DOWNLOAD", 7, "cyan")] + value_unit("126.4", 36)
    a += [text(8, 100, "UPLOAD", 7, "blue"), text(8, 125, "18.7", 23), text(162, 125, "Mb/s", 8, "dim", "end")]
    a += [panel(8, 135, 154, 67), text(15, 149, "14S LIVE", 7, "dim"), curve("cyan", 155, fill=True)]
    a += tiles([("API RT", "42ms", "orange"), ("MEM", "52%", "cyan"), ("DISK", "61%", "amber")], 213)
    a += [text(8, 270, "CPU 46%", 7, "dim"), text(74, 270, "TEMP 54°C", 7, "dim"), text(162, 270, "23d", 7, "dim", "end")]
    return a


def page_network():
    a = header("NETWORK", accent="blue") + [panel(8, 31, 154, 43)]
    for x, label, color in ((9, "NAS", "blue"), (64, "WIFI", "cyan"), (118, "LCD", "lime")):
        a += [f'<rect x="{x}" y="39" width="44" height="24" rx="6" fill="{C[color]}" opacity=".14" stroke="{C[color]}"/>', text(x+22, 54, label, 7, color, "middle")]
    a += [text(8, 91, "WAN DOWN", 7, "cyan"), text(162, 96, "126.4 Mb/s", 16, anchor="end"),
          text(8, 119, "WAN UP", 7, "blue"), text(162, 124, "18.7 Mb/s", 16, anchor="end")]
    a += trend("API RTT 14S  42ms", "orange", fill=False)
    a += tiles([("LAN", "10G LINK", "blue"), ("WIFI", "-48dBm", "cyan")])
    a += [text(8, 272, "192.168.9.23", 7, "dim"), text(162, 272, "LIVE", 7, "green", "end")]
    return a


def page_storage():
    a = header("STORAGE", accent="amber")
    a += ['<path d="M24 91 A45 45 0 1 1 104 91" fill="none" stroke="#25313e" stroke-width="9"/>',
          '<path d="M24 91 A45 45 0 0 1 91 48" fill="none" stroke="#ffc800" stroke-width="9"/>',
          text(65, 84, "61%", 25, anchor="middle"), text(65, 105, "USED", 7, "dim", "middle"),
          text(120, 54, "TOTAL", 7, "dim"), text(120, 72, "8.0T", 13), text(120, 94, "FREE", 7, "dim"), text(120, 112, "3.1T", 13)]
    a += [panel(8, 133, 154, 70), text(15, 147, "CAPACITY", 7, "dim"), text(15, 166, "POOL", 7),
          '<rect x="49" y="159" width="104" height="7" rx="3" fill="#25313e"/>', '<rect x="49" y="159" width="63" height="7" rx="3" fill="#ffc800"/>',
          text(15, 189, "USED 4.9T", 7), text(153, 189, "FREE 3.1T", 7, "dim", "end")]
    a += [text(8, 225, "DISK TEMP", 7, "orange"), text(162, 225, "43°C", 15, anchor="end"), curve("orange", 232)]
    a += [text(8, 280, "RAID HEALTHY", 7, "green"), text(162, 280, "R 4.2MB/s", 7, "blue", "end")]
    return a


def page_system():
    a = header("SYSTEM", accent="blue")
    for x, title, value, color, sub in ((8, "CPU", "46%", "blue", "54°C"), (89, "MEMORY", "52%", "cyan", "8.3G")):
        a += [panel(x, 31, 73, 74), text(x+8, 47, title, 7, color), text(x+8, 77, value, 24), text(x+8, 98, sub, 7, "dim")]
    a += trend("CPU LOAD 14S", "blue", fill=False)
    a += [text(8, 207, "GPU", 7, "dim"), text(162, 207, "18%", 15, anchor="end"), panel(8, 218, 154, 35, fill="panel2"),
          '<circle cx="18" cy="235" r="4" fill="#36d27e"/>', text(28, 239, "HEALTHY", 7), text(153, 239, "43°C", 7, anchor="end"),
          text(8, 274, "UPTIME 23d", 7, "dim"), text(162, 274, "LOAD 1.24", 7, "dim", "end")]
    return a


def page_state(kind):
    cfg = {
        "NORMAL": ("ONLINE HEALTHY", "green", "126.4", "NETWORK 14S", False),
        "HIGH LOAD": ("HIGH LOAD", "amber", "986.2", "TRAFFIC PEAK 14S", False),
        "TIMEOUT": ("CONNECTION LOST", "red", "--.-", "NO REALTIME DATA", True),
    }[kind]
    band, status_color, value, label, offline = cfg
    a = header("STATE", "OFFLINE" if offline else ("HIGH LOAD" if kind == "HIGH LOAD" else "LIVE"), status_color)
    a += state_band(band, status_color) + [text(8, 82, "DOWNLOAD", 7, "idle" if offline else "cyan")]
    a += value_unit(value, 76, "dim" if offline else "text") + trend(label, "cyan", offline, not offline)
    if offline:
        a += tiles([("LAST GOOD", "12s ago", "dim"), ("NEXT RETRY", "1s", "red")])
        a += [text(8, 272, "API UNAVAILABLE", 7, "red"), text(162, 272, "OFFLINE", 7, "red", "end")]
    else:
        a += tiles([("API RT", "42ms", "orange"), ("CPU", "91%" if kind == "HIGH LOAD" else "46%", "amber" if kind == "HIGH LOAD" else "blue"), ("DISK", "86%" if kind == "HIGH LOAD" else "61%", "amber")])
        a += [text(8, 272, "THROTTLE WATCH" if kind == "HIGH LOAD" else "SERVICES READY", 7, status_color),
              text(162, 272, "18.7 UP" if kind == "HIGH LOAD" else "API 42ms", 7, "blue" if kind == "HIGH LOAD" else "green", "end")]
    return a


PAGES = [
    ("01-overview", "OVERVIEW", page_overview), ("02-network", "NETWORK", page_network),
    ("03-storage", "STORAGE", page_storage), ("04-system", "SYSTEM", page_system),
    ("05-normal", "NORMAL", lambda: page_state("NORMAL")),
    ("06-high-load", "HIGH LOAD", lambda: page_state("HIGH LOAD")),
    ("07-timeout", "TIMEOUT", lambda: page_state("TIMEOUT")),
]


def svg(body, width=W, height=H):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}"><rect width="100%" height="100%" fill="#000"/>{body}</svg>'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rendered = []
    for index, (slug, name, fn) in enumerate(PAGES):
        body = fn() + footer(index, name) + ['<rect x=".5" y=".5" width="169" height="319" fill="none" stroke="#25313e"/>']
        content = svg("".join(body))
        path = OUT / f"{slug}-170x320.svg"
        path.write_text(content, encoding="utf-8")
        rendered.append((slug, content))
    cw, ch, gap = W * 4 + 50, H * 2 + 30, 10
    cells = []
    for i, (_, content) in enumerate(rendered):
        x, y = gap + (i % 4) * (W + gap), gap + (i // 4) * (H + gap)
        inner = content[content.index(">") + 1:content.rindex("</svg>")]
        inner = inner.replace('<rect width="100%" height="100%" fill="#000"/>',
                              '<rect width="170" height="320" fill="#000"/>')
        cells.append(f'<g transform="translate({x} {y})">{inner}</g>')
    (OUT / "fnos-ui-v3-contact-sheet.svg").write_text(svg("".join(cells), cw, ch), encoding="utf-8")


if __name__ == "__main__":
    main()
