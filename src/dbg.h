/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DBG.H

  Designed by THE on JUN 11, 2019
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DBG_H_
#define _DBG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>

#include <common.h>

#define LOGDBG     1U
#define LOGINFO    2U
#define LOGWARN    3U
#define LOGERR     4U
#define LOGUNKNOWN 0U

char *PPP_state2str(U16 state);
char *PPP_event2str(U16 event);
char *DHCP_state2str(U16 state);

/*
 * Pass only the source filename (basename) to LOGGER instead of the full
 * path provided by __FILE__. This strips directory prefixes like "src/"
 * so callers see e.g. "dhcpd.c" rather than "src/dhcp/dhcpd.c".
 */
#define BASENAME_FILE(path) \
    ((const char *)( \
        (strrchr((path), '/') ? strrchr((path), '/') + 1 : \
         (strrchr((path), '\\') ? strrchr((path), '\\') + 1 : (path)))  \
    ))

/* log level, logfile fp, log msg */
#ifdef UNIT_TEST
#define FastRG_LOG(lvl, fp, ccb, ccb2str, ...) \
    LOGGER(LOGUNKNOWN, BASENAME_FILE(__FILE__), __LINE__, fp, ccb, ccb2str, __VA_ARGS__)
#elif NB_TEST
#define FastRG_LOG(lvl, fp, ccb, ccb2str, ...)
#else
#define FastRG_LOG(lvl, fp, ccb, ccb2str, ...) \
    LOGGER(LOG ## lvl, (__FILE__), __LINE__, fp, ccb, ccb2str, __VA_ARGS__)
#endif

void LOGGER(U8 level, const char *filename, int line_num, FILE *log_fp, void *ccb, 
    void (*ccb2str)(void *, char *), const char *fmt,...);
char *loglvl2str(U8 level);
U8 logstr2lvl(const char *log_str);
void PPPLOGMSG(void *ccb, char *buf);
void DHCPLOGMSG(void *ccb, char *buf);
void dbg_init(void *ccb);

#ifdef __cplusplus
}
#endif

#endif
