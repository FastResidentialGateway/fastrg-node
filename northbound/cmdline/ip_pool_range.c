#include <string.h>

#include <common.h>

STATUS cli_parse_ip_pool_range(const char *range, char *start, size_t start_sz, char *end, size_t end_sz)
{
    const char *delimiter;
    size_t start_len;
    size_t end_len;

    if (range == NULL || start == NULL || start_sz == 0 || end == NULL || end_sz == 0)
        return ERROR;

    delimiter = strchr(range, '~');
    if (delimiter == NULL)
        delimiter = strchr(range, '-');
    if (delimiter == NULL)
        return ERROR;

    start_len = (size_t)(delimiter - range);
    end_len = strlen(delimiter + 1);
    if (start_len >= start_sz || end_len >= end_sz)
        return ERROR;

    memcpy(start, range, start_len);
    start[start_len] = '\0';
    memcpy(end, delimiter + 1, end_len + 1);

    return SUCCESS;
}
