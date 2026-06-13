#ifndef IFACE_STATS_H
#define IFACE_STATS_H

#include <stdint.h>

#define IFACE_NAME_MAX_LEN 64
#define IFACE_TEXT_MAX_LEN 128

typedef struct {
    char name[IFACE_NAME_MAX_LEN];

    char mac[IFACE_NAME_MAX_LEN];
    char operstate[IFACE_TEXT_MAX_LEN];
    char duplex[IFACE_TEXT_MAX_LEN];

    int speed_mbps;

    uint64_t rx_bytes;
    uint64_t rx_packets;
    uint64_t rx_errors;
    uint64_t rx_dropped;
    
    uint64_t tx_bytes;
    uint64_t tx_packets;
    uint64_t tx_errors;
    uint64_t tx_dropped;
} iface_stats_t;

int read_iface_stats(const char *iface_name, iface_stats_t *stats);
void print_iface_stats(const iface_stats_t *stats);
int append_iface_stats_csv(const char *filename, const iface_stats_t *stats);

#endif // IFACE_STATS_H