# Hardware Monitor Daemon (hw_monitor_daemon)

## Overview
A production-ready foundational architecture for an advanced hardware monitoring service in C. Focuses on framework rather than sensor logic:
- Daemon lifecycle with graceful shutdown
- Synchronized multithreaded design (templates for CPU/RAM monitors)
- Thread-safe basic logging module
- Strict build with GCC

## Project Structure
```
.
├── include/
│   ├── logger.h      # Logging interface & thread-safe config
│   └── threads.h     # Thread templates, sync primitives (mutex, cond)
├── src/
│   ├── main.c        # Daemon entry, signal handling, main loop, cleanup
│   ├── logger.c      # Event logging impl (levels, timestamps, mutex)
│   └── threads.c     # Placeholder monitor threads (extendable)
├── build/            # Compiled binaries (from make)
├── logs/             # (Reserved; logs use /tmp/hw_monitor.log)
└── Makefile          # Build with -Wall -Wextra -Werror -pthread
```

## Build & Run
```bash
make clean && make
./build/hw_monitor_daemon  # Runs as daemon
```

## Key Features (Framework)
- **Daemonization**: Double-fork, setsid, std fd close for background.
- **Signals**: SIGTERM/SIGINT handling for shutdown.
- **Multithreading**: pthread-based, shared mutex/condvar, volatile running flag.
- **Logging**: Basic module in src/logger.c for system events, levels DEBUG-FATAL.
- **Lifecycle**: Main loop + cleanup ensures memory safety/no leaks.
- **Shutdown Test**: `kill -TERM <pid>` (pid in /tmp/hw_monitor.log); verify orderly thread join.

## Enhancements: Producer-Consumer with Ring Buffer
- **Ring Buffer** (`include/ring_buffer.h` / `src/ring_buffer.c`): Pre-sized circular struct (16 elems) for sensor data (timestamp, type="CPU"/"RAM", value %). Thread-safe push/pop.
- **Producers**: cpu_monitor_thread/ram_monitor_thread now gen dummy rand() % values, push to buffer (block on full via condvar).
- **Consumer**: New `consumer_thread` pops items, logs via logger (uses LOG_SENSOR; condvar on empty).
- **Sync**: Extended mutex/conds (buffer_not_empty/full) prevent busy-waiting; graceful shutdown via broadcasts.

## Testing
- Build/run as before.
- Logs (`/tmp/hw_monitor.log`) now show: ring init, producer gens (e.g., "CPU producer: 72.00%"), consumer logs (e.g., "Sensor data: CPU=72.00%"), DEBUG waits (cond usage), clean shutdown.
- Verify no busy-wait: grep "waiting..." in logs.

## Next Steps (Beyond Framework)
- Implement sensor reading in thread templates (e.g., /proc stats for CPU/RAM).
- Add config, PID file, error recovery, export formats.
- Tune buffer size/intervals.

See main.c, threads.c, ring_buffer.h etc. for details. Production-ready base for zero-to-one project.
