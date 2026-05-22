#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <linux/ethtool.h>

#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_bus.h>
#include <rte_bus_pci.h>
#include <rte_version.h>

#include <uuid/uuid.h>

#include "config.h"
#include "utils.h"
#include "dbg.h"
#include "fastrg.h"

/**
 * fastrg_calc_rss_queue_count - number of RSS worker queues.
 * Thread budget: 4 fixed (main + ctrl + wan_ctrl + lan_ctrl)
 *                + 2 * N data (wan_data + lan_data per RSS queue)
 * Northbound and related timer management run on the main lcore, no dedicated thread.
 * => N = max(1, (cpu_count - 4) / 2)
 */
static U16 fastrg_calc_rss_queue_count(unsigned int cpu_count)
{
    if (cpu_count <= 4)
        return 1;

    U16 data_queue_count = (U16)((cpu_count - 4) / 2);
    return data_queue_count > MAX_DATA_QUEUES ? MAX_DATA_QUEUES : data_queue_count;
}

void get_all_lcore_id(struct lcore_map *lcore, unsigned int cpu_count)
{
    memset(lcore, 0, sizeof(*lcore));

    /* Fixed threads: ctrl, wan_ctrl, lan_ctrl. Northbound and related timer management run on the main lcore. */
    lcore->ctrl_thread = rte_get_next_lcore(rte_lcore_id(), 1, 0);
    lcore->wan_ctrl_thread = rte_get_next_lcore(lcore->ctrl_thread, 1, 0);
    lcore->lan_ctrl_thread = rte_get_next_lcore(lcore->wan_ctrl_thread, 1, 0);
    lcore->timer_thread = rte_lcore_id();

    /* Dynamic data threads: wan_data[i] + lan_data[i] per RSS queue */
    U16 rss_count = fastrg_calc_rss_queue_count(cpu_count);
    lcore->num_data_queues = rss_count;

    unsigned int prev = lcore->lan_ctrl_thread;
    for(U16 i=0; i<rss_count; i++) {
        lcore->wan_data_threads[i] = rte_get_next_lcore(prev, 1, 0);
        lcore->lan_data_threads[i] = rte_get_next_lcore(lcore->wan_data_threads[i], 1, 0);
        prev = lcore->lan_data_threads[i];
    }
    lcore->northbound_thread = rte_get_next_lcore(prev, 1, 0);
}

/**
 * make_eal_args_string
 * 
 * @brief Make EAL args string from argc and argv
 * 
 * @param argc
 *      argument count
 * @param argv
 *      argument vector
 * 
 * @return
 *      EAL args string (caller free())
 */
char *make_eal_args_string(int argc, const char **argv)
{
    /* Validate inputs */
    if (argc <= 0 || argv == NULL)
        return NULL;

    size_t total_len = 0;
    for(int i=0; i<argc; i++)
        total_len += strlen(argv[i]);
    total_len += (argc - 1) + 1; // spaces + null terminator

    char *result = (char *)malloc(total_len);
    if (!result)
        return NULL;

    memset(result, 0, total_len);
    for(int i=0; i<argc; i++) {
        strcat(result, argv[i]);
        if (i < argc - 1)
            strcat(result, " ");
    }

    return result; // caller free()
}

