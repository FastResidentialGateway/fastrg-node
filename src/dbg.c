/**************************************************************************
 * DBG.C
 *
 * Debug methods for ppp detection
 *
 * Created by THE on JUN 11,'19
 **************************************************************************/

#include    <common.h>

#include 	<rte_byteorder.h>

#include	"pppd/pppd.h"
#include 	"pppd/fsm.h"
#include 	"dhcpd/dhcp_fsm.h"
#include    "dbg.h"

#define 	DBG_FastRG_MSG_LEN 256
#define 	LOGGER_BUF_LEN 1024

static FastRG_t *fastrg_ccb;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    U8 level;
    const char *str;
} log_level_map_t;

static const log_level_map_t log_level_map[] = {
    { LOGDBG,     "DBG" },
    { LOGINFO,    "INFO" },
    { LOGWARN,    "WARN" },
    { LOGERR,     "ERR" },
    { LOGUNKNOWN, "UNKNOWN" }
};

#define LOG_LEVEL_MAP_SIZE (sizeof(log_level_map) / sizeof(log_level_map[0]))

char *loglvl2str(U8 level)
{
    for(size_t i=0; i<LOG_LEVEL_MAP_SIZE-1; i++) {
        if (log_level_map[i].level == level)
            return (char *)log_level_map[i].str;
    }
    return "UNKNOWN";
}

U8 logstr2lvl(const char *log_str)
{
    if (!log_str)
        return LOGUNKNOWN;

    for(size_t i=0; i<LOG_LEVEL_MAP_SIZE-1; i++) {
        if (strcmp(log_str, log_level_map[i].str) == 0)
            return log_level_map[i].level;
    }
    return LOGUNKNOWN;
}

/*-------------------------------------------------------------------
 * PPP_state2str
 *
 * input : state
 * return: string of corresponding state value
 *------------------------------------------------------------------*/
char *PPP_state2str(U16 state)
{
    static struct {
        PPP_STATE	state;
        char		str[20];
    } ppp_state_desc_tbl[] = {
    	{ S_INIT,  			"INIT" },
    	{ S_STARTING,  		"STARTING" },
    	{ S_CLOSED,  		"CLOSED" },
    	{ S_STOPPED,		"STOPPED" },
    	{ S_CLOSING,  		"CLOSING" },
    	{ S_STOPPING,		"STOPPING" },
    	{ S_REQUEST_SENT,  	"REQUEST_SENT" },
    	{ S_ACK_RECEIVED,  	"ACK_RECEIVED" },
    	{ S_ACK_SENT,		"ACK_SENT" },
    	{ S_OPENED,  		"OPENED" },
    	{ S_INVLD,			"Unknown" },
    };

    U8 i;

    for(i=0; ppp_state_desc_tbl[i].state!=S_INVLD; i++) {
        if (ppp_state_desc_tbl[i].state == state)  break;
    }

    return ppp_state_desc_tbl[i].str;
}

/*-------------------------------------------------------------------
 * PPP_event2str
 *
 * input : event
 * return: string of corresponding event value
 *------------------------------------------------------------------*/
char *PPP_event2str(U16 event)
{
    static struct {
        PPP_EVENT_TYPE	event;
        char			str[64];
    } ppp_event_desc_tbl[] = {
        { E_UP,  			"UP" },
        { E_DOWN,  			"DOWN" },
        { E_OPEN,  			"OPEN" },
        { E_CLOSE,			"CLOSE" },
    	{ E_TIMEOUT_COUNTER_POSITIVE, "TIMEOUT_COUNTER_POSITIVE" },
    	{ E_TIMEOUT_COUNTER_EXPIRED,  "TIMEOUT_COUNTER_EXPIRED" },
    	{ E_RECV_GOOD_CONFIG_REQUEST, "RECV_GOOD_CONFIG_REQUEST" },
    	{ E_RECV_BAD_CONFIG_REQUEST,  "RECV_BAD_CONFIG_REQUEST" },
    	{ E_RECV_CONFIG_ACK,		  "RECV_CONFIG_ACK" },
    	{ E_RECV_CONFIG_NAK_REJ,  	  "RECV_CONFIG_NAK_REJECT" },
    	{ E_RECV_TERMINATE_REQUEST,	  "RECV_TERMINATE_REQUEST" },
        { E_RECV_TERMINATE_ACK,  	  "RECV_TERMINATE_ACK" },
    	{ E_RECV_UNKNOWN_CODE,  	  "RECV_UNKNOWN_CODE" },
    	{ E_RECV_GOOD_CODE_PROTOCOL_REJECT,	"RECV_GOOD_CODE_PROTOCOL_REJECT" },
    	{ E_RECV_BAD_CODE_PROTOCOL_REJECT,  "RECV_BAD_CODE_PROTOCOL_REJECT" },
    	{ E_RECV_ECHO_REQUEST, "RECV_ECHO_REQUEST" },
    	{ E_RECV_ECHO_REPLY,  "RECV_ECHO_REPLY" },
        { E_UNKNOWN,  		"UNKNOWN" },
    };

    U8 i;

    for(i=0; ppp_event_desc_tbl[i].event!=E_UNKNOWN; i++) {
        if (ppp_event_desc_tbl[i].event == event)  break;
    }

    return ppp_event_desc_tbl[i].str;
}

