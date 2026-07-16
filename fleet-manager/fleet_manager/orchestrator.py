"""
fleet_manager.orchestrator
~~~~~~~~~~~~~~~~~~~~~~~~~~
High-level fleet control: broadcast commands, timed choreography,
and OTA firmware delivery.

All network calls use stdlib ``urllib`` — no third-party dependencies.

Bot HTTP API conventions assumed:
    POST /api/<endpoint>   — JSON body, JSON response
    POST /api/ota          — multipart/form-data firmware binary
"""
from __future__ import annotations

import json
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Callable
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from .discovery import Bot, Fleet

# ─── Constants ───────────────────────────────────────────────────────────────

DEFAULT_PORT: int = 80
DEFAULT_TIMEOUT: float = 5.0
OTA_TIMEOUT: float = 60.0
OTA_CHUNK_SIZE: int = 4096


# ─── Low-level helpers ────────────────────────────────────────────────────────


def _post_json(
    ip: str,
    endpoint: str,
    payload: dict | None = None,
    *,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
) -> dict | None:
    """
    POST JSON to ``http://ip:port/api/<endpoint>``.

    Returns parsed response dict, or ``None`` on any error.
    """
    url = f"http://{ip}:{port}/api/{endpoint.lstrip('/')}"
    body = json.dumps(payload or {}).encode()
    req = Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        with urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            return json.loads(raw.decode()) if raw else {}
    except (URLError, HTTPError, json.JSONDecodeError, OSError):
        return None


# ─── Single-bot command ───────────────────────────────────────────────────────


def send_command(
    bot: Bot,
    endpoint: str,
    payload: dict | None = None,
    *,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
) -> dict | None:
    """
    Send a JSON command to a single bot.

    Args:
        bot:      Target :class:`~fleet_manager.discovery.Bot` (must be online).
        endpoint: API path without leading slash, e.g. ``"servo/move"``.
        payload:  JSON-serializable body dict (optional).
        port:     HTTP port (default 80).
        timeout:  Request timeout in seconds (default 5.0).

    Returns:
        Parsed JSON response dict, or ``None`` on failure / offline bot.

    Example::

        resp = send_command(bot, "led/color", {"r": 255, "g": 0, "b": 0})
    """
    if not bot.is_online or bot.ip in ("N/A", "?", ""):
        return None
    return _post_json(bot.ip, endpoint, payload, port=port, timeout=timeout)


# ─── Fan-out broadcast ────────────────────────────────────────────────────────


def broadcast(
    fleet: Fleet,
    endpoint: str,
    payload: dict | None = None,
    *,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
    workers: int = 10,
    on_result: Callable[[Bot, dict | None], None] | None = None,
) -> dict[str, dict | None]:
    """
    Fan-out a command to every **online** bot in the fleet concurrently.

    Args:
        fleet:     Target Fleet.
        endpoint:  API endpoint, e.g. ``"led/color"``.
        payload:   JSON body dict (optional).
        port:      HTTP port (default 80).
        timeout:   Per-request timeout in seconds (default 5.0).
        workers:   ThreadPoolExecutor concurrency (default 10).
        on_result: Optional callback ``fn(bot, response)`` per result.

    Returns:
        Mapping of ``hostname → response_dict`` (``None`` on failure).

    Example::

        results = broadcast(fleet, "led/color", {"r": 0, "g": 255, "b": 0})
        for hostname, resp in results.items():
            print(hostname, "OK" if resp is not None else "FAILED")
    """
    results: dict[str, dict | None] = {}

    def _send(bot: Bot) -> tuple[str, dict | None]:
        resp = send_command(bot, endpoint, payload, port=port, timeout=timeout)
        if on_result:
            on_result(bot, resp)
        return bot.hostname, resp

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(_send, b) for b in fleet.online]
        for future in as_completed(futures):
            hostname, resp = future.result()
            results[hostname] = resp

    return results


# ─── Choreography sequencer ───────────────────────────────────────────────────


