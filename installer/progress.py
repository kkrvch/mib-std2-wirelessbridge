"""Append-only operation log with a short live footer; plain lines when redirected."""

import math
import sys
import threading
import time
from contextlib import contextmanager
from contextvars import ContextVar

from rich.console import Console as RichConsole
from rich.console import Group
from rich.live import Live
from rich.text import Text

# Semantic ANSI names, so the terminal's own theme decides the actual colors.
ACCENT = "cyan"
OK = "green"
WARN = "yellow"
ERROR = "red"
DIM = "dim"
MAX_WIDTH = 80
BAR = 24
SPINNER = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
PROBLEMS_SHOWN = 8

_current = ContextVar("setup_progress", default=None)


def size(count):
    if count >= 1_000_000:
        return f"{count / 1_000_000:.1f} MB"
    if count >= 1000:
        return f"{count / 1000:.0f} KB"
    return f"{count:.0f} B"


def duration(seconds):
    if seconds < 60:
        return f"{seconds:.1f}s"
    return f"{int(seconds // 60)}m {int(seconds % 60):02d}s"


def clock(seconds):
    return f"{int(seconds // 60)}:{int(seconds % 60):02d}"


def terminal(file=None, stderr=False):
    return RichConsole(file=file, stderr=stderr, highlight=False)


def width(console):
    return min(console.width, MAX_WIDTH)


def fit(text, cells, style=""):
    """Exactly `cells` wide: truncated with an ellipsis, never wrapped."""
    line = Text(text, style=style, no_wrap=True)
    line.truncate(max(0, cells), overflow="ellipsis", pad=True)
    return line


