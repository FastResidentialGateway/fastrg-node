#ifndef _UTILS_H_
#define _UTILS_H_

#include <common.h>

#include <rte_malloc.h>
#include <rte_ring.h>
#include <rte_mbuf.h>

#include <linux/ethtool.h>

#include "protocol.h"

#define RING_BURST_SIZE 64

extern rte_atomic16_t stop_flag;

typedef enum {
    EV_NORTHBOUND_PPPoE,
    EV_NORTHBOUND_DHCP,
    EV_DP_PPPoE,
    EV_DP_DNS,
    EV_DP_DHCP,
    EV_LINK,
} fastrg_event_type_t;

typedef struct {
    U8 cmd;
    U16 ccb_id;
} fastrg_event_northbound_msg_t;

/**
 * @brief msg between IF driver and daemon
 */
typedef struct {
    fastrg_event_type_t type;
    U8                  refp[ETH_JUMBO];
    int                 len;
    U16                 ccb_id;     /* subscriber CCB ID (used by EV_DP_DNS / EV_DP_DHCP) */
    U8                  port_id;    /* source port: LAN_PORT(0) or WAN_PORT(1) */
    struct rte_mbuf     *mbuf;      /* mbuf pointer for EV_DP_* zero-copy path */
} tFastRG_MBX;

/* only execution when condition is true */
#define FastRG_ASSERT(cond, op, ret) do { \
    if (unlikely(!(cond))) { \
        (ret) = (op); \
    } \
} while(0)

/* Memory operation wrappers */
#ifdef UNIT_TEST
#define fastrg_malloc(type, size, aligned) (type *)malloc(size)
#define fastrg_calloc(type, num, size, aligned) (type *)calloc(num, size)
#define fastrg_realloc(type, ptr, size, aligned) (type *)realloc(ptr, size)
#define fastrg_mfree(ptr) free(ptr)
#else
#define fastrg_malloc(type, size, aligned) (type *)_fastrg_malloc(size, aligned)
#define fastrg_calloc(type, num, size, aligned) (type *)_fastrg_calloc((num)*(size), aligned)
#define fastrg_realloc(type, ptr, size, aligned) (type *)_fastrg_realloc(ptr, size, aligned)
#define fastrg_mfree(ptr) _fastrg_mfree(ptr)

static inline void *_fastrg_malloc(size_t size, unsigned int aligned) {
    if (unlikely(size == 0)) {
        return NULL;
    }
    return rte_malloc(NULL, size, aligned);
}

static inline void * _fastrg_calloc(size_t size, unsigned int aligned) {
    if (unlikely(size == 0)) {
        return NULL;
    }

    return rte_zmalloc(NULL, size, aligned);
}

static inline void * _fastrg_realloc(void *ptr, size_t size, unsigned int aligned) {
    if (unlikely(size == 0)) {
        return NULL;
    }
    return rte_realloc(ptr, size, aligned);
}

static inline void _fastrg_mfree(void *ptr) {
    if (unlikely(ptr == NULL)) {
        return;
    }
    rte_free(ptr);
}
#endif

/* rte_timer wrappers */
#ifdef UNIT_TEST
#define fastrg_get_cycles_in_sec simulate_get_timer_hz
static inline uint64_t simulate_get_timer_hz(void)
{
    return 2000000000ULL; // 2 GHz for unit tests
}
#define fastrg_get_cur_cycles __rdtsc
#else
#define fastrg_get_cycles_in_sec rte_get_timer_hz
#define fastrg_get_cur_cycles rte_rdtsc
#endif

/**
 * @fn fastrg_ring_enqueue
 * 
 * @brief fastrg lockless ring enqueue, it will try to enqueue all mails
 * @param ring
 *      ring pointer
 * @param mails
 *      mail array
 * @param enqueue_num
 *      mail amount
 * @return
 *      void
 */ 
static inline void fastrg_ring_enqueue(struct rte_ring *ring, void **mails, unsigned int enqueue_num)
{
    unsigned int burst_size = 0;
    unsigned int rest_num = enqueue_num;

    for(;;) {
        int rest_mails_index = enqueue_num - rest_num;
        burst_size = rte_ring_enqueue_burst(ring, &mails[rest_mails_index], rest_num, NULL);
        rest_num -= burst_size;
        if (likely(rest_num == 0))
            break;
    }
}

/**
 * @fn fastrg_ring_dequeue
 * 
 * @brief fastrg lockless ring dequeue, it will return once there is a mail
 * @param ring
 *      ring pointer
 * @param mails
 *      mail array
 * @return
 *      mail amount
 */ 
static inline int fastrg_ring_dequeue(struct rte_ring *ring, void **mail)
{
    U16 burst_size;

    while(likely(rte_atomic16_read(&stop_flag) == 0)) {
        burst_size = rte_ring_dequeue_burst(ring, mail, RING_BURST_SIZE, NULL);
        if (likely(burst_size == 0))
            continue;
        break;
    }
    return burst_size;
}

#define MAX_DATA_QUEUES 16

