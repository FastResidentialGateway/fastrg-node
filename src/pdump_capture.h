/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  PDUMP_CAPTURE.H

  CLI-driven, per-subscriber packet capture (exec pdump start/stop).

  Capture is implemented with DPDK rte_eth RX/TX callbacks (the same mechanism
  rte_pdump uses internally) so the data-plane fast path (dp.c) is never
  touched: when no capture session is active no callback is registered and the
  forwarding path pays exactly zero cost. While capturing, a callback derives
  the subscriber id from the frame's outer VLAN tag (present on both WAN and LAN
  frames, see parse_l2_hdr in dp.c), consults a per-(port, subscriber) on/off
  matrix and an optional per-entry tcpdump-style BPF filter, and deep-copies
  matching frames into a ring drained by a writer thread that produces a single
  merged classic-pcap file.
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _PDUMP_CAPTURE_H_
#define _PDUMP_CAPTURE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <common.h>

#include "fastrg.h"

/* Capture direction selector. Values match PdumpRequest.dir in the proto. */
enum pdump_dir {
    PDUMP_DIR_ALL = 0, /**< both WAN and LAN ports          */
    PDUMP_DIR_WAN = 1, /**< WAN port only                   */
    PDUMP_DIR_LAN = 2, /**< LAN port only                   */
};

/**
 * @fn fastrg_pdump_capture_init
 * @brief Allocate the per-(port, subscriber) capture state. Call once at
 *        startup after user_count/max_user_count are known.
 * @return SUCCESS on success, ERROR on allocation failure.
 */
STATUS fastrg_pdump_capture_init(FastRG_t *fastrg_ccb);

/**
 * @fn fastrg_pdump_start
 * @brief Start capturing for a direction and subscriber, optionally filtered.
 * @param dir         one of enum pdump_dir
 * @param subscriber  1-based subscriber id, or 0 for all subscribers
 * @param filter      tcpdump-style BPF expression (NULL/empty = capture all).
 *                    The expression matches the frame as seen on the wire
 *                    (outer VLAN on both sides, PPPoE on WAN) — encapsulation
 *                    aware expressions are required to match inner protocols.
 * @param size_limit_mb  max pcap file size in MB; 0 = default. Always capped at
 *                    2048 (2GB). When the file reaches the limit the capture
 *                    stops writing (run stop to release resources).
 * @param out_file    filled with the active pcap file path on success
 * @param err         filled with a short reason on failure
 * @return SUCCESS on success, ERROR on failure.
 */
STATUS fastrg_pdump_start(FastRG_t *fastrg_ccb, int dir, U16 subscriber,
    const char *filter, U32 size_limit_mb, char *out_file, U32 out_len,
    char *err, U32 err_len);

/**
 * @fn fastrg_pdump_stop
 * @brief Stop capturing for a direction and subscriber. Other active
 *        (direction, subscriber) captures keep running. When the last one is
 *        stopped the writer thread, ring, mempool and pcap file are released.
 * @param subscriber  1-based subscriber id, or 0 for all subscribers
 * @return SUCCESS on success, ERROR on failure.
 */
STATUS fastrg_pdump_stop(FastRG_t *fastrg_ccb, int dir, U16 subscriber,
    char *err, U32 err_len);

/**
 * @fn fastrg_pdump_capture_cleanup
 * @brief Force-stop any active capture and free all state. Call at shutdown.
 */
void fastrg_pdump_capture_cleanup(FastRG_t *fastrg_ccb);

#ifdef __cplusplus
}
#endif

#endif /* _PDUMP_CAPTURE_H_ */
