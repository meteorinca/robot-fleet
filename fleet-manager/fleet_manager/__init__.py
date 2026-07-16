"""
fleet_manager
~~~~~~~~~~~~~
Robot fleet management toolkit.

Provides concurrent discovery, HTTP capability probing, and
high-level orchestration for ESP32-based robot fleets.

Quick start::

    from fleet_manager import scan, probe_fleet, broadcast

    fleet = scan(["paulbot", "rfbot"], workers=20)
    print(fleet.summary())

    probe_fleet(fleet)                          # enrich with /api/info data
    broadcast(fleet, "led/blink", {"hz": 2})   # fan-out command
"""
from .discovery import Bot, Fleet, load_fleet_yaml, ping_bot, scan
from .capabilities import probe_bot, probe_fleet
from .orchestrator import (
    broadcast,
    ota_update,
    ota_update_fleet,
    send_command,
    sequence,
)

__all__ = [
    # Data model
    "Bot",
    "Fleet",
    # Discovery
    "load_fleet_yaml",
    "ping_bot",
    "scan",
    # Capabilities
    "probe_bot",
    "probe_fleet",
    # Orchestration
    "send_command",
    "broadcast",
    "sequence",
    "ota_update",
    "ota_update_fleet",
]

__version__ = "2.0.0"
__author__ = "robot-fleet"
