#ifndef CONFIG_H
#define CONFIG_H

/*
 * Default network configuration.
 */
#define ETHERBENCH_DEFAULT_FPGA_PORT      55555
#define ETHERBENCH_DEFAULT_RX_PORT        9999
#define ETHERBENCH_DEFAULT_TIMEOUT_MS     3000

/*
 * FPGA / MDIO timing.
 */
#define ETHERBENCH_DEFAULT_MDIO_DELAY_US  500000

/*
 * Default log files.
 */
#define ETHERBENCH_IFACE_LOG_FILE         "interface_log.csv"
#define ETHERBENCH_NET_LOG_FILE           "net_log.csv"
#define ETHERBENCH_FPGA_LOG_FILE          "fpga_log.csv"

#endif