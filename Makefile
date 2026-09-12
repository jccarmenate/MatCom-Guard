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
		src/gui/gui_thread_bridge.c \
		src/gui/gui_shell.c \
		src/gui/gui_demo_scan.c \
		src/gui/gui_main.c \
		src/gui/window/gui_logging.c \
		src/gui/window/gui_stats.c \
		src/gui/window/gui_status.c \
		src/gui/window/gui_usb_panel.c \
		src/gui/window/gui_process_panel.c \
		src/gui/window/gui_ports_panel.c \
		src/gui/window/gui_config_dialog.c \
		src/gui/integration/gui_system_coordinator.c \
		src/gui/integration/gui_backend_adapters.c \
		src/gui/integration/gui_process_integration.c \
		src/gui/integration/gui_ports_integration.c \
		src/gui/integration/gui_usb_integration.c
		
.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(GTK_FLAGS) $(CAIRO_FLAGS) $(LIBS)

clean:
	rm -f $(TARGET) *.o
	rm -f tests/unit/test_progress tests/unit/test_threadpool tests/unit/test_port_classification tests/unit/test_port_scan_range tests/unit/test_hash_skip tests/unit/test_process_shutdown tests/unit/benchmark_port_scan

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

test-unit: test-progress test-threadpool test-port-classification test-port-scan-range test-hash-skip test-process-shutdown

benchmark-port-scan: tests/unit/benchmark_port_scan.c src/port_scanner.c
	$(CC) $(UNIT_CFLAGS) -O2 -o tests/unit/benchmark_port_scan tests/unit/benchmark_port_scan.c src/port_scanner.c src/threadpool.c -lpthread
	./tests/unit/benchmark_port_scan

.PHONY: test-unit test-progress test-threadpool test-port-classification test-port-scan-range test-hash-skip test-process-shutdown benchmark-port-scan