STATUS parse_ip_range(const char *ip_range_str, U32 *ip_start, U32 *ip_end)
{
    if (!ip_range_str || !ip_start || !ip_end)
        return ERROR;

    char range_copy[128];
    strncpy(range_copy, ip_range_str, sizeof(range_copy) - 1);
    range_copy[sizeof(range_copy) - 1] = '\0';

    // Find delimiter (~ or -)
    char *delimiter = strchr(range_copy, '~');
    if (!delimiter)
        delimiter = strchr(range_copy, '-');

    if (!delimiter)
        return ERROR;

    // Split the string
    *delimiter = '\0';
    char *start_ip_str = range_copy;
    char *end_ip_str = delimiter + 1;

    // Parse start IP
    unsigned int s1, s2, s3, s4;
    if (sscanf(start_ip_str, "%u.%u.%u.%u", &s1, &s2, &s3, &s4) != 4) {
        return ERROR;
    }

    // Validate IP octets
    if (s1 > 255 || s2 > 255 || s3 > 255 || s4 > 255) {
        return ERROR;
    }

    // Parse end IP
    unsigned int e1, e2, e3, e4;
    if (sscanf(end_ip_str, "%u.%u.%u.%u", &e1, &e2, &e3, &e4) != 4) {
        return ERROR;
    }

    // Validate IP octets
    if (e1 > 255 || e2 > 255 || e3 > 255 || e4 > 255) {
        return ERROR;
    }

    // Convert to big-endian (network byte order)
    // For 192.168.1.1: we want 0x0101a8c0 (01 01 a8 c0 in memory)
    *ip_start = (U32)((s4 << 24) | (s3 << 16) | (s2 << 8) | s1);
    *ip_end = (U32)((e4 << 24) | (e3 << 16) | (e2 << 8) | e1);

    // Calculate pool length (number of IPs in the range)
    // Convert to host byte order for calculation
    U32 start_host = (s1 << 24) | (s2 << 16) | (s3 << 8) | s4;
    U32 end_host = (e1 << 24) | (e2 << 16) | (e3 << 8) | e4;

    if (end_host < start_host)
        return ERROR;

    return SUCCESS;
}

STATUS parse_ip(const char *ip_addr_str, U32 *ip_addr)
{
    if (!ip_addr_str || !ip_addr)
        return ERROR;

    unsigned int m1, m2, m3, m4;
    if (sscanf(ip_addr_str, "%u.%u.%u.%u", &m1, &m2, &m3, &m4) != 4)
        return ERROR;

    if (m1 > 255 || m2 > 255 || m3 > 255 || m4 > 255) {
        return ERROR;
    }

    *ip_addr = (U32)((m4 << 24) | (m3 << 16) | (m2 << 8) | m1);

    return SUCCESS;
}

STATUS fastrg_create_pthread(const char *thread_name, 
    void *(*thread_func)(void *), void *arg, unsigned int cpu_id)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;
    pthread_t thread_id;
    pthread_attr_t thread_attr;
    int ret;

    ret = pthread_attr_init(&thread_attr);
    if (ret != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Error: pthread_attr_init failed: %s\n", strerror(ret));
        return ERROR;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    ret = pthread_attr_setaffinity_np(&thread_attr, sizeof(cpu_set_t), &cpuset);
    if (ret != 0)
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "pthread_attr_setaffinity_np failed: %s\n", strerror(ret));

    ret = pthread_create(&thread_id, &thread_attr, thread_func, (void *)fastrg_ccb);
    if (ret != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "pthread_create failed: %s\n", strerror(ret));
        pthread_attr_destroy(&thread_attr);
        return ERROR;
    }
    ret = pthread_setname_np(thread_id, thread_name);
    if (ret != 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
            "pthread_setname_np with name %s failed: %s\n", 
            thread_name, strerror(ret));
    } else {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
            "Successfully named the thread '%s'\n", thread_name);
    }

    pthread_attr_destroy(&thread_attr);

    return SUCCESS;
}

STATUS fastrg_get_id(char node_id[])
{
    if (!node_id)
        return ERROR;

    // Try to read existing UUID from CONFIG_DIR_PATH/node_uuid
    char uuid_path[512];
    snprintf(uuid_path, sizeof(uuid_path), "%snode_uuid", CONFIG_DIR_PATH);

    char buf[128] = {0};
    int generated_new = 0;
    FILE *fp = fopen(uuid_path, "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp) != NULL) {
            // trim newline and whitespace
            size_t len = strlen(buf);
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ' || buf[len-1] == '\t')) {
                buf[len-1] = '\0';
                len--;
            }
        }
        fclose(fp);
    }

    if (buf[0] != '\0') {
        // use existing uuid from file
        rte_memcpy(node_id, buf, UUID_STR_LEN);
    } else {
        // Generate UUID for this node
        uuid_t uuid_bytes;
        uuid_generate(uuid_bytes);
        uuid_unparse(uuid_bytes, node_id);
        generated_new = 1;
    }

    // If we generated a new uuid (no file or empty file), write it back to the file
    if (generated_new) {
        FILE *wf = fopen(uuid_path, "w");
        if (wf) {
            fprintf(wf, "%s\n", node_id);
            fclose(wf);
        }
    }

    return SUCCESS;
}

