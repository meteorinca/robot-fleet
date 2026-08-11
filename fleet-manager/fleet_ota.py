#!/usr/bin/env python3
"""
fleet_ota.py — Concurrent Fleet OTA Updater for PaulBot / DogBot
===================================================================
Discovers online fleet bots (e.g. paulbot12 .. paulbot22), pairs each with
its corresponding binary from the `device_bins/` directory created by
createallbinsYo.bat (or custom build folder), and performs parallel OTA updates.

Usage
-----
    python fleet_ota.py                                 # dry-run & update paulbot12..22 from device_bins
    python fleet_ota.py --dry-run                       # preview mapping without flashing
    python fleet_ota.py --start 12 --end 22 --workers 10
    python fleet_ota.py --bins-dir ./device_bins --port 80
    python fleet_ota.py --probe                         # query /api/info before flashing

Exit codes
----------
    0  all online targets updated successfully (or dry-run finished)
    1  one or more OTA uploads failed / missing binaries / invalid args
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

# Allow running directly from fleet-manager folder or root
_HERE = Path(__file__).parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from fleet_manager.discovery import Bot, Fleet, ping_bot
from fleet_manager.capabilities import probe_bot
from fleet_manager.orchestrator import ota_update

# ─── ANSI Color Helpers ───────────────────────────────────────────────────────

_USE_COLOR: bool = sys.stdout.isatty() and not os.environ.get("NO_COLOR")


def _esc(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _USE_COLOR else text


def _vlen(s: str) -> int:
    return len(re.sub(r"\033\[[0-9;]*m", "", s))


def _rpad(colored: str, width: int) -> str:
    return colored + " " * max(0, width - _vlen(colored))


BOLD   = lambda t: _esc("1",  t)
DIM    = lambda t: _esc("2",  t)
CYAN   = lambda t: _esc("96", t)
GREEN  = lambda t: _esc("92", t)
RED    = lambda t: _esc("91", t)
YELLOW = lambda t: _esc("93", t)
GRAY   = lambda t: _esc("90", t)
WHITE  = lambda t: _esc("97", t)

# ─── Banner ───────────────────────────────────────────────────────────────────

_BANNER_LINES = [
    "  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓",
    "  ┃  🚀  R O B O T   F L E E T   O T A   U P D A T E R                 ┃",
    "  ┃      Concurrent HTTP OTA  ·  per-device bins  ·  PaulBot v1.9       ┃",
    "  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛",
]


def _print_banner() -> None:
    if _USE_COLOR:
        for i, line in enumerate(_BANNER_LINES):
            color = CYAN if i in (0, 3) else WHITE
            print(color(line))
        print(DIM("  Parallel HTTP flash worker  ·  zero third-party deps") + "\n")
    else:
        print("Robot Fleet OTA Updater\n")


# ─── Bin Resolution Helper ───────────────────────────────────────────────────

def find_bin_for_device(bins_dir: Path, dev_num: int) -> Path | None:
    """
    Find the .bin file for a given device number.
    Checks:
      1. bins_dir / device_{dev_num} / *.bin
      2. bins_dir / device_{dev_num}.bin
      3. bins_dir / paulbot{dev_num}.bin
      4. bins_dir / dogbot_{dev_num}.bin
    """
    # 1. Directory convention from createallbinsYo.bat: device_bins/device_N/*.bin
    sub_dir = bins_dir / f"device_{dev_num}"
    if sub_dir.is_dir():
        bins = list(sub_dir.glob("*.bin"))
        if bins:
            # Sort by modification time descending
            bins.sort(key=lambda p: p.stat().st_mtime, reverse=True)
            return bins[0]

    # 2. File name conventions in root of bins_dir
    candidates = [
        bins_dir / f"device_{dev_num}.bin",
        bins_dir / f"paulbot{dev_num}.bin",
        bins_dir / f"dogbot_{dev_num}.bin",
        bins_dir / f"device_{dev_num}" / "firmware.bin",
    ]
    for cand in candidates:
        if cand.is_file():
            return cand

    return None


# ─── Main CLI ─────────────────────────────────────────────────────────────────

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="fleet_ota",
        description="Concurrent Fleet OTA Firmware Updater for PaulBot.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python fleet_ota.py\n"
            "  python fleet_ota.py --dry-run\n"
            "  python fleet_ota.py --start 12 --end 22 --workers 10\n"
            "  python fleet_ota.py --bins-dir ./device_bins --port 80\n"
        ),
    )
    p.add_argument(
        "--bins-dir", type=Path, default=Path("device_bins"),
        help="Directory containing per-device firmware bins (default: ./device_bins).",
    )
    p.add_argument(
        "--prefix", type=str, default="paulbot",
        help="Bot hostname prefix (default: 'paulbot').",
    )
    p.add_argument(
        "--start", type=int, default=12,
        help="Starting device number (default: 12).",
    )
    p.add_argument(
        "--end", type=int, default=22,
        help="Ending device number (default: 22).",
    )
    p.add_argument(
        "--workers", "-w", type=int, default=10,
        help="Concurrent OTA upload workers (default: 10).",
    )
    p.add_argument(
        "--port", type=int, default=80,
        help="HTTP port for OTA endpoint (default: 80).",
    )
    p.add_argument(
        "--timeout", type=float, default=60.0,
        help="Socket timeout per upload in seconds (default: 60.0).",
    )
    p.add_argument(
        "--probe", action="store_true",
        help="Probe /api/info before flashing to report current firmware version.",
    )
    p.add_argument(
        "--dry-run", action="store_true",
        help="Discover targets and show bin mapping without flashing.",
    )
    return p


def main() -> None:
    args = _build_parser().parse_args()
    _print_banner()

    bins_dir: Path = args.bins_dir
    if not bins_dir.is_dir():
        # Check relative to parent directory if called inside fleet-manager
        alt_bins = Path(__file__).parent / args.bins_dir
        if alt_bins.is_dir():
            bins_dir = alt_bins
        else:
            print(RED(f"Error: Bins directory '{args.bins_dir}' not found."))
            print(GRAY("Run createallbinsYo.bat first to generate device_bins/"))
            sys.exit(1)

    devices = list(range(args.start, args.end + 1))
    hostnames = [f"{args.prefix}{n}" for n in devices]

    print(
        f"  Target devices: {BOLD(f'{args.prefix}{args.start} .. {args.prefix}{args.end}')} "
        f"({len(devices)} total)\n"
        f"  Bins folder:    {CYAN(str(bins_dir.resolve()))}\n"
        f"  Concurrency:    {BOLD(str(args.workers))} parallel workers\n"
    )

    # 1. Ping sweep target bots
    print(CYAN("  [1/3] Scanning targets on network..."))
    
    def _ping_target(h: str) -> Bot:
        host = f"{h}.local" if not h.endswith(".local") else h
        alive, ip, latency = ping_bot(host, timeout=1.0)
        return Bot(
            hostname=h,
            ip=ip,
            status="online" if alive else "offline",
            latency_ms=latency,
        )

    bots: list[Bot] = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(_ping_target, host): host for host in hostnames}
        for fut in as_completed(futures):
            bots.append(fut.result())

    # Sort by hostname device number
    def _dev_num(b: Bot) -> int:
        m = re.search(r"\d+", b.hostname)
        return int(m.group(0)) if m else 0

    bots.sort(key=_dev_num)

    # Optional HTTP probe
    if args.probe:
        print(CYAN("  Probing /api/info endpoints..."))
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futures = [pool.submit(probe_bot, b, port=args.port) for b in bots if b.is_online]
            for fut in as_completed(futures):
                fut.result()

    # 2. Map binaries
    print(CYAN("  [2/3] Mapping binaries to targets...\n"))

    targets_to_flash: list[tuple[Bot, Path, int]] = []  # (Bot, bin_path, dev_num)
    
    # Render table header
    col_w = {"host": 16, "status": 10, "ip": 15, "fw": 10, "bin": 32, "size": 10}
    tot_w = sum(col_w.values()) + 14
    rule = GRAY("─" * tot_w)

    print(rule)
    print(
        "  "
        + _rpad(BOLD(WHITE("Target Bot")), col_w["host"])
        + _rpad(BOLD(WHITE("Status")), col_w["status"])
        + _rpad(BOLD(WHITE("IP Address")), col_w["ip"])
        + _rpad(BOLD(WHITE("FW Ver")), col_w["fw"])
        + _rpad(BOLD(WHITE("Bin File")), col_w["bin"])
        + BOLD(WHITE("Size"))
    )
    print(rule)

    for bot in bots:
        n = _dev_num(bot)
        bin_path = find_bin_for_device(bins_dir, n)
        
        status_str = GREEN("● Online ") if bot.is_online else RED("○ Offline")
        ip_str = bot.ip if bot.is_online else GRAY("—")
        fw_str = bot.fw_version if bot.fw_version else GRAY("—")
        
        if bin_path:
            bin_str = bin_path.name
            size_kb = f"{bin_path.stat().st_size / 1024:.1f} KB"
            if bot.is_online:
                targets_to_flash.append((bot, bin_path, n))
        else:
            bin_str = RED("MISSING")
            size_kb = GRAY("—")

        print(
            "  "
            + _rpad(BOLD(bot.hostname), col_w["host"])
            + _rpad(status_str, col_w["status"])
            + _rpad(ip_str, col_w["ip"])
            + _rpad(fw_str, col_w["fw"])
            + _rpad(CYAN(bin_str) if bin_path else RED(bin_str), col_w["bin"])
            + size_kb
        )

    print(rule)
    print(
        f"\n  Online & ready for OTA: {GREEN(str(len(targets_to_flash)))} / {len(bots)} bots\n"
    )

    if args.dry_run:
        print(YELLOW("  [DRY RUN COMPLETE] No firmwares were flashed."))
        sys.exit(0)

    if not targets_to_flash:
        print(RED("  No online targets with valid firmware binaries found. Aborting."))
        sys.exit(1)

    # 3. Perform OTA Flash
    print(CYAN(f"  [3/3] Flashing {len(targets_to_flash)} bots simultaneously via POST /ota...\n"))

    results: dict[str, tuple[bool, float]] = {}  # hostname -> (success, elapsed)
    lock = threading.Lock()

    def _flash_worker(target: tuple[Bot, Path, int]) -> tuple[str, bool, float]:
        bot, path, _ = target
        t0 = time.perf_counter()
        print(f"  {CYAN('⚡')} Starting OTA on {BOLD(bot.hostname)} ({bot.ip})...")
        
        ok = ota_update(
            bot,
            path,
            port=args.port,
            timeout=args.timeout,
        )
        elapsed = time.perf_counter() - t0
        
        with lock:
            if ok:
                print(f"  {GREEN('✔')} {BOLD(bot.hostname)}: {GREEN('SUCCESS')} in {elapsed:.2f}s (rebooting...)")
            else:
                print(f"  {RED('✖')} {BOLD(bot.hostname)}: {RED('FAILED')} after {elapsed:.2f}s")
                
        return bot.hostname, ok, elapsed

    t_start = time.perf_counter()

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(_flash_worker, target) for target in targets_to_flash]
        for fut in as_completed(futures):
            host, ok, el = fut.result()
            results[host] = (ok, el)

    total_time = time.perf_counter() - t_start
    success_count = sum(1 for ok, _ in results.values() if ok)
    fail_count = len(results) - success_count

    # Summary
    print(rule)
    print(
        f"  {BOLD('Fleet OTA Complete:')} "
        f"{GREEN(str(success_count))} updated successfully, "
        f"{RED(str(fail_count))} failed  ·  Total elapsed: {total_time:.2f}s"
    )
    print(rule + "\n")

    if fail_count > 0:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
