# 🛡️ MatCom Guard - System Monitoring and Security

🇬🇧 English (you are here) · 🇪🇸 [Leer en español](README.es.md)

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Platform](https://img.shields.io/badge/platform-Linux-blue)]()
[![License](https://img.shields.io/badge/license-Open%20Source-orange)]()
[![Version](https://img.shields.io/badge/version-1.0-red)]()
[![GTK](https://img.shields.io/badge/GUI-GTK%2B3-purple)]()
[![Language](https://img.shields.io/badge/language-C99-blue)]()

> *In this vast digital realm, computer viruses and threats are like plagues and invading armies seeking to corrupt your lands and plunder your resources. MatCom Guard is your wall and your royal guard, a real-time monitoring and security system designed to watch over and protect your realm (a UNIX/Linux system) from any intruder or suspicious activity.*

## 🌟 Highlighted Features

- **🔒 Comprehensive Real-Time Monitoring**: USB, processes, and network ports
- **🎯 Differentiated USB Functionality**: a unique snapshot and advanced-detection system
- **⚡ Modern Graphical Interface**: GTK+3-based GUI with a centralized dashboard
- **🧠 Intelligent Analysis**: advanced threat-detection heuristics
- **📊 Professional Export**: PDF reports with embedded charts
- **🔧 Flexible Configuration**: customizable thresholds and auto-scan
- **🛡️ Thread-Safe Architecture**: a robust multi-threaded system

## 📋 Table of Contents

- [🌟 Highlighted Features](#-highlighted-features)
- [⚡ Quick View](#-quick-view)
- [📸 Screenshots](#-screenshots)
- [🏗️ System Architecture](#️-system-architecture)
- [🛠️ Installation and Build](#️-installation-and-build)
- [📖 Usage Guide](#-usage-guide)
- [🔧 Advanced Features](#-advanced-features)
- [📚 Technical Documentation](#-technical-documentation)
- [🔧 Troubleshooting](#-troubleshooting)
- [🤝 Contributing](#-contributing)
- [📜 License and Credits](#-license-and-credits)

## ⚡ Quick View

```bash
# Quick build
make clean && make

# Run
./matcom-guard

# Check dependencies
make check-deps
```

**Main functions available right away:**
- 🔍 **USB Monitor**: automatic device detection and forensic analysis
- ⚡ **Process Monitor**: real-time CPU/memory alerts
- 🔌 **Port Scanner**: quick scan (1-1024) and full scan (1-65535)
- 📊 **Dashboard**: a consolidated view of system status
- 📄 **Export PDF**: professional reports with one click
- ⚙️ **Configuration**: `matcomguard.conf` for the backend, plus an in-app "Configuración" dialog

## 📸 Screenshots

| Dashboard | USB Monitor |
|:---:|:---:|
| ![Dashboard](docs/screenshots/dashboard.png) | ![USB Monitor](docs/screenshots/usb-monitor.png) |

| Process Monitor | Port Scanner |
|:---:|:---:|
| ![Process Monitor](docs/screenshots/process-monitor.png) | ![Port Scanner](docs/screenshots/port-scanner.png) |

<details>
<summary>Live log view</summary>

![Logs](docs/screenshots/logs.png)

</details>

## 🏗️ System Architecture

MatCom Guard is built with a 3-layer architecture that keeps the backend fully independent from the GUI:

```
┌─────────────────────────────────────────────────────────────┐
│                   PRESENTATION LAYER                         │
│   "Night Watch" shell: icon rail + status badge + 5 panels   │
│  ┌───────────┬───────────┬───────────┬───────────┬────────┐ │
│  │ Dashboard │    USB    │ Processes │   Ports   │  Logs  │ │
│  └───────────┴───────────┴───────────┴───────────┴────────┘ │
│              Config dialog (tabbed, modal)                   │
└─────────────────────────────────────────────────────────────┘
                               │
┌─────────────────────────────────────────────────────────────┐
│                   INTEGRATION LAYER                          │
│  ┌─────────────────┬─────────────────┬─────────────────┐    │
│  │  GUI-Backend    │   System         │   Thread         │    │
│  │   Adapters      │   Coordinator    │   Bridge         │    │
│  └─────────────────┴─────────────────┴─────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                               │
┌─────────────────────────────────────────────────────────────┐
│                     BACKEND LAYER                             │
│  ┌─────────────────┬─────────────────┬─────────────────┐    │
│  │   USB             │   Process        │   Port           │    │
│  │   Monitor          │   Monitor        │   Scanner        │    │
│  └─────────────────┴─────────────────┴─────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

Each panel owns its own worker thread (or reuses the shared `gui_periodic_worker`
module) and only ever touches GTK widgets through `g_idle_add`/
`gui_thread_bridge_post` — the backend never calls into GTK directly.
Navigation is a narrow icon rail on the left (Dashboard/USB/Processes/Ports/Logs);
there's no tabbed notebook anymore.

### 🧩 Key Components

#### **📊 Centralized Dashboard**
- **Unified view** of every system module
- **Real-time statistics** on devices, processes, and ports
- **Global system status** with a color-coded status badge
- **Quick access** to every main feature

#### **💾 Advanced USB System**
```c
typedef struct {
    char *device_name;          // Unique device identifier
    FileInfo **files;           // Dynamic array of analyzed files
    int file_count;             // File count in the snapshot
    int capacity;                // Array capacity (files may be added incrementally)
    time_t snapshot_time;       // Snapshot creation timestamp
} DeviceSnapshot;
// Each FileInfo carries its own sha256_hash[65] for integrity checking.
```

**Differentiated functionality:**
- **🔄 "Actualizar" button**: the ONLY one able to retake reference snapshots
- **🔍 "Escaneo Profundo" button**: comparative analysis WITHOUT altering the baseline
- **🚨 Alert System**: advanced threat-detection heuristics

#### **⚡ Intelligent Process Monitor**
```c
typedef struct {
    pid_t pid;                  // Process identifier
    char name[256];             // Executable name
    float cpu_usage;            // CPU usage percentage
    float mem_usage;            // Memory usage percentage
    time_t inicio_alerta;       // Timestamp the active alert started
    int alerta_activa;          // 1 if an alert is currently active
    int is_whitelisted;         // Whitelist status
} ProcessInfo;
```

#### **🔌 Professional Port Scanner**
```c
typedef struct {
    int port;                   // Port number
    char service_name[64];      // Identified service name
    int is_open;                // Port state (open/closed)
    int is_suspicious;          // Security risk assessment
} PortInfo;
```

## 🛠️ Installation and Build

### **📋 System Requirements**

#### **Operating System**
- Linux (Ubuntu 18.04+, Debian 10+, CentOS 7+, Arch Linux)
- Kernel 3.2+ with `/proc` and `/sys` support
- Access to USB devices and network permissions

#### **Essential Dependencies**
```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install build-essential pkg-config git
sudo apt-get install libgtk-3-dev libcairo2-dev cairo-pdf-dev
sudo apt-get install libssl-dev libudev-dev
sudo apt-get install pthread libc6-dev

# CentOS/RHEL/Fedora
sudo dnf groupinstall "Development Tools"
sudo dnf install gtk3-devel cairo-devel cairo-pdf-devel
sudo dnf install openssl-devel libudev-devel
sudo dnf install pkg-config git

# Arch Linux
sudo pacman -S base-devel gtk3 cairo openssl libudev pkg-config git
```

#### **Libraries Used**
| Library | Version | Purpose |
|------------|---------|-----------|
| **GTK+ 3.0** | ≥3.20 | Modern, responsive graphical interface |
| **Cairo/Cairo-PDF** | ≥1.14 | Graphics rendering and PDF export |
| **OpenSSL** | ≥1.1 | Cryptography for SHA-256 hashes |
| **libudev** | ≥230 | USB device monitoring on Linux |
| **pthreads** | POSIX | Multi-threading for real-time monitoring |

### **🔧 Build Process**

#### **Standard Installation**
```bash
# 1. Clone the repository
git clone https://github.com/jccarmenate/MatCom-Guard-SO-Project.git
cd MatCom-Guard-SO-Project

# 2. Check dependencies
make check-deps

# 3. Build the project
make clean && make

# 4. Run the application
./matcom-guard

# 5. Install on the system (optional)
sudo make install
```

#### **Build with debug options**
```bash
# For development and debugging
make debug

# For memory analysis
make clean
CFLAGS="-g -DDEBUG -O0 -fsanitize=address" make

# Run with Valgrind
valgrind --leak-check=full --show-leak-kinds=all ./matcom-guard
```

### **⚙️ Smart Makefile**
The project includes a complete Makefile with multiple build options:

```makefile
# Optimized configuration for development and production
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -Iinclude -DDEBUG
GTK_FLAGS = `pkg-config --cflags --libs gtk+-3.0`
CAIRO_FLAGS = `pkg-config --cflags --libs cairo cairo-pdf`
LIBS = -lcrypto -lpthread -ludev

# Available commands:
make                    # Standard build
make clean              # Clean object files
make debug             # Build with debug symbols
make install           # Install on the system
make test              # Run the program
make check-deps        # Check dependencies
```

**Makefile features:**
- **Automatic dependency detection** with `check-deps`
- **Conditional build support** with debug flags
- **Automatic cleanup** of temporary files
- **System installation** with elevated privileges

### **🔐 Permission Configuration**

#### **Permissions for USB Devices**
```bash
# Add the user to the plugdev group for USB access
sudo usermod -a -G plugdev $USER

# Create a custom udev rule (optional)
echo 'SUBSYSTEM=="usb", GROUP="plugdev", MODE="0664"' | \
sudo tee /etc/udev/rules.d/99-matcom-guard-usb.rules

# Reload udev rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

#### **Permissions for Process Monitoring**
```bash
# For full monitoring of system processes
sudo chmod +s ./matcom-guard

# Or run with elevated privileges
sudo ./matcom-guard
```

### **📁 Project Structure**
```
MatCom-Guard-SO-Project/
├── Makefile                       # Build system
├── matcomguard.conf                # Backend config file (thresholds, whitelist)
├── README.md / README.es.md       # This documentation
├── include/                       # Project headers
│   ├── device_monitor.h           # USB device monitor
│   ├── process_monitor.h          # Process monitor
│   ├── port_scanner.h             # Port scanner
│   ├── threadpool.h, progress.h   # Shared backend/GUI primitives
│   └── gui*.h                     # GUI: shell, panels, adapters, widgets
├── src/                           # Main source code
│   ├── main.c                     # Program entry point
│   ├── device_monitor.c           # USB monitor implementation
│   ├── process_monitor.c          # Process monitor implementation
│   ├── port_scanner.c             # Port scanner implementation
│   └── gui/                       # Graphical interface code
│       ├── gui_main.c             # Window setup, backend lifecycle, action bar
│       ├── gui_shell.c            # Night Watch shell: icon rail + status badge
│       ├── gui_backend_adapters.c # Backend struct -> GUI struct conversions
│       ├── gui_system_coordinator.c # Cross-module state, security scoring
│       ├── gui_config_dialog.c    # Settings dialog (tabbed)
│       ├── gui_thread_bridge.c    # Cross-thread-safe progress delivery
│       ├── gui_periodic_worker.c  # Shared interruptible background-loop helper
│       ├── panels/                # The 5 real panels (dashboard/usb/process/ports/logs)
│       ├── widgets/                # guard_dial (Cairo gauge), gui_icons (Cairo icon set)
│       └── window/                # gui_status.c (system-status -> shell badge glue)
├── tests/unit/                    # Assert-based unit tests (see `make test-unit`)
└── docs/                          # Design specs, plans, screenshots
```

## 📖 Usage Guide

### **🚀 Quick Start**

1. **Launch the Application**
   ```bash
   ./matcom-guard
   ```

2. **Main Dashboard**
   - General overview of system status
   - Real-time statistics
   - Quick access to every module

3. **Basic Scan**
   - **Ports**: "Quick Scan" and "Full Scan" buttons
   - **Processes**: automatic monitoring with alerts
   - **USB**: unique differentiated functionality

### **🔌 USB Device Monitoring**

#### **Unique Differentiated Functionality**

**🔄 "Refresh" Button**
- **Exclusive function**: the only one able to retake snapshots
- **Use**: after legitimate changes to devices
- **Result**: establishes a new reference baseline
- **GUI state**: marks devices as "UPDATED"

**🔍 "Deep Scan" Button**
- **Non-destructive function**: compares without altering snapshots
- **Use**: periodic security verification
- **Result**: detects changes without modifying the baseline
- **GUI states**: "CLEAN", "CHANGES", "SUSPICIOUS"

#### **Threat-Detection Criteria**
```
Suspicious Activity:
├── Mass Deletion: >10% of files deleted
├── Mass Modification: >20% of files modified
├── High Activity: >30% total changes
└── Injection: many new files on small devices
```

### **📊 Process Monitoring**

- **Real time**: continuous CPU and memory updates
- **Smart Alerts**: suspicious-process detection
- **Detailed Info**: PID, name, user, state
- **Actions**: safe termination of problematic processes

### **🔍 Port Scanning**

- **Quick Scan**: common ports (21, 22, 23, 25, 53, 80, 110, 443, 993, 995)
- **Full Scan**: wide port range (1-65535)
- **Service Detection**: automatic service identification
- **Threat Analysis**: security risk assessment

## 🔧 Advanced Features

### **📄 PDF Report Export**

```c
// Professional reporting system
- Professional format with logos
- Detailed information from every module
- Complete timestamps and metadata
```

### **🔄 Advanced Logging System**

```c
// Logging categories
- INFO: general information
- WARNING: important warnings
- ERROR: system errors
- ALERT: detected threats
```

### **⚙️ Flexible Configuration**

There are two independent configuration surfaces today:

**Backend config** — `matcomguard.conf` (project root), read by `process_monitor.c`
at startup:
```properties
# File: matcomguard.conf
UMBRAL_CPU=70.0          # CPU threshold for the backend's own alert loop (%)
UMBRAL_RAM=50.0          # Memory threshold for the backend's own alert loop (%)
INTERVALO=5              # Monitoring interval (seconds)
DURACION_ALERTA=10       # Alert duration (seconds)
WHITELIST=systemd,kthreadd,ksoftirqd,migration,rcu_gp,rcu_par_gp,watchdog,stress,yes
```

**GUI config** — the "Configuración" dialog (top action bar), persisted to
`~/.config/matcom-guard/config.ini`. Its **Umbrales** tab (CPU/memory
thresholds) drives the Process panel's row coloring directly. The other tabs
(scan intervals, auto-scan toggles, sound/notifications, port range,
whitelist) are stored and editable, but nothing currently reads them back
into scan behavior — they're not wired to the backend yet.

### **🛡️ Thread-Safe Security**

```c
// Robust protection against race conditions
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int should_stop_monitoring = 0;

// Smart timeout to avoid deadlocks
int timeout_seconds = 3;
// Periodic check every second
```

## 📚 Technical Documentation

### **🔍 Main APIs**

Each panel owns its module end-to-end: it builds its GTK widgets, wires the
backend's callbacks, and exposes a small lifecycle API to `gui_main.c`.

#### **USB Panel** (`gui_usb_panel.h`)
```c
GtkWidget *gui_usb_panel_create(void);     // Builds the panel, starts auto-monitoring
void gui_usb_panel_shutdown(void);         // Stops monitoring, frees the snapshot cache
```
Backed by `device_monitor.h`'s `create_device_snapshot_ex()` (cancellable,
hash-pool-parallelized) and `free_device_snapshot()`.

#### **Process Panel** (`gui_process_panel.h`)
```c
GtkWidget *gui_process_panel_create(void);
void gui_process_panel_shutdown(void);
int get_process_statistics_for_gui(int *total_processes, int *high_cpu_count,
                                   int *high_memory_count, int *suspicious_count);
```
Backed by `process_monitor.h`'s `start_monitoring()`/`stop_monitoring()` and
`get_process_list_copy()`.

#### **Ports Panel** (`gui_ports_panel.h`)
```c
GtkWidget *gui_ports_panel_create(void);
void gui_ports_panel_shutdown(void);
```
Backed by `port_scanner.h`'s real multi-threaded, cancellable scanner:
```c
int scan_ports_range(int start_port, int end_port, int num_threads,
                      ProgressCallback cb, void *user_data,
                      volatile sig_atomic_t *cancel, ScanResult *out);
```

### **🧪 Test Cases**

#### **USB Functionality Tests**
```bash
# Test 1: Differentiated functionality
1. Connect a USB device
2. Press "Refresh" → verify "UPDATED" status
3. Modify files on the device
4. Press "Deep Scan" → verify change detection
5. Verify the snapshot didn't change

# Test 2: Threat detection
1. Delete >10% of files → verify "SUSPICIOUS" status
2. Modify >20% of files → verify a security alert
```

#### **Robustness Tests**
```bash
# Clean-shutdown test
1. Run full monitoring
2. Close the application → verify termination in <5 seconds
3. Verify no zombie processes remain

# Concurrency test
1. Run multiple simultaneous scans
2. Verify protection against race conditions
3. Verify correct resource cleanup
```

## 🔧 Troubleshooting

### **❌ Common Issues**

#### **Build Error**
```bash
# Error: pkg-config not found
sudo apt-get install pkg-config

# Error: GTK+ headers not found
sudo apt-get install libgtk-3-dev

# Error: libcrypto not found
sudo apt-get install libssl-dev
```

#### **Runtime Issues**
```bash
# Error: cannot access USB devices
sudo usermod -a -G plugdev $USER
# Restart the session afterward

# Error: insufficient permissions for processes
sudo chmod +s ./matcom-guard
# Or run with sudo for full functionality

# Error: cannot create the PDF file
sudo apt-get install cairo-pdf-dev
# Or check write permissions on the directory
```

#### **Performance Issues**
```bash
# High CPU load
- Adjust scan intervals in the configuration
- Use quick scan instead of full scan

# Excessive memory use
- Verify USB-snapshot cleanup
- Check the logs for memory leaks
```

### **🔍 Debugging**

```bash
# Build with debug info
make clean
CFLAGS="-g -DDEBUG -O0" make

# Run with gdb
gdb ./matcom-guard

# Check for memory leaks
valgrind --leak-check=full ./matcom-guard
```

### **📞 Support**

For unresolved issues:

1. **Check the logs** in the graphical interface
2. **Consult the documentation** in `/docs`
3. **Run the tests** in `/tests`
4. **Report issues** with complete system information

## 🤝 Contributing

### **🌟 How to Contribute**

1. **Fork** the repository
2. **Create a branch** for the new feature
3. **Implement** with complete documentation
4. **Run regression tests**
5. **Submit a Pull Request** with a detailed description

### **📋 Code Standards**

- **Style**: standard C99 with JSDoc-style comments
- **Naming**: snake_case for functions, UPPER_CASE for constants
- **Documentation**: Doxygen-style for every public function
- **Testing**: test cases for every new feature

### **🏆 Areas for Improvement**

- **Multi-Platform Support**: extend to Windows/macOS
- **ML Analysis**: anomaly detection with machine learning
- **REST API**: web interface for remote monitoring
- **Database**: log and statistics persistence

---

## 📜 Credits and License

**Developed for the Operating Systems course project - MatCom**

### **🔗 Resources Used**
- [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)
- [GTK+ Documentation](https://www.gtk.org/docs/)
- [Linux Man Pages - proc(5)](https://man7.org/linux/man-pages/man5/proc.5.html)

### **⚖️ License**
This project is open source and available under an educational license for academic purposes.

---

*MatCom Guard v1.0 - Your reliable digital guard* 🛡️
