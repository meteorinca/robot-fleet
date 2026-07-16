"""
fleet_manager.capabilities
~~~~~~~~~~~~~~~~~~~~~~~~~~
HTTP capability probing for online robots.

Targets ``GET /api/info`` on each bot and enriches the Bot object
with firmware version, platform, uptime, and free-heap data.

No third-party dependencies — stdlib ``urllib`` only.

Expected JSON response schema from ``/api/info``::

    {
        "platform":   "dogbot_v1",   // str  — hardware platform identifier
        "firmware":   "1.2.3",       // str  — firmware version string
        "uptime":     3600,          // int  — seconds since last boot
        "free_heap":  182400,        // int  — bytes of free heap
        "hostname":   "dogbot2"      // str  — (optional) self-reported name
    }
"""
from __future__ import annotations

import json
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Callable
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

from .discovery import Bot, Fleet

# ─── Constants ───────────────────────────────────────────────────────────────

INFO_ENDPOINT = "/api/info"
DEFAULT_PORT: int = 80
DEFAULT_TIMEOUT: float = 2.0


# ─── Single-bot probe ────────────────────────────────────────────────────────


def probe_bot(
    bot: Bot,
    *,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
) -> Bot:
    """
    Probe a single bot's HTTP ``/api/info`` endpoint.

    Mutates *bot* in-place and returns it.
    If the bot is offline, or the request fails for any reason,
    the bot is returned unchanged (best-effort — never raises).

    Args:
        bot:     Target :class:`~fleet_manager.discovery.Bot`.
        port:    HTTP port to connect to (default 80).
        timeout: Request timeout in seconds (default 2.0).

    Returns:
        The same *bot* object, enriched with HTTP data if available.
    """
    if not bot.is_online or bot.ip in ("N/A", "?", ""):
        return bot

    url = f"http://{bot.ip}:{port}{INFO_ENDPOINT}"
    try:
        with urlopen(url, timeout=timeout) as resp:
            data: dict = json.loads(resp.read().decode())

        bot.platform = data.get("platform", bot.platform) or bot.platform
        bot.fw_version = data.get("firmware", bot.fw_version) or bot.fw_version
        uptime = data.get("uptime")
        if uptime is not None:
            bot.uptime_s = int(uptime)
        free_heap = data.get("free_heap")
        if free_heap is not None:
            bot.free_heap = int(free_heap)

    except (URLError, HTTPError, json.JSONDecodeError, OSError, ValueError):
        pass  # Best-effort — ignore all probe failures

    return bot


# ─── Fleet-wide probe ────────────────────────────────────────────────────────


def probe_fleet(
    fleet: Fleet,
    *,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
    workers: int = 10,
    on_result: Callable[[Bot], None] | None = None,
) -> Fleet:
    """
    Concurrently probe every *online* bot in the fleet.

    Returns the same :class:`~fleet_manager.discovery.Fleet` object
    with bots enriched in-place.

    Args:
        fleet:     Target Fleet.
        port:      HTTP port (default 80).
        timeout:   Per-request timeout in seconds (default 2.0).
        workers:   ThreadPoolExecutor concurrency (default 10).
        on_result: Optional callback ``fn(bot)`` called after each probe.

    Returns:
        The same *fleet* object, bots enriched where reachable.

    Example::

        fleet = scan(["paulbot"])
        probe_fleet(fleet)
        for bot in fleet.online:
            print(bot.hostname, bot.fw_version, bot.uptime_s)
    """
    online = fleet.online
    if not online:
        return fleet

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {
            pool.submit(probe_bot, b, port=port, timeout=timeout): b
            for b in online
        }
        for future in as_completed(futures):
            bot = future.result()
            if on_result:
                on_result(bot)

    return fleet
