CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude

SRC = 	src/main.c \
		src/iface_stats.c \
		src/net_stats.c \
		src/fpga_stats.c \
		src/fpga_mdio.c \
		src/fpga_config.c \
		src/fpga_setup.c
OUT = etherbench

all:
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

clean:
	rm -f $(OUT)

clear-logs:
	rm -f interface_log.csv
	rm -f net_log.csv
	rm -f fpga_log.csv