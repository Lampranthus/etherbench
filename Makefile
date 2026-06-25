CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude
LDLIBS = -lm
FPGA_IP ?= 192.168.1.12
FPGA_PORT ?= 55555

SRC = 	src/main.c \
		src/iface_stats.c \
		src/net_stats.c \
		src/fpga_stats.c \
		src/fpga_mdio.c \
		src/fpga_config.c \
		src/fpga_setup.c \
		src/fpga_ctrl.c \
		src/fpga_rtt.c \
		src/fpga_loopback_mode.c \
		src/fpga_loopback_load.c \
		src/fpga_loopback_capture.c \
		src/fpga_tx_test.c \
		src/utils.c
OUT = etherbench

.PHONY: all clean clear-logs eth0 up down

all:
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDLIBS)

clean:
	rm -f $(OUT)

clear-logs:
	rm -f interface_log.csv
	rm -f net_log.csv
	rm -f fpga_log.csv
	rm -f fpga_rtt_logs.csv
	rm -f fpga_loopback_load_logs.csv
	rm -f fpga_loopback_loss_logs.csv
	rm -f fpga_tx_test_logs.csv

eth0:
	@:

down:
	@if echo "$(MAKECMDGOALS)" | grep -qw eth0; then \
		ip link set eth0 down; \
	else \
		echo "Usage: make eth0 down"; \
		exit 1; \
	fi

up: all
	@if echo "$(MAKECMDGOALS)" | grep -qw eth0; then \
		ip link set eth0 up; \
		./$(OUT) fpga-arp eth0 $(FPGA_IP) $(FPGA_PORT); \
	else \
		echo "Usage: make eth0 up"; \
		exit 1; \
	fi