STATUS parse_unix_sock_path(char *unix_sock, char **path, size_t *path_len)
{
    // e.g. unix_sock = "unix:///var/run/fastrg/fastrg.sock";
    if (!unix_sock)
        return ERROR;

    const char prefix[] = "unix://";
    size_t prefix_len = strlen(prefix);
    if (strncmp(unix_sock, prefix, prefix_len) != 0) {
        return ERROR;
    }
    // Shift the string to remove "unix://"
    *path = unix_sock + prefix_len;

    // find the last '/' to get directory path length
    char *last_slash = strrchr(*path, '/');
    if (last_slash == NULL)
        return ERROR;

    *path_len = last_slash - *path;

    return SUCCESS;
}

STATUS create_dir_if_not_exists(const char *dir_path)
{
    if (!dir_path || strlen(dir_path) == 0)
        return ERROR;

    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    len = strlen(tmp);

    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for(p=tmp+1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return ERROR;
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return ERROR;

    return SUCCESS;
}

STATUS parse_vlan_id(const char *vlan_str, U16 *vlan_id)
{
    if (!vlan_str || !vlan_id)
        return ERROR;

    char *endptr;
    long val = strtol(vlan_str, &endptr, 10);

    // check conversion errors
    if (endptr == vlan_str || *endptr != '\0')
        return ERROR;

    // VLAN ID valid range: 1-4094
    if (val < 1 || val > 4094)
        return ERROR;

    *vlan_id = (U16)val;

    return SUCCESS;
}

U16 fastrg_calc_queue_count(unsigned int cpu_count)
{
    /* queue 0 (control) + RSS worker queues */
    return fastrg_calc_rss_queue_count(cpu_count) + 1;
}

int get_drvinfo(U16 port_id, struct ethtool_drvinfo *drvinfo)
{
    struct rte_eth_dev_info dev_info;
    struct rte_dev_reg_info reg_info;
    int n;

    if (drvinfo == NULL)
        return -EINVAL;

    RTE_ETH_VALID_PORTID_OR_ERR_RET(port_id, -ENODEV);

    int ret = rte_eth_dev_fw_version_get(port_id, drvinfo->fw_version,
                  sizeof(drvinfo->fw_version));
    if (ret < 0) {
        printf("firmware version get error: (%s)\n", strerror(-ret));
    } else if (ret > 0) {
        printf("Insufficient fw version buffer size, "
               "the minimum size should be %d\n", ret);
    }

    memset(&dev_info, 0, sizeof(dev_info));
    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        printf("get port %d info failed: %s\n", port_id, strerror(-ret));
        return ret;
    }

    strlcpy(drvinfo->driver, dev_info.driver_name, sizeof(drvinfo->driver));
    strlcpy(drvinfo->version, rte_version(), sizeof(drvinfo->version));
    strlcpy(drvinfo->bus_info, rte_dev_name(dev_info.device), sizeof(drvinfo->bus_info));

    memset(&reg_info, 0, sizeof(reg_info));
    drvinfo->regdump_len = rte_eth_dev_get_reg_info(port_id, &reg_info) ? reg_info.length : 0;

    n = rte_eth_dev_get_eeprom_length(port_id);
    drvinfo->eedump_len = n > 0 ? n : 0;

    drvinfo->n_stats = sizeof(struct rte_eth_stats) / sizeof(uint64_t);
    drvinfo->testinfo_len = 0;

    return 0;
}
