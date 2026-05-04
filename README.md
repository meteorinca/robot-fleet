# Robot Fleet

A modular framework for managing a fleet of ESP32-based robots.

## Project Structure

```text
robot-fleet/
├── firmware/                    # ESP-IDF monorepo
│   ├── common/                  # Shared code across all robots
│   │   ├── wifi_mgr/            # WiFi management logic
│   │   ├── timekeep/            # Time synchronization and RTC
│   │   └── web_server/          # Embedded web server for local control
│   ├── platforms/               # Per-robot-type configurations
│   │   ├── dogbot_v1/           # Current quadruped configuration
│   │   │   ├── sdkconfig.defaults
│   │   │   ├── partitions.csv
│   │   │   └── CMakeLists.txt
│   │   ├── dogbot_v2/           # Future version (8 servos)
│   │   │   ├── sdkconfig.defaults
│   │   │   └── ...
│   │   └── sensorbot/           # Different robot type (environmental sensing)
│   │       └── ...
│   └── components/              # Hardware abstraction layer
│       ├── robot_hw/            # Auto-detects capabilities
│       │   ├── robot_hw.h
│       │   └── robot_hw.c
│       └── ...
├── fleet-manager/               # Python package for control
│   ├── fleet_manager/
│   │   ├── __init__.py
│   │   ├── discovery.py         # mDNS scanning and robot discovery
│   │   ├── capabilities.py      # Robot capability registry
│   │   └── orchestrator.py      # High-level choreography and fleet sync
│   └── pyproject.toml
├── notebooks/                   # Jupyter for experimentation
│   ├── 01-discover-fleet.ipynb
│   ├── 02-fleet-dance.ipynb
│   └── 03-ota-update.ipynb
├── dashboard/                   # Static monitoring site
│   └── fleet-status.html        # Single-file dashboard for GitHub Pages
└── README.md                    # The source of truth
```