struct lcore_map {
    U8 ctrl_thread;
    U8 wan_ctrl_thread;     /* WAN queue 0: PPPoE control + ICMP decap */
    U8 lan_ctrl_thread;     /* LAN queue 0: ARP, ICMP, PPPoE passthrough */
    U8 timer_thread;
    U8 northbound_thread;
    U8 wan_data_threads[MAX_DATA_QUEUES];  /* WAN queues 1..N: PPPoE session TCP/UDP */
    U8 lan_data_threads[MAX_DATA_QUEUES];  /* LAN queues 1..N: TCP/UDP encap */
    U16 num_data_queues;    /* number of RSS data queues (= rss_queue_count) */
};

void get_all_lcore_id(struct lcore_map *lcore, unsigned int cpu_count);
char *make_eal_args_string(int argc, const char **argv);

/**
 * @fn parse_ip_range
 * 
 * @brief Parse IP range string to start IP, end IP and pool length
 *        Input format: "192.168.1.1~192.168.1.150" or "192.168.1.1-192.168.1.150"
 *        IPs are returned in big-endian format (network byte order)
 * 
 * @param ip_range_str
 *      IP range string (e.g., "192.168.1.1~192.168.1.150")
 * @param ip_start
 *      Pointer to store start IP in big-endian (e.g., 0x0101a8c0 for 192.168.1.1)
 * @param ip_end
 *      Pointer to store end IP in big-endian (e.g., 0x9601a8c0 for 192.168.1.150)
 * 
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS parse_ip_range(const char *ip_range_str, U32 *ip_start, U32 *ip_end);

/**
 * @fn parse_ip
 *
 * @brief Parse IP string to U32 big-endian format
 *        Input format: "192.168.1.1"
 * @param ip_str
 *      IP address string (e.g., "192.168.1.1")
 * @param ip
 *      Pointer to store IP address in big-endian (e.g., 0x0101a8c0)
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS parse_ip(const char *ip_str, U32 *ip);

/**
 * @fn is_ip_in_range
 * 
 * @brief Check if an IP is within the specified subnet
 * @param ip
 *      IP address to check (in U32 format)
 * @param gateway_ip
 *      Gateway IP address (in U32 format)
 * @param subnet_mask
 *      Subnet mask (in U32 format)
 * @return
 *      TRUE if IP is in range, FALSE otherwise
*/
static inline BOOL is_ip_in_range(U32 ip, U32 gateway_ip, U32 subnet_mask)
{
    return ((ip & subnet_mask) == (gateway_ip & subnet_mask));
}

STATUS fastrg_create_pthread(const char *thread_name, 
    void *(*thread_func)(void *), void *arg, unsigned int cpu_id);

/**
 * @fn fastrg_get_id
 * 
 * @brief Get previously stored unique node ID (UUID)
 * @param node_id
 *     Buffer to store the node ID (UUID), must be at least 37 bytes
 * @return
 *     SUCCESS on success, ERROR on failure
 */
STATUS fastrg_get_id(char node_id[]);

/**
 * @fn parse_unix_sock_path
 * 
 * @brief Parse unix socket path to get directory path and its length
 * @param unix_sock
 *      Full unix socket path (e.g., "unix:///var/run/fastrg/fastrg.sock")
 * @param path
 *      Pointer to the begining position of store the directory path 
 *      (e.g., "/var/run/fastrg/")
 * @param path_len
 *      Pointer to store the length of the directory path
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS parse_unix_sock_path(char *unix_sock, char **path, size_t *path_len);

/**
 * @fn create_dir_if_not_exists
 * 
 * @brief Create directory if it does not exist
 * @param dir_path
 *      Directory path to create
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS create_dir_if_not_exists(const char *dir_path);

/**
 * @fn parse_vlan_id
 * 
 * @brief Parse VLAN ID from string
 * @param vlan_str
 *      VLAN ID string (e.g., "100")
 * @param vlan_id
 *      Pointer to store parsed VLAN ID
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS parse_vlan_id(const char *vlan_str, U16 *vlan_id);

/**
 * @fn posix_sleep_ms
 * 
 * @brief Sleep for specified milliseconds
 * @param ms
 *      Milliseconds to sleep
 * @return
 *      void
 */
static inline void posix_sleep_ms(unsigned int ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

/**
 * @fn fastrg_calc_queue_count - Compute total RX/TX queue count from CPU count.
 *
 * @brief Queue layout:
 *   Queue 0          : PPPoE control plane (PPPoED 0x8863 / PPPoES without
 *                       inner 5-tuple 0x8864).  Never used by RSS.
 *   Queue 1 .. N     : RSS worker queues (5-tuple hash, PPPoE inner-header
 *                       aware).
 *
 * Thread budget: 5 fixed (main + ctrl + wan_ctrl + lan_ctrl + timer)
 *                + 2 * N data (wan_data + lan_data per RSS queue)
 * => N = max(1, (cpu_count - 5) / 2)
 *
 * @param cpu_count
 *      Number of available CPU cores (>= 1).
 * @return
 *      Total queue count (RSS queues + 1).
 */
U16 fastrg_calc_queue_count(unsigned int cpu_count);

/**
 * @fn get_drvinfo
 * 
 * @brief Get port driver information
 * 
 * @param port_id
 *      Port ID
 * @param drvinfo
 *      Driver information in ethtool format
 * 
 * @return
 *      errno
 */
int get_drvinfo(U16 port_id, struct ethtool_drvinfo *drvinfo);

#endif
