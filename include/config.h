#ifndef CONFIG_H
#define CONFIG_H

/*
 * Default network configuration.
 */
#define ETHERBENCH_DEFAULT_FPGA_CTRL_PORT       55555
#define ETHERBENCH_DEFAULT_FPGA_LOOPBACK_PORT   1234
#define ETHERBENCH_DEFAULT_RX_PORT              9999
#define ETHERBENCH_DEFAULT_TIMEOUT_MS           3000

/*
 * FPGA / MDIO timing.
 */
#define ETHERBENCH_DEFAULT_MDIO_DELAY_US  200000

/*
 * Default log files.
 */
#define ETHERBENCH_IFACE_LOG_FILE         "interface_log.csv"
#define ETHERBENCH_NET_LOG_FILE           "net_log.csv"
#define ETHERBENCH_FPGA_LOG_FILE          "fpga_log.csv"
#define ETHERBENCH_FPGA_RTT_LOG_FILE      "fpga_rtt_logs.csv"
#define ETHERBENCH_FPGA_LOOPBACK_LOAD_LOG_FILE "fpga_loopback_load_logs.csv"
#define ETHERBENCH_FPGA_LOOPBACK_LOSS_LOG_FILE "fpga_loopback_loss_logs.csv"

/*
 * KSZ9031 1GbE default MDIO configuration sequence.
 *
 * This is used to configure the PHY through the FPGA MDIO block.
 */
#define ETHERBENCH_KSZ9031_1GBE_OPS_INIT { \
    "w:0x0D:0x0002", \
    "w:0x0E:0x0000", \
    "w:0x0D:0x4002", \
    "w:0x0E:0x0008", \
    \
    "w:0x0D:0x0002", \
    "w:0x0E:0x0004", \
    "w:0x0D:0x4002", \
    "w:0x0E:0x00FC", \
    \
    "w:0x0D:0x0002", \
    "w:0x0E:0x0005", \
    "w:0x0D:0x4002", \
    "w:0x0E:0x7FFF", \
    \
    "w:0x0D:0x0002", \
    "w:0x0E:0x0006", \
    "w:0x0D:0x4002", \
    "w:0x0E:0xCCCC", \
    \
    "w:0x0D:0x0002", \
    "w:0x0E:0x0008", \
    "w:0x0D:0x4002", \
    "w:0x0E:0x004C" \
}

#define ETHERBENCH_KSZ9031_1GBE_OPS_COUNT 20

#endif