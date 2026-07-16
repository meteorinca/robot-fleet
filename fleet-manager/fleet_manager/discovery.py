"""
fleet_manager.discovery
~~~~~~~~~~~~~~~~~~~~~~~
Concurrent robot fleet discovery via ICMP ping and fleet.yaml metadata.

Core objects:
    Bot   — dataclass representing one robot.
    Fleet — ordered collection of Bot objects with query helpers.

Key functions:
    load_fleet_yaml(path) → dict[hostname, meta]
    ping_bot(hostname, timeout) → (alive, ip, latency_ms)
    scan(base_names, *, ...) → Fleet
"""
from __future__ import annotations

import csv
import datetime
import os
import platform
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Iterator


# ─── Data model ──────────────────────────────────────────────────────────────


@dataclass
class Bot:
    """Represents one robot in the fleet with all known metadata."""

    hostname: str
    ip: str = "N/A"
    status: str = "unknown"     # "online" | "offline" | "shipped" | "unknown"
    latency_ms: float = -1.0
    platform: str = ""
    fw_version: str = ""
    uptime_s: int = -1
    free_heap: int = -1
    notes: str = ""
    shipped: bool = False
    last_seen: str = ""

    @property
    def is_online(self) -> bool:
        return self.status == "online"

    def __repr__(self) -> str:  # pragma: no cover
        return f"Bot({self.hostname!r}, {self.status!r}, ip={self.ip!r})"


class Fleet:
    """
    Ordered, queryable collection of Bot objects.

    Typical usage::

        fleet = scan(["paulbot", "rfbot"])
        for bot in fleet.online:
            print(bot.hostname, bot.ip)
        fleet.to_csv("docs/ping_log.csv")
    """

    def __init__(
        self,
        bots: list[Bot],
        scanned_at: datetime.datetime | None = None,
    ) -> None:
        self.bots = bots
        self.scanned_at = scanned_at or datetime.datetime.now()

    # ── Filters ──────────────────────────────────────────────────────────────

    @property
    def online(self) -> list[Bot]:
        """All bots that responded to ping."""
        return [b for b in self.bots if b.status == "online"]

    @property
    def offline(self) -> list[Bot]:
        """All bots that did not respond."""
        return [b for b in self.bots if b.status == "offline"]

    @property
    def shipped(self) -> list[Bot]:
        """All bots marked as shipped in fleet.yaml."""
        return [b for b in self.bots if b.shipped]

    def by_platform(self, name: str) -> list[Bot]:
        """Filter by platform name (case-insensitive)."""
        return [b for b in self.bots if b.platform.lower() == name.lower()]

    def get(self, hostname: str) -> Bot | None:
        """Look up a bot by exact hostname."""
        for b in self.bots:
            if b.hostname == hostname:
                return b
        return None

    # ── Export ───────────────────────────────────────────────────────────────

    def to_csv(self, path: str | Path, *, append: bool = True) -> Path:
        """
        Write scan results to a CSV file.

        Args:
            path:   Destination file path.
            append: If True (default), append rows; otherwise overwrite.

        Returns:
            Resolved Path of the written file.
        """
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        file_exists = path.exists() and path.stat().st_size > 0
        mode = "a" if append else "w"
        fieldnames = [
            "Timestamp", "BotName", "Status", "IPAddress",
            "LatencyMs", "Platform", "FirmwareVersion", "Notes",
        ]
        with path.open(mode, newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            if not file_exists or not append:
                writer.writeheader()
            ts = self.scanned_at.strftime("%Y-%m-%d %H:%M:%S")
            for bot in self.bots:
                writer.writerow({
                    "Timestamp": ts,
                    "BotName": bot.hostname,
                    "Status": bot.status.capitalize(),
                    "IPAddress": bot.ip,
                    "LatencyMs": f"{bot.latency_ms:.1f}" if bot.latency_ms >= 0 else "",
                    "Platform": bot.platform,
                    "FirmwareVersion": bot.fw_version,
                    "Notes": bot.notes,
                })
        return path.resolve()

    def to_dict(self) -> list[dict]:
        """Return a list of plain dicts (suitable for pandas, JSON, etc.)."""
        return [asdict(b) for b in self.bots]

    def summary(self) -> str:
        """One-line human-readable summary."""
        n, on, off, sh = (
            len(self.bots), len(self.online), len(self.offline), len(self.shipped)
        )
        return (
            f"Fleet({n} bots): {on} online, {off} offline, {sh} shipped"
            f" — scanned at {self.scanned_at:%H:%M:%S}"
        )

    # ── Dunder ───────────────────────────────────────────────────────────────

    def __len__(self) -> int:
        return len(self.bots)

    def __iter__(self) -> Iterator[Bot]:
        return iter(self.bots)

    def __repr__(self) -> str:  # pragma: no cover
        return f"Fleet({len(self.bots)} bots, {len(self.online)} online)"


# ─── YAML loader ─────────────────────────────────────────────────────────────


def load_fleet_yaml(path: str | Path) -> dict[str, dict]:
    """
    Parse ``fleet.yaml`` and return a mapping of ``hostname → metadata dict``.

    Handles both ``shipped_bots`` and ``active_fleet`` sections.
    Tries PyYAML first; falls back to a hand-rolled parser so the module
    works with zero external dependencies.

    Returns:
        Dict mapping e.g. ``"paulbot9.local"`` to its metadata dict,
        with an injected ``"shipped": bool`` key.
    """
    path = Path(path)
    if not path.exists():
        return {}

    meta: dict[str, dict] = {}

    # ── PyYAML fast-path ──────────────────────────────────────────────────────
    try:
        import yaml as _yaml  # type: ignore

        with path.open(encoding="utf-8") as f:
            doc = _yaml.safe_load(f) or {}

        for entry in doc.get("shipped_bots", []) or []:
            if isinstance(entry, dict) and "hostname" in entry:
                meta[entry["hostname"]] = {**entry, "shipped": True}

        for entry in doc.get("active_fleet", []) or []:
            if isinstance(entry, dict) and "hostname" in entry:
                meta[entry["hostname"]] = {**entry, "shipped": False}

        return meta

    except ImportError:
        pass

    # ── Hand-rolled fallback ──────────────────────────────────────────────────
    current_section: str | None = None
    current_entry: dict = {}

    def _flush(entry: dict, section: str) -> None:
        hostname = entry.get("hostname")
        if hostname:
            meta[hostname] = {**entry, "shipped": section == "shipped_bots"}

    with path.open(encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip()
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue

            # Top-level section key (no leading whitespace, ends with colon)
            if not line[0].isspace() and stripped.endswith(":"):
                if current_entry and current_section:
                    _flush(current_entry, current_section)
                    current_entry = {}
                current_section = stripped[:-1]
                continue

            # New list item
            if stripped.startswith("- "):
                if current_entry and current_section:
                    _flush(current_entry, current_section)
                current_entry = {}
                rest = stripped[2:]
                if ":" in rest:
                    k, _, v = rest.partition(":")
                    current_entry[k.strip()] = v.strip().strip('"')
                continue

            # Indented key: value
            if ":" in stripped and not stripped.startswith("-"):
                k, _, v = stripped.partition(":")
                current_entry[k.strip()] = v.strip().strip('"')

    if current_entry and current_section:
        _flush(current_entry, current_section)

    return meta


# ─── Ping primitive ───────────────────────────────────────────────────────────

_IS_WINDOWS = platform.system().lower() == "windows"


def ping_bot(hostname: str, timeout: float = 1.0) -> tuple[bool, str, float]:
    """
    Send a single ICMP echo to *hostname*.

    Args:
        hostname: Hostname or IP to ping (e.g. ``"paulbot0.local"``).
        timeout:  Timeout in seconds.

    Returns:
        ``(alive, ip_address, latency_ms)`` — latency is ``-1.0`` when down.
    """
    if _IS_WINDOWS:
        cmd = ["ping", "-n", "1", "-w", str(int(timeout * 1000)), hostname]
    else:
        cmd = ["ping", "-c", "1", "-W", str(max(1, int(timeout))), hostname]

    t0 = time.perf_counter()
    try:
        output = subprocess.check_output(
            cmd, stderr=subprocess.STDOUT, universal_newlines=True
        )
        wall_ms = (time.perf_counter() - t0) * 1000

        # Extract IP from "[x.x.x.x]" or "Reply from x.x.x.x:"
        ip_match = re.search(r"\[(\d+\.\d+\.\d+\.\d+)\]", output)
        if not ip_match:
            ip_match = re.search(r"Reply from (\d+\.\d+\.\d+\.\d+):", output)
        ip = ip_match.group(1) if ip_match else "?"

        # Prefer the ping-reported round-trip time
        lat_match = re.search(r"[Tt]ime[=<](\d+(?:\.\d+)?)\s*ms", output)
        latency_ms = float(lat_match.group(1)) if lat_match else wall_ms

        alive = bool(re.search(r"\bTTL=\d+\b", output, re.IGNORECASE))
        return alive, ip, (latency_ms if alive else -1.0)

    except subprocess.CalledProcessError as e:
        ip_match = re.search(r"\[(\d+\.\d+\.\d+\.\d+)\]", e.output or "")
        ip = ip_match.group(1) if ip_match else "N/A"
        return False, ip, -1.0


# ─── Fleet scanner ────────────────────────────────────────────────────────────


def scan(
    base_names: list[str],
    *,
    min_suffix: int = 0,
    max_suffix: int = 9,
    workers: int = 20,
    timeout: float = 1.0,
    yaml_path: str | Path | None = None,
    on_result: Callable[[Bot], None] | None = None,
) -> Fleet:
    """
    Concurrent fleet sweep — pings every ``{base}{i}.local`` combination,
    merges ``fleet.yaml`` metadata, and returns a :class:`Fleet`.

    Args:
        base_names: Bot-name prefixes, e.g. ``["paulbot", "rfbot"]``.
        min_suffix: First numeric suffix to try (default 0).
        max_suffix: Last numeric suffix to try (default 9).
        workers:    ThreadPoolExecutor concurrency (default 20).
        timeout:    Per-ping timeout in seconds (default 1.0).
        yaml_path:  Path to ``fleet.yaml``; uses ``../../docs/fleet.yaml``
                    relative to this file by default.
        on_result:  Optional callback ``fn(bot)`` called as each result
                    arrives (useful for live progress indicators).

    Returns:
        :class:`Fleet` containing all :class:`Bot` results in target order.

    Example::

        fleet = scan(["paulbot", "rfbot"], workers=30)
        print(fleet.summary())
    """
    # Resolve YAML path
    if yaml_path is None:
        yaml_path = Path(__file__).parent.parent.parent / "docs" / "fleet.yaml"
    yaml_meta = load_fleet_yaml(yaml_path)

    # Build ordered target list
    targets: list[str] = [
        f"{base}{i}.local"
        for base in base_names
        for i in range(min_suffix, max_suffix + 1)
    ]

    scanned_at = datetime.datetime.now()

    def _ping(hostname: str) -> Bot:
        meta = yaml_meta.get(hostname, {})
        shipped = bool(meta.get("shipped", False))

        if shipped:
            return Bot(
                hostname=hostname,
                status="shipped",
                shipped=True,
                platform=meta.get("platform", ""),
                fw_version=meta.get("firmware_version", ""),
                notes=meta.get("notes", ""),
            )

        alive, ip, latency = ping_bot(hostname, timeout=timeout)
        return Bot(
            hostname=hostname,
            ip=ip,
            status="online" if alive else "offline",
            latency_ms=latency,
            platform=meta.get("platform", ""),
            fw_version=meta.get("version", meta.get("firmware_version", "")),
            notes=meta.get("notes", ""),
            shipped=False,
            last_seen=(
                scanned_at.strftime("%Y-%m-%d %H:%M:%S") if alive else ""
            ),
        )

    unordered: list[Bot] = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(_ping, h): h for h in targets}
        for future in as_completed(futures):
            bot = future.result()
            unordered.append(bot)
            if on_result:
                on_result(bot)

    # Restore original target order
    order = {h: i for i, h in enumerate(targets)}
    unordered.sort(key=lambda b: order.get(b.hostname, 9_999))

    return Fleet(unordered, scanned_at=scanned_at)
