CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -Iinclude -DDEBUG
GTK_FLAGS = `pkg-config --cflags --libs gtk+-3.0`
CAIRO_FLAGS = `pkg-config --cflags --libs cairo cairo-pdf`
LIBS = -lcrypto -lpthread -ludev

TARGET = matcom-guard
SRC = src/main.c \
		src/port_scanner.c \
		src/threadpool.c \
		src/process_monitor.c \
		src/device_monitor.c \
		src/gui/widgets/guard_dial.c \
		src/gui/widgets/gui_icons.c \
		src/gui/gui_thread_bridge.c \
		src/gui/gui_periodic_worker.c \
		src/gui/gui_shell.c \
		src/gui/gui_main.c \
		src/gui/panels/gui_logs_panel.c \
		src/gui/panels/gui_dashboard_panel.c \
		src/gui/panels/gui_usb_panel.c \
		src/gui/panels/gui_process_panel.c \
		src/gui/window/gui_status.c \
		src/gui/window/gui_ports_panel.c \
		src/gui/window/gui_config_dialog.c \
		src/gui/integration/gui_system_coordinator.c \
		src/gui/gui_backend_adapters.c \
		src/gui/integration/gui_ports_integration.c
		
.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(GTK_FLAGS) $(CAIRO_FLAGS) $(LIBS)

clean:
	rm -f $(TARGET) *.o
	rm -f tests/unit/test_progress tests/unit/test_threadpool tests/unit/test_port_classification tests/unit/test_port_scan_range tests/unit/test_hash_skip tests/unit/test_process_shutdown tests/unit/test_guard_dial_math tests/unit/test_gui_thread_bridge tests/unit/test_gui_periodic_worker tests/unit/test_gui_backend_adapters tests/unit/benchmark_port_scan

install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/

test: $(TARGET)
	./$(TARGET)

debug: $(SRC)
	$(CC) $(CFLAGS) -DDEBUG -o $(TARGET)_debug $(SRC) $(GTK_FLAGS) $(CAIRO_FLAGS) $(LIBS)

check-deps:
	@echo "Verificando dependencias..."
	@pkg-config --exists gtk+-3.0 && echo "✓ GTK+3 encontrado" || echo "✗ GTK+3 no encontrado"
	@ldconfig -p | grep -q libcrypto && echo "✓ OpenSSL encontrado" || echo "✗ OpenSSL no encontrado"
	@ldconfig -p | grep -q libudev && echo "✓ libudev encontrado" || echo "✗ libudev no encontrado"

UNIT_CFLAGS = -Wall -Wextra -std=c99 -g -Iinclude

test-progress: tests/unit/test_progress.c include/progress.h
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_progress tests/unit/test_progress.c
	./tests/unit/test_progress

test-threadpool: tests/unit/test_threadpool.c src/threadpool.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_threadpool tests/unit/test_threadpool.c src/threadpool.c -lpthread
	./tests/unit/test_threadpool

test-port-classification: tests/unit/test_port_classification.c src/port_scanner.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_port_classification tests/unit/test_port_classification.c src/port_scanner.c src/threadpool.c -lpthread
	./tests/unit/test_port_classification

test-port-scan-range: tests/unit/test_port_scan_range.c src/port_scanner.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_port_scan_range tests/unit/test_port_scan_range.c src/port_scanner.c src/threadpool.c -lpthread
	./tests/unit/test_port_scan_range

test-hash-skip: tests/unit/test_hash_skip.c src/device_monitor.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_hash_skip tests/unit/test_hash_skip.c src/device_monitor.c src/threadpool.c -lcrypto -lpthread
	./tests/unit/test_hash_skip

test-process-shutdown: tests/unit/test_process_shutdown.c src/process_monitor.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_process_shutdown tests/unit/test_process_shutdown.c src/process_monitor.c -lpthread
	./tests/unit/test_process_shutdown

test-guard-dial-math: tests/unit/test_guard_dial_math.c src/gui/widgets/guard_dial.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_guard_dial_math tests/unit/test_guard_dial_math.c src/gui/widgets/guard_dial.c $(GTK_FLAGS)
	./tests/unit/test_guard_dial_math

test-gui-thread-bridge: tests/unit/test_gui_thread_bridge.c src/gui/gui_thread_bridge.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_gui_thread_bridge tests/unit/test_gui_thread_bridge.c src/gui/gui_thread_bridge.c `pkg-config --cflags --libs gobject-2.0`
	./tests/unit/test_gui_thread_bridge

test-gui-periodic-worker: tests/unit/test_gui_periodic_worker.c src/gui/gui_periodic_worker.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_gui_periodic_worker tests/unit/test_gui_periodic_worker.c src/gui/gui_periodic_worker.c -lpthread
	./tests/unit/test_gui_periodic_worker

test-gui-backend-adapters: tests/unit/test_gui_backend_adapters.c src/gui/gui_backend_adapters.c src/gui/panels/gui_logs_panel.c src/gui/panels/gui_dashboard_panel.c src/device_monitor.c src/threadpool.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_gui_backend_adapters tests/unit/test_gui_backend_adapters.c src/gui/gui_backend_adapters.c src/gui/panels/gui_logs_panel.c src/gui/panels/gui_dashboard_panel.c src/device_monitor.c src/threadpool.c `pkg-config --cflags --libs gtk+-3.0` -lcrypto -lpthread
	./tests/unit/test_gui_backend_adapters

test-unit: test-progress test-threadpool test-port-classification test-port-scan-range test-hash-skip test-process-shutdown test-guard-dial-math test-gui-thread-bridge test-gui-periodic-worker test-gui-backend-adapters

benchmark-port-scan: tests/unit/benchmark_port_scan.c src/port_scanner.c
	$(CC) $(UNIT_CFLAGS) -O2 -o tests/unit/benchmark_port_scan tests/unit/benchmark_port_scan.c src/port_scanner.c src/threadpool.c -lpthread
	./tests/unit/benchmark_port_scan

.PHONY: test-unit test-progress test-threadpool test-port-classification test-port-scan-range test-hash-skip test-process-shutdown test-guard-dial-math test-gui-thread-bridge test-gui-periodic-worker test-gui-backend-adapters benchmark-port-scan
