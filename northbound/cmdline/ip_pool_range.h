#ifndef _IP_POOL_RANGE_H_
#define _IP_POOL_RANGE_H_

#include <stddef.h>

#include <common.h>

/**
 * @fn cli_parse_ip_pool_range
 * 
 * @brief Split an IP pool range into start and end strings.
 * 
 * @param range 
 *      IP pool range separated by '~' or '-'.
 * @param start
 *      Destination buffer for the start string.
 * @param start_sz
 *      Size of the start destination buffer.
 * @param end
 *      Destination buffer for the end string.
 * @param end_sz
 *      Size of the end destination buffer.
 * @return
 *      SUCCESS when both strings fit, otherwise ERROR.
 */
STATUS cli_parse_ip_pool_range(const char *range, char *start, size_t start_sz, char *end, size_t end_sz);

#endif