/*-------------------------------------------------------------------
 * DHCP_state2str
 *
 * input : state
 * return: string of corresponding state value
 *------------------------------------------------------------------*/
char *DHCP_state2str(U16 state)
{
    static struct {
        DHCP_STATE	state;
        char		str[20];
    } dhcp_state_desc_tbl[] = {
    	{ S_DHCP_INIT,  		"DHCP INIT" },
    	{ S_DHCP_DISCOVER_RECV, "DHCP DISCOVERY RECV" },
    	{ S_DHCP_OFFER_SENT,  	"DHCP OFFER SENT" },
    	{ S_DHCP_REQUEST_RECV,	"DHCP REQUEST RECV" },
    	{ S_DHCP_ACK_SENT,  	"DHCP ACK SENT" },
    	{ S_DHCP_NAK_SENT,		"DHCP NAK SENT" },
    	{ S_DHCP_INVLD,  		"DHCP INVALID" },
    };

    U8  i;

    for(i=0; dhcp_state_desc_tbl[i].state != S_DHCP_INVLD; i++) {
        if (dhcp_state_desc_tbl[i].state == state)  break;
    }

    return dhcp_state_desc_tbl[i].str;
}

void PPPLOGMSG(void *ccb, char *buf)
{
    ppp_ccb_t *s_ppp_ccb = (ppp_ccb_t *)ccb;
    if (s_ppp_ccb)
    	sprintf(buf, "pppd> Session id [%x] ", rte_be_to_cpu_16(s_ppp_ccb->session_id));
}

void DHCPLOGMSG(void *ccb, char *buf)
{
    dhcp_ccb_t *dhcp_ccb = (dhcp_ccb_t *)ccb;
    if (dhcp_ccb)
    	sprintf(buf, "dhcpd> ");
}

/***************************************************
 * LOGGER:
 ***************************************************/	
void LOGGER(U8 level, const char *filename, int line_num, FILE *log_fp, void *ccb, 
    void (*ccb2str)(void *, char *), const char *fmt,...)
{
    va_list ap; /* points to each unnamed arg in turn */
    char    buf[LOGGER_BUF_LEN], protocol_buf[LOGGER_BUF_LEN-100], 
            msg[DBG_FastRG_MSG_LEN], timestamp[32];
    time_t  now;
    struct tm *tm_info;

    pthread_mutex_lock(&log_mutex);

    protocol_buf[0] = '\0';
    msg[0] = 0;
    buf[0] = 0;

    //user offer level must > system requirement
    if (fastrg_ccb->loglvl > level) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    /* Get timestamp */
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    va_start(ap, fmt); /* set ap pointer to 1st unnamed arg */
    vsnprintf(msg, DBG_FastRG_MSG_LEN, fmt, ap);
    va_end(ap);

    if (ccb2str)
        ccb2str(ccb, protocol_buf);

    snprintf(buf, sizeof(buf)-1, "FastRG[%s][%s]: %s:%d> %s%s", 
        loglvl2str(level), timestamp, filename, line_num, protocol_buf, msg);
    buf[sizeof(buf) - 1] = '\0';

    buf[sizeof(buf)-1] = '\0';
    if (fastrg_ccb->loglvl == LOGDBG && log_fp != stdout)
        fprintf(stdout, "%s\n", buf);
    if (log_fp != NULL) {
        fprintf(log_fp, "%s\n", buf);
        fflush(log_fp);
    }

    pthread_mutex_unlock(&log_mutex);
}

void dbg_init(void *ccb)
{
    fastrg_ccb = (FastRG_t *)ccb;
}
