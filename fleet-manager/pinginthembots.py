#!/usr/bin/env python3
"""
pinginthembots.py — Robot Fleet Scanner
========================================
Beautiful concurrent ping sweep with live progress, colour-coded results,
and fleet.yaml awareness.  A thin, rich CLI wrapper over fleet_manager.

Usage
-----
    python pinginthembots.py                      # scan default bot families
    python pinginthembots.py paulbot rfbot         # specific families
    python pinginthembots.py --probe               # also probe HTTP /api/info
    python pinginthembots.py --workers 30          # more concurrency
    python pinginthembots.py --suffix-max 4        # only check bots 0-4
    python pinginthembots.py --yaml ../docs/fleet.yaml
    python pinginthembots.py --no-log              # skip CSV output

Exit codes
----------
    0  scan completed (even if all bots are offline)
    1  argument error
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import threading
import time
from pathlib import Path

# ── Allow running without installing the package ──────────────────────────────
_HERE = Path(__file__).parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from fleet_manager.discovery import Bot, Fleet, scan
from fleet_manager.capabilities import probe_fleet

# ─── ANSI colour helpers ──────────────────────────────────────────────────────

# Respect NO_COLOR and non-TTY environments
_USE_COLOR: bool = sys.stdout.isatty() and not os.environ.get("NO_COLOR")


def _esc(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _USE_COLOR else text


def _vlen(s: str) -> int:
    """Visible length: strip ANSI escape sequences before measuring."""
    return len(re.sub(r"\033\[[0-9;]*m", "", s))


def _rpad(colored: str, width: int) -> str:
    """Right-pad a (possibly ANSI-coloured) string to *width* visible chars."""
    return colored + " " * max(0, width - _vlen(colored))


BOLD   = lambda t: _esc("1",  t)
DIM    = lambda t: _esc("2",  t)
CYAN   = lambda t: _esc("96", t)
GREEN  = lambda t: _esc("92", t)
RED    = lambda t: _esc("91", t)
YELLOW = lambda t: _esc("93", t)
GRAY   = lambda t: _esc("90", t)
WHITE  = lambda t: _esc("97", t)
BLINK  = lambda t: _esc("5",  t)


# ─── Banner ───────────────────────────────────────────────────────────────────

_BANNER_LINES = [
    "  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓",
    "  ┃  🤖  R O B O T   F L E E T   S C A N N E R                         ┃",
    "  ┃      Concurrent mDNS ping  ·  fleet.yaml aware  ·  HTTP probing     ┃",
    "  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛",
]


def _print_banner(version: str = "2.0") -> None:
    if _USE_COLOR:
        for i, line in enumerate(_BANNER_LINES):
            color = CYAN if i in (0, 3) else WHITE
            print(color(line))
        print(DIM(f"  v{version}  ·  stdlib-only  ·  zero deps") + "\n")
    else:
        print(f"Robot Fleet Scanner v{version}\n")


# ─── Status / cell formatters ─────────────────────────────────────────────────

def _fmt_status(bot: Bot) -> str:
    if bot.status == "online":
        return GREEN("● Online ")
    if bot.status == "shipped":
        return YELLOW("⊗ Shipped")
    return RED("○ Offline")


def _fmt_latency(bot: Bot) -> str:
    if bot.latency_ms >= 0:
        ms = bot.latency_ms
        color = GREEN if ms < 10 else (YELLOW if ms < 50 else RED)
        return color(f"{ms:6.1f} ms")
    return GRAY("      —   ")


def _fmt_ip(bot: Bot) -> str:
    if bot.ip not in ("N/A", "?", ""):
        return WHITE(bot.ip)
    return GRAY("—")


def _fmt_hostname(bot: Bot) -> str:
    if bot.is_online:
        return BOLD(bot.hostname)
    if bot.shipped:
        return YELLOW(bot.hostname)
    return GRAY(bot.hostname)


# ─── Table renderer ───────────────────────────────────────────────────────────

_COL = {                 # visible column widths
    "hostname":  22,
    "status":     9,
    "ip":        15,
    "latency":   10,
    "platform":  12,
    "firmware":   9,
}
_TOTAL_WIDTH = 2 + sum(_COL.values()) + len(_COL) * 2  # borders + separators


def _rule(char: str = "─") -> str:
    return GRAY(char * (_TOTAL_WIDTH + 2))


def _render_table(fleet: Fleet) -> None:
    # Header
    print(_rule())
    hdr_parts = [
        _rpad(BOLD(CYAN("Hostname")),  _COL["hostname"]),
        _rpad(BOLD(CYAN("Status")),    _COL["status"]),
        _rpad(BOLD(CYAN("IP Address")),_COL["ip"]),
        _rpad(BOLD(CYAN("Latency")),   _COL["latency"]),
        _rpad(BOLD(CYAN("Platform")),  _COL["platform"]),
        BOLD(CYAN("Firmware")),
    ]
    print("  " + "  ".join(hdr_parts))
    print(_rule())

    # Rows
    for bot in fleet.bots:
        row_parts = [
            _rpad(_fmt_hostname(bot), _COL["hostname"]),
            _rpad(_fmt_status(bot),   _COL["status"]),
            _rpad(_fmt_ip(bot),       _COL["ip"]),
            _rpad(_fmt_latency(bot),  _COL["latency"]),
            _rpad(DIM(bot.platform) if bot.platform else GRAY("—"), _COL["platform"]),
            DIM(bot.fw_version) if bot.fw_version else GRAY("—"),
        ]
        print("  " + "  ".join(row_parts))

    print(_rule())


# ─── Summary footer ───────────────────────────────────────────────────────────

def _render_summary(
    fleet: Fleet,
    elapsed: float,
    log_path: Path | None,
) -> None:
    n   = len(fleet.bots)
    on  = len(fleet.online)
    off = len(fleet.offline)
    sh  = len(fleet.shipped)

    print(
        f"\n  {BOLD('Online:')} {GREEN(str(on))}/{n}   "
        f"{BOLD('Offline:')} {RED(str(off))}   "
        f"{BOLD('Shipped:')} {YELLOW(str(sh))}   "
        f"{BOLD('Scan time:')} {elapsed:.2f}s"
    )
    if log_path:
        print(f"  {BOLD('Log:')} {DIM(str(log_path))}")
    print()


# ─── Live progress spinner ────────────────────────────────────────────────────

class _Spinner:
    """Thread-safe braille spinner with running count."""

    _FRAMES = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

    def __init__(self, total: int) -> None:
        self._total = total
        self._done  = 0
        self._lock  = threading.Lock()
        self._stop  = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        idx = 0
        while not self._stop.is_set():
            with self._lock:
                done = self._done
            frame = self._FRAMES[idx % len(self._FRAMES)] if _USE_COLOR else "-"
            print(
                f"\r  {CYAN(frame)} Scanning…  "
                f"{GREEN(str(done))}/{self._total} done   ",
                end="", flush=True,
            )
            idx += 1
            time.sleep(0.08)

    def start(self) -> "_Spinner":
        self._thread.start()
        return self

    def tick(self) -> None:
        with self._lock:
            self._done += 1

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        # Clear the spinner line
        print("\r" + " " * 50 + "\r", end="", flush=True)


# ─── CLI ──────────────────────────────────────────────────────────────────────

_DEFAULT_BOTS = ["rfbot", "mybot", "carbot", "paulbot", "simplebot"]
_DEFAULT_YAML = Path(__file__).parent.parent / "docs" / "fleet.yaml"
_DEFAULT_LOG  = Path(__file__).parent.parent / "docs" / "ping_log.csv"


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="pinginthembots",
        description="🤖  Concurrent robot fleet scanner with fleet.yaml awareness.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python pinginthembots.py\n"
            "  python pinginthembots.py paulbot rfbot --probe\n"
            "  python pinginthembots.py --workers 30 --suffix-max 4\n"
            "  python pinginthembots.py --no-log --no-banner\n"
        ),
    )
    p.add_argument(
        "bases", nargs="*", metavar="BOT_BASE",
        help=(
            "Bot name prefixes to scan "
            f"(default: {' '.join(_DEFAULT_BOTS)})."
        ),
    )
    p.add_argument(
        "--probe", "-p", action="store_true",
        help="Also probe each online bot's HTTP /api/info endpoint.",
    )
    p.add_argument(
        "--workers", "-w", type=int, default=20, metavar="N",
        help="Concurrent ping workers (default: 20).",
    )
    p.add_argument(
        "--timeout", "-t", type=float, default=1.0, metavar="SEC",
        help="Per-ping timeout in seconds (default: 1.0).",
    )
    p.add_argument(
        "--suffix-max", type=int, default=9, metavar="N",
        help="Highest numeric suffix per family (default: 9 → bots 0–9).",
    )
    p.add_argument(
        "--yaml", type=Path, default=_DEFAULT_YAML, metavar="PATH",
        help=f"fleet.yaml path (default: {_DEFAULT_YAML}).",
    )
    p.add_argument(
        "--log", type=Path, default=_DEFAULT_LOG, metavar="PATH",
        help=f"CSV log path (default: {_DEFAULT_LOG}).",
    )
    p.add_argument(
        "--no-log", action="store_true",
        help="Skip writing to the CSV log.",
    )
    p.add_argument(
        "--no-banner", action="store_true",
        help="Suppress the header banner.",
    )
    return p


def main() -> None:
    args = _build_parser().parse_args()
    base_names = args.bases or _DEFAULT_BOTS

    if not args.no_banner:
        _print_banner()

    total = len(base_names) * (args.suffix_max + 1)
    print(
        f"  Scanning {BOLD(str(total))} targets "
        f"across {BOLD(str(len(base_names)))} platform(s) "
        f"· {BOLD(str(args.workers))} workers "
        f"· {args.timeout}s timeout\n"
    )

    spinner = _Spinner(total).start()
    t0 = time.perf_counter()

    fleet = scan(
        base_names,
        max_suffix=args.suffix_max,
        workers=args.workers,
        timeout=args.timeout,
        yaml_path=args.yaml,
        on_result=lambda _: spinner.tick(),
    )

    elapsed = time.perf_counter() - t0
    spinner.stop()

    # Optional HTTP probe
    if args.probe and fleet.online:
        print(
            f"  {CYAN('→')} Probing {BOLD(str(len(fleet.online)))} "
            f"online bot(s) via HTTP /api/info…\n"
        )
        probe_fleet(fleet)

    _render_table(fleet)
    log_path: Path | None = None

    if not args.no_log:
        log_path = fleet.to_csv(args.log, append=True)

    _render_summary(fleet, elapsed, log_path)


if __name__ == "__main__":
    main()
