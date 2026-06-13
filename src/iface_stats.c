#include "../include/iface_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

static int read_text_file(const char *path, char *buffer, size_t buffer_size)
{
        FILE *file = fopen(path, "r");

        if (file == NULL) {
            return -1;
        }

        if (fgets(buffer, buffer_size, file) == NULL) {
            fclose(file);
            return -1;
        }

        fclose(file);

        /*
        * Remove trailing newline
        */
       buffer[strcspn(buffer, "\n")] = '\0';
        
        return 0;   
}

static int read_uint64_file(const char *path, uint64_t *value)
{
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        return -1;
    }

    if (fscanf(file, "%" SCNu64, value) != 1) {
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

static int build_sysfs_path(
    char *path,
    size_t path_size,
    const char *iface_name,
    const char *file_name
)
{
    int written = snprintf(
        path,
        path_size,
        "/sys/class/net/%s/%s",
        iface_name,
        file_name
    );

    if (written < 0 || (size_t)written >= path_size) {
        return -1;
    }

    return 0;
}

static int build_stat_path(
    char *path,
    size_t path_size,
    const char *iface_name,
    const char *stat_name
)
{
    int written = snprintf(
        path,
        path_size,
        "/sys/class/net/%s/statistics/%s",
        iface_name,
        stat_name
    );

    if (written < 0 || (size_t)written >= path_size) {
        return -1;
    }

    return 0;
}

static int read_int_file(const char *path, int *value)
{
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        return -1;
    }

    if (fscanf(file, "%d", value) != 1) {
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

int read_iface_stats(const char *iface_name, iface_stats_t *stats)
{
    char path[256];

    if (iface_name == NULL || stats == NULL) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));

    snprintf(stats->name, IFACE_NAME_MAX_LEN, "%s", iface_name);

    /*
     * Basic interface information.
     */
    if (build_sysfs_path(path, sizeof(path), iface_name, "address") == 0) {
        if (read_text_file(path, stats->mac, sizeof(stats->mac)) != 0) {
            snprintf(stats->mac, sizeof(stats->mac), "unknown");
        }
    }

    if (build_sysfs_path(path, sizeof(path), iface_name, "operstate") == 0) {
        if (read_text_file(path, stats->operstate, sizeof(stats->operstate)) != 0) {
            snprintf(stats->operstate, sizeof(stats->operstate), "unknown");
        }
    }

    if (build_sysfs_path(path, sizeof(path), iface_name, "duplex") == 0) {
        if (read_text_file(path, stats->duplex, sizeof(stats->duplex)) != 0) {
            snprintf(stats->duplex, sizeof(stats->duplex), "unknown");
        }
    }

    if (build_sysfs_path(path, sizeof(path), iface_name, "speed") == 0) {
        if (read_int_file(path, &stats->speed_mbps) != 0) {
            stats->speed_mbps = -1;
        }
    }

    /*
     * RX counters.
     */
    if (build_stat_path(path, sizeof(path), iface_name, "rx_bytes") == 0) {
        read_uint64_file(path, &stats->rx_bytes);
    }

    if (build_stat_path(path, sizeof(path), iface_name, "rx_packets") == 0) {
        read_uint64_file(path, &stats->rx_packets);
    }

    if (build_stat_path(path, sizeof(path), iface_name, "rx_errors") == 0) {
        read_uint64_file(path, &stats->rx_errors);
    }

    if (build_stat_path(path, sizeof(path), iface_name, "rx_dropped") == 0) {
        read_uint64_file(path, &stats->rx_dropped);
    }

    /*
     * TX counters.
     */
    if (build_stat_path(path, sizeof(path), iface_name, "tx_bytes") == 0) {
        read_uint64_file(path, &stats->tx_bytes);
    }

    if (build_stat_path(path, sizeof(path), iface_name, "tx_packets") == 0) {
        read_uint64_file(path, &stats->tx_packets);
    }

    if (build_stat_path(path, sizeof(path), iface_name, "tx_errors") == 0) {
        read_uint64_file(path, &stats->tx_errors);
    }

    if (build_stat_path(path, sizeof(path), iface_name, "tx_dropped") == 0) {
        read_uint64_file(path, &stats->tx_dropped);
    }

    return 0;
}

void print_iface_stats(const iface_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    printf("\n");
    printf("=== Interface information ===\n");
    printf("Interface: %s\n", stats->name);
    printf("MAC:       %s\n", stats->mac);
    printf("State:     %s\n", stats->operstate);

    if (stats->speed_mbps >= 0) {
        printf("Speed:     %d Mb/s\n", stats->speed_mbps);
    } else {
        printf("Speed:     unknown\n");
    }

    printf("Duplex:    %s\n", stats->duplex);

    printf("\n");
    printf("=== Interface RX stats ===\n");
    printf("RX bytes:   %" PRIu64 "\n", stats->rx_bytes);
    printf("RX packets: %" PRIu64 "\n", stats->rx_packets);
    printf("RX errors:  %" PRIu64 "\n", stats->rx_errors);
    printf("RX dropped: %" PRIu64 "\n", stats->rx_dropped);

    printf("\n");
    printf("=== Interface TX stats ===\n");
    printf("TX bytes:   %" PRIu64 "\n", stats->tx_bytes);
    printf("TX packets: %" PRIu64 "\n", stats->tx_packets);
    printf("TX errors:  %" PRIu64 "\n", stats->tx_errors);
    printf("TX dropped: %" PRIu64 "\n", stats->tx_dropped);
}

static int file_exists(const char *filename)
{
    FILE *file = fopen(filename, "r");

    if (file == NULL) {
        return 0;
    }

    fclose(file);
    return 1;
}

int append_iface_stats_csv(const char *filename, const iface_stats_t *stats)
{
    FILE *file;
    time_t now;
    int write_header;

    if (filename == NULL || stats == NULL) {
        return -1;
    }

    write_header = !file_exists(filename);

    file = fopen(filename, "a");

    if (file == NULL) {
        return -1;
    }

    if (write_header) {
        fprintf(
            file,
            "timestamp,interface,mac,state,speed_mbps,duplex,"
            "rx_bytes,rx_packets,rx_errors,rx_dropped,"
            "tx_bytes,tx_packets,tx_errors,tx_dropped\n"
        );
    }

    now = time(NULL);

    fprintf(
        file,
        "%ld,%s,%s,%s,%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
        now,
        stats->name,
        stats->mac,
        stats->operstate,
        stats->speed_mbps,
        stats->duplex,
        stats->rx_bytes,
        stats->rx_packets,
        stats->rx_errors,
        stats->rx_dropped,
        stats->tx_bytes,
        stats->tx_packets,
        stats->tx_errors,
        stats->tx_dropped
    );

    fclose(file);

    return 0;
}