def columns(total):
    label = 26 if total >= 70 else max(12, (total - 11) // 2)
    return label, max(0, total - 3 - label - 8)


def row(
    total,
    glyph,
    glyph_style,
    label,
    detail="",
    seconds=None,
    label_style="",
    *,
    timer=duration,
):
    label_cells, detail_cells = columns(total)
    line = Text(" ", no_wrap=True)
    line.append(glyph, glyph_style)
    line.append(" ")
    line.append_text(fit(label, label_cells, label_style))
    line.append_text(fit(detail, detail_cells, DIM))
    line.append(f"{timer(seconds) if seconds is not None else '':>8}", DIM)
    return line


def indented(text, total, style=DIM, indent=3):
    return Text(" " * indent) + fit(text, total - indent, style)


class Console:
    def __init__(self):
        self.output = sys.stdout
        self.console = terminal(self.output)
        self.live = self.console.is_terminal and not self.console.is_dumb_terminal
        self.lock = threading.RLock()
        self.display = None
        self.label = None
        self.stage = None
        self.started = 0
        self.status = ""
        self.note = None
        self.bytes = None
        self.transfer_started = self.transfer_reported = 0
        self.item_label = None
        self.item_index = self.item_total = self.items_done = 0
        self.problems = []

    def render(self):
        total = width(self.console)
        if not self.label:
            return Text("")
        now = time.monotonic()
        lines = [
            row(
                total,
                SPINNER[int(now * 10) % len(SPINNER)],
                ACCENT,
                self.label,
                f"{self.stage[0]}/{self.stage[1]}" if self.stage else "",
                now - self.started,
                "bold",
                timer=clock,
            )
        ]
        inner = total - 3
        if self.item_total:
            counter = f" {self.item_index}/{self.item_total}"
            lines.append(
                Text("   ")
                + fit(self.item_label or "", inner - len(counter))
                + Text(counter, style=DIM)
            )
        if self.status and not (self.item_total and self.bytes):
            lines.append(indented(self.status, total))
        if self.bytes:
            lines.append(self.meter(inner))
        remaining = self.stage[1] - self.stage[0] if self.stage else 0
        hint = (f"{remaining} more · " if remaining else "") + "ctrl-c to stop"
        lines += [Text(""), indented(hint, total, indent=1)]
        return Group(*lines)

    def meter(self, cells):
        current, expected = self.bytes
        filled = round(BAR * current / expected) if expected else BAR
        line = Text("   ", no_wrap=True)
        line.append("━" * filled, ACCENT)
        line.append("─" * (BAR - filled), DIM)
        info = f"  {size(current)} / {size(expected)}"
        spent = time.monotonic() - self.transfer_started
        if spent >= 1 and 0 < current < expected:
            rate = current / spent
            left = math.ceil((expected - current) / rate)
            info += f"  {size(rate)}/s  {f'{left}s' if left < 60 else clock(left)} left"
        return line + fit(info, cells - BAR, DIM)

    def print(self, *lines):
        with self.lock:
            for line in lines:
                self.console.print(line, no_wrap=True, overflow="ellipsis", crop=True)

    def start_display(self):
        if self.live and self.display is None:
            self.display = Live(
                console=self.console,
                get_renderable=self.render,
                refresh_per_second=10,
                transient=True,
            )
            self.display.start()

    def commit(self, glyph, style, detail):
        total = width(self.console)
        self.print(
            row(
                total, glyph, style, self.label, detail, time.monotonic() - self.started
            )
        )
        for label, status in self.problems[:PROBLEMS_SHOWN]:
            line = Text("   ", no_wrap=True)
            line.append("! ", WARN)
            label_cells = max(0, (total - 5) // 2)
            line.append_text(fit(label, label_cells))
            line.append_text(fit(status, total - 5 - label_cells, DIM))
            self.print(line)
        if len(self.problems) > PROBLEMS_SHOWN:
            hidden = len(self.problems) - PROBLEMS_SHOWN
            self.print(indented(f"+ {hidden} more, see manifest.json", total, indent=5))

    def done(self):
        with self.lock:
            if not self.label:
                return
            detail = self.note
            if detail is None:
                detail = (
                    f"{self.items_done}/{self.item_total}" if self.item_total else ""
                )
            self.commit("✓", OK, detail)
            self.label = None

    def step(self, label, *, index=None, total=None):
        with self.lock:
            self.done()
            self.stage = (
                (index, total) if index is not None and total is not None else None
            )
            self.label = label
            self.started = time.monotonic()
            self.status = ""
            self.note = None
            self.bytes = None
            self.item_label = None
            self.item_index = self.item_total = self.items_done = 0
            self.problems = []
            if not self.live:
                prefix = f"[{index}/{total}] " if self.stage else ""
                self.print(Text(" › " + prefix + label))
            self.start_display()

    def item(self, label, index, total):
        with self.lock:
            self.item_label, self.item_index, self.item_total = label, index, total
            self.bytes = None
            self.status = ""
            if not self.live:
                self.print(Text(f"   [{index}/{total}] {label}"))

    def item_done(self, status="ok"):
        with self.lock:
            if status != "interrupted":
                self.items_done = self.item_index
            if status != "ok":
                self.problems.append((self.item_label, status))
                if not self.live:
                    self.print(Text(f"     {status}"))

    def detail(self, text, *, keep_transfer=False, plain=True):
        with self.lock:
            self.status = text
            if not keep_transfer:
                self.bytes = None
            if plain and not self.live:
                self.print(Text("   " + text))

    def summary(self, text):
        with self.lock:
            self.note = text

    def warning(self, text):
        """Keep the full reason above the transient footer, including in logs."""
        with self.lock:
            self.console.print(
                Text(" ! " + text, style=WARN),
                width=width(self.console),
                no_wrap=False,
                overflow="fold",
            )

    def transfer(self, current, total):
        with self.lock:
            now = time.monotonic()
            if self.bytes is None or current < self.bytes[0]:
                self.transfer_started = self.transfer_reported = now
            self.bytes = (current, total)
            if not self.live and now - self.transfer_reported >= 5:
                self.transfer_reported = now
                self.print(Text(f"   {size(current)} / {size(total)}"))

    def outline(self, title, rows):
        """Permanent multi-part plan, e.g. work split across reboots."""
        with self.lock:
            total = width(self.console)
            self.print(Text(""), Text(" " + title))
            for state, number, text, note in rows:
                glyph, style = {
                    "done": ("✓", OK),
                    "now": ("●", ACCENT),
                    "next": ("○", DIM),
                }[state]
                line = Text("   ", no_wrap=True)
                line.append(glyph, style)
                line.append(" ")
                label = f"{number}  {text}" if number else f"   {text}"
                label_cells = max(0, (total - 5) * 2 // 3)
                line.append_text(
                    fit(
                        label,
                        label_cells,
                        "bold" if state == "now" else "" if number else DIM,
                    )
                )
                line.append_text(fit(note, total - 5 - label_cells, DIM))
                self.print(line)
            self.print(Text(""))

    def close(self, failed=False):
        if self.display:
            self.display.stop()
        with self.lock:
            if failed and self.label:
                where = (
                    f"stopped at {self.item_label} ({self.item_index}/{self.item_total})"
                    if self.item_total
                    else "stopped"
                )
                self.problems = []
                self.commit("✗", ERROR, where)
                self.label = None
            else:
                self.done()


@contextmanager
def reporting():
    console = Console()
    token = _current.set(console)
    failed = False
    try:
        yield console
    except BaseException:
        failed = True
        raise
    finally:
        try:
            console.close(failed)
        finally:
            _current.reset(token)


def enabled():
    return _current.get() is not None


def step(label, *, index=None, total=None):
    if console := _current.get():
        console.step(label, index=index, total=total)


def item(label, index, total):
    if console := _current.get():
        console.item(label, index, total)


def item_done(status="ok"):
    if console := _current.get():
        console.item_done(status)


def detail(text, *, keep_transfer=False, plain=True):
    if console := _current.get():
        console.detail(text, keep_transfer=keep_transfer, plain=plain)


def summary(text):
    if console := _current.get():
        console.summary(text)


def warning(text):
    if console := _current.get():
        console.warning(text)


def transfer(current, total):
    if console := _current.get():
        console.transfer(current, total)


def outline(title, rows):
    if console := _current.get():
        console.outline(title, rows)
