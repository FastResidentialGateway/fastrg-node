/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  METRICS.H

  Prometheus /metrics exposition for a FastRG node. Implemented as a lighthttp
  handler (transport lives in lighthttp.c). Exposes the fastrg_node_* counters
  the controller used to re-publish (Grafana-compatible) plus node liveness /
  NIC link metrics that only make sense scraping the node directly.
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _METRICS_H_
#define _METRICS_H_

#include "lighthttp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @fn metrics_build
 *
 * @brief lighthttp handler that builds the Prometheus text exposition for this
 *        node into the response buffer
 * @param out
 *      Buffer the exposition text is appended to
 * @param content_type
 *      Set to the Prometheus text-format content type
 * @param ctx
 *      FastRG control block (FastRG_t *)
 * @return
 *      HTTP status code (200)
 */
int metrics_build(lighthttp_buf_t *out, const char **content_type, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* _METRICS_H_ */