def sequence(
    fleet: Fleet,
    steps: list[tuple[str, dict | None, float]],
    *,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
) -> None:
    """
    Execute an ordered choreography sequence across the fleet.

    Each step broadcasts to all online bots, then sleeps for the
    specified delay before proceeding to the next step.

    Args:
        fleet: Target Fleet.
        steps: List of ``(endpoint, payload, delay_seconds)`` tuples.
        port:  HTTP port (default 80).
        timeout: Per-request timeout in seconds (default 5.0).

    Example::

        sequence(fleet, [
            ("led/color",  {"r": 255, "g": 0,   "b": 0},   1.0),  # red
            ("servo/wave", {"speed": "slow"},               2.0),  # wave
            ("led/color",  {"r": 0,   "g": 255, "b": 0},   0.5),  # green
            ("servo/home", None,                             0.0),  # reset
        ])
    """
    for step_num, (endpoint, payload, delay_s) in enumerate(steps, start=1):
        broadcast(fleet, endpoint, payload, port=port, timeout=timeout)
        if delay_s > 0:
            time.sleep(delay_s)


# ─── OTA firmware update ──────────────────────────────────────────────────────


def ota_update(
    bot: Bot,
    firmware_path: str | Path,
    *,
    port: int = DEFAULT_PORT,
    timeout: float = OTA_TIMEOUT,
    on_progress: Callable[[int, int], None] | None = None,
) -> bool:
    """
    Push a firmware binary to a bot via ``POST /api/ota``.

    Sends the file as ``multipart/form-data`` using only stdlib urllib.
    The bot is expected to reboot automatically on success.

    Args:
        bot:           Target :class:`~fleet_manager.discovery.Bot`.
        firmware_path: Local path to the ``.bin`` firmware file.
        port:          HTTP port (default 80).
        timeout:       Socket timeout for the upload in seconds (default 60).
        on_progress:   Optional callback ``fn(bytes_sent, total_bytes)``.

    Returns:
        ``True`` if the server responded with HTTP 2xx, ``False`` otherwise.

    Raises:
        FileNotFoundError: If the firmware file does not exist.

    Example::

        ok = ota_update(bot, "build/firmware.bin",
                        on_progress=lambda s, t: print(f"{s}/{t}"))
    """
    if not bot.is_online or bot.ip in ("N/A", "?", ""):
        return False

    firmware_path = Path(firmware_path)
    if not firmware_path.exists():
        raise FileNotFoundError(firmware_path)

    firmware_bytes = firmware_path.read_bytes()
    total = len(firmware_bytes)

    boundary = "----FleetManagerOTABoundary"
    header = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="firmware"; '
        f'filename="{firmware_path.name}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode()
    footer = f"\r\n--{boundary}--\r\n".encode()

    body = header + firmware_bytes + footer

    url = f"http://{bot.ip}:{port}/api/ota"
    req = Request(
        url,
        data=body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
        },
    )

    try:
        with urlopen(req, timeout=timeout):
            pass
        if on_progress:
            on_progress(total, total)
        return True
    except (URLError, HTTPError, OSError):
        return False


def ota_update_fleet(
    fleet: Fleet,
    firmware_path: str | Path,
    *,
    port: int = DEFAULT_PORT,
    timeout: float = OTA_TIMEOUT,
    workers: int = 3,
    on_bot_done: Callable[[Bot, bool], None] | None = None,
) -> dict[str, bool]:
    """
    Push a firmware binary to every online bot in the fleet.

    Args:
        fleet:         Target Fleet.
        firmware_path: Local path to the ``.bin`` firmware file.
        port:          HTTP port (default 80).
        timeout:       Socket timeout per bot in seconds (default 60).
        workers:       Concurrent upload workers (default 3 — be gentle).
        on_bot_done:   Optional callback ``fn(bot, success)`` per bot.

    Returns:
        Mapping of ``hostname → True/False`` (success per bot).
    """
    firmware_path = Path(firmware_path)
    results: dict[str, bool] = {}

    def _upload(bot: Bot) -> tuple[str, bool]:
        ok = ota_update(bot, firmware_path, port=port, timeout=timeout)
        if on_bot_done:
            on_bot_done(bot, ok)
        return bot.hostname, ok

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(_upload, b) for b in fleet.online]
        for future in as_completed(futures):
            hostname, ok = future.result()
            results[hostname] = ok

    return results
