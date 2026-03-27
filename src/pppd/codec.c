#include <rte_timer.h>
#include <rte_ether.h>
#include <rte_memcpy.h>
#include <rte_byteorder.h>
#include <rte_errno.h>

#include "../protocol.h"
#include "../dbg.h"
#include "../fastrg.h"
#include "../dp.h"
#include "../utils.h"
#include "codec.h"

typedef enum check_nak_rej_result {
    CHECK_NAK_REJ_ERROR = -1,
    CHECK_NAK_REJ_NO_ACTION = 0,
    CHECK_NAK_REJ_SEND_RESPONSE = 1
} check_nak_rej_result_t;

/**
 * @fn check_ipcp_nak_rej
 *
 * @brief check whether IPCP config request we received includes PPP options we dont want.
 *
 * @param flag
 *      check NAK/REJ
 * @param pppoe_header
 *      pppoe_header pointer
 * @param ppp_payload
 *      ppp_payload pointer
 * @param ppp_hdr
 *      ppp_hdr pointer
 * @param ppp_options
 *      ppp_options pointer
 * @param ppp_hdr_len
 * 
 * @return should send NAK/REJ or ACK
 **/
check_nak_rej_result_t check_ipcp_nak_rej(U8 flag, ppp_ccb_t *s_ppp_ccb, U16 ppp_hdr_len)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    pppoe_header_t *pppoe_header = &(s_ppp_ccb->pppoe_header);
    ppp_header_t *ppp_hdr = &(s_ppp_ccb->ppp_phase[1].ppp_hdr);
    ppp_options_t *ppp_options = s_ppp_ccb->ppp_phase[1].ppp_options;
    ppp_options_t *tmp_buf = fastrg_malloc(ppp_options_t, PPP_MSG_BUF_LEN*sizeof(char), 0);
    ppp_options_t *tmp_cur = tmp_buf;
    int bool_flag = 0;
    U16 tmp_total_length = 4;

    if (tmp_buf == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "check_ipcp_nak_rej failed: fastrg_malloc failed: %s", rte_strerror(rte_errno));
        return CHECK_NAK_REJ_ERROR;
    }

    memset(tmp_buf, 0, PPP_MSG_BUF_LEN);
    rte_memcpy(tmp_buf, ppp_options, ppp_hdr_len-sizeof(ppp_header_t));

    ppp_hdr->length = sizeof(ppp_header_t);
    for(ppp_options_t *cur=ppp_options; tmp_total_length<ppp_hdr_len; cur=(ppp_options_t *)((char *)cur + cur->length)) {
        if (flag == CONFIG_NAK) {
            if (cur->type == IP_ADDRESS && cur->val[0] == 0) {
                bool_flag = 1;
                rte_memcpy(tmp_cur,cur,cur->length);
                ppp_hdr->length += cur->length;
                tmp_cur = (ppp_options_t *)((char *)tmp_cur + cur->length);
            }
        } else {
            if (cur->type != IP_ADDRESS) {
                bool_flag = 1;
                rte_memcpy(tmp_cur,cur,cur->length);
                ppp_hdr->length += cur->length;
                tmp_cur = (ppp_options_t *)((char *)tmp_cur + cur->length);
            }
        }
        tmp_total_length += cur->length;
    }

    if (bool_flag == 1) {
        rte_memcpy(ppp_options, tmp_buf, ppp_hdr->length - sizeof(ppp_header_t));
        pppoe_header->length = rte_cpu_to_be_16((ppp_hdr->length) + sizeof(ppp_payload_t));
        ppp_hdr->length = rte_cpu_to_be_16(ppp_hdr->length);
        ppp_hdr->code = flag;
        fastrg_mfree(tmp_buf);

        return CHECK_NAK_REJ_SEND_RESPONSE;
    }
    fastrg_mfree(tmp_buf);
    return CHECK_NAK_REJ_NO_ACTION;
}

/**
 * @fn check_lcp_nak_rej
 *
 * @brief check whether LCP config request we received includes PPP options we dont want.
 * 
 * @param flag
 *      check NAK/REJ
 * @param pppoe_header
 *      pppoe_header pointer
 * @param ppp_payload
 *      ppp_payload pointer
 * @param ppp_hdr
 *      ppp_hdr pointer
 * @param ppp_options
 *      ppp_options pointer
 * @param ppp_hdr_len
 *      ppp_hdr_len
 * 
 * @return should send NAK/REJ or ACK
 **/
check_nak_rej_result_t check_nak_reject(U8 flag, ppp_ccb_t *s_ppp_ccb, U16 ppp_hdr_len)
{
    FastRG_t       *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    pppoe_header_t *pppoe_header = &(s_ppp_ccb->pppoe_header);
    ppp_header_t   *ppp_hdr = &(s_ppp_ccb->ppp_phase[0].ppp_hdr);
    ppp_options_t  *ppp_options = s_ppp_ccb->ppp_phase[0].ppp_options;
    ppp_options_t  *tmp_buf = fastrg_malloc(ppp_options_t, PPP_MSG_BUF_LEN, 0);
    ppp_options_t  *tmp_cur = tmp_buf;
    BOOL           need_res_nak_reject = FALSE;
    U16            tmp_total_length = 4;

    if (tmp_buf == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "check_nak_reject failed: fastrg_malloc failed: %s", rte_strerror(rte_errno));
        return CHECK_NAK_REJ_ERROR;
    }

    if (ppp_hdr_len < sizeof(ppp_header_t) || 
            (ppp_hdr_len - sizeof(ppp_header_t)) > PPP_MSG_BUF_LEN) {
        fastrg_mfree(tmp_buf);
        return CHECK_NAK_REJ_ERROR;
    }

    memset(tmp_buf, 0, PPP_MSG_BUF_LEN);
    rte_memcpy(tmp_buf, ppp_options, ppp_hdr_len-sizeof(ppp_header_t));

    ppp_hdr->length = sizeof(ppp_header_t);
    for(ppp_options_t *cur=ppp_options; tmp_total_length<ppp_hdr_len; cur=(ppp_options_t *)((char *)cur + cur->length)) {
        if (cur->length == 0 || cur->length > (ppp_hdr_len - tmp_total_length)) {
            FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "Invalid PPP option length");
            fastrg_mfree(tmp_buf);
            return ERROR;
        }
        if (flag == CONFIG_NAK) {
            U8 len_byte = PPP_MRU_LOW_BYTE;
            if (cur->type == MRU && (cur->val[0] != PPP_MRU_HIGH_BYTE || cur->val[1] != len_byte)) {
                need_res_nak_reject = TRUE;
                FastRG_LOG(WARN, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "MRU = %x%x", cur->val[0], cur->val[1]);
                cur->val[0] = PPP_MRU_HIGH_BYTE;
                cur->val[1] = len_byte;
                rte_memcpy(tmp_cur, cur, cur->length);
                ppp_hdr->length += cur->length;
                tmp_cur = (ppp_options_t *)((char *)tmp_cur + cur->length);
            } else if (cur->type == AUTH) {
                U16 ppp_server_auth_method = cur->val[0] << 8 | cur->val[1];
                if (ppp_server_auth_method != s_ppp_ccb->auth_method) {
                    /* if server wants to use pap or chap, then we just follow it */
                    if (ppp_server_auth_method == PAP_PROTOCOL) {
                        s_ppp_ccb->auth_method = PAP_PROTOCOL;
                    } else if (ppp_server_auth_method == CHAP_PROTOCOL) {
                        s_ppp_ccb->auth_method = CHAP_PROTOCOL;
                    } else {
                        /* unknown auth method */

                        need_res_nak_reject = TRUE;
                        /* by default, we use pap auth */
                        s_ppp_ccb->auth_method = PAP_PROTOCOL;
                        cur->val[1] = s_ppp_ccb->auth_method & 0xff;
                        cur->val[0] = (s_ppp_ccb->auth_method & 0xff00) >> 8;
                        rte_memcpy(tmp_cur, cur, cur->length);
                        ppp_hdr->length += cur->length;
                        tmp_cur = (ppp_options_t *)((char *)tmp_cur + cur->length);
                    }
                }
            }
        } else {
            if (cur->type != MAGIC_NUM && cur->type != MRU && cur->type != AUTH) {
                need_res_nak_reject = TRUE;
                rte_memcpy(tmp_cur, cur, cur->length);
                ppp_hdr->length += cur->length;
                tmp_cur = (ppp_options_t *)((char *)tmp_cur + cur->length);
            }
        }
        tmp_total_length += cur->length;
    }

    if (need_res_nak_reject == TRUE) {
        rte_memcpy(ppp_options, tmp_buf, ppp_hdr->length - sizeof(ppp_header_t));
        pppoe_header->length = rte_cpu_to_be_16((ppp_hdr->length) + sizeof(ppp_payload_t));
        ppp_hdr->length = rte_cpu_to_be_16(ppp_hdr->length);
        ppp_hdr->code = flag;
        fastrg_mfree(tmp_buf);

        return CHECK_NAK_REJ_SEND_RESPONSE;
    }
    fastrg_mfree(tmp_buf);
    return CHECK_NAK_REJ_NO_ACTION;
}

STATUS decode_lcp(U16 ppp_hdr_len, U16 *event, struct rte_timer *tim, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t      *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    ppp_header_t  *ppp_hdr = &(s_ppp_ccb->ppp_phase[0].ppp_hdr);
    ppp_options_t *ppp_options = s_ppp_ccb->ppp_phase[0].ppp_options;

    switch(ppp_hdr->code) {
        case CONFIG_REQUEST : 
            if (s_ppp_ccb->phase != LCP_PHASE)
                return ERROR;
            /* we check if the request packet contains what we want */
            switch (check_nak_reject(CONFIG_NAK, s_ppp_ccb, ppp_hdr_len)) {
                case ERROR:
                    return ERROR;
                case 1:
                    *event = E_RECV_BAD_CONFIG_REQUEST;
                    return SUCCESS;
                default:
                    ;
            }
            switch (check_nak_reject(CONFIG_REJECT, s_ppp_ccb, ppp_hdr_len)) {
                case ERROR:
                        return ERROR;
                case 1:
                    *event = E_RECV_BAD_CONFIG_REQUEST;
                    return SUCCESS;
                default:
                    ;
            }
            *event = E_RECV_GOOD_CONFIG_REQUEST;
            ppp_hdr->length = rte_cpu_to_be_16(ppp_hdr_len);
            return SUCCESS;
        case CONFIG_ACK :
            if (s_ppp_ccb->phase != LCP_PHASE)
                return ERROR;
            if (ppp_hdr->identifier != s_ppp_ccb->identifier)
                return ERROR;

            /* only check magic number. Skip the bytes stored in ppp_options_t length to find magic num. */
            U8 ppp_options_length = 0;
            for(ppp_options_t *cur=ppp_options; ppp_options_length<(rte_cpu_to_be_16(ppp_hdr->length)-4);) {
                if (cur->type == MAGIC_NUM) {
                    for(int i=cur->length-3; i>=0; i--) {
                        if (*(((U8 *)&(s_ppp_ccb->magic_num)) + i) != cur->val[i]) {
                            FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "Session 0x%x recv ppp LCP magic number error.", rte_cpu_to_be_16(s_ppp_ccb->session_id));
                            return ERROR;
                        }
                    }
                }
                ppp_options_length += cur->length;
                cur = (ppp_options_t *)((char *)cur + cur->length);
            }
            *event = E_RECV_CONFIG_ACK;
            rte_timer_stop(tim);
            return SUCCESS;
        case CONFIG_NAK : 
            *event = E_RECV_CONFIG_NAK_REJ;
            if (ppp_options->type == AUTH)
                s_ppp_ccb->auth_method = PAP_PROTOCOL;
            FastRG_LOG(WARN, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " recv LCP nak message with option %x.", s_ppp_ccb->user_num, ppp_options->type);
            return SUCCESS;
        case CONFIG_REJECT :
            *event = E_RECV_CONFIG_NAK_REJ;
            FastRG_LOG(WARN, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " recv LCP reject message with option %x.", s_ppp_ccb->user_num, ppp_options->type);
            if (ppp_options->type == AUTH) {
                if (s_ppp_ccb->is_pap_auth == TRUE)
                    return ERROR;
                s_ppp_ccb->is_pap_auth = TRUE;
                s_ppp_ccb->auth_method = PAP_PROTOCOL;
            }
            return SUCCESS;
        case TERMIN_REQUEST :
            *event = E_RECV_TERMINATE_REQUEST;
            return SUCCESS;
        case TERMIN_ACK :
            *event = E_RECV_TERMINATE_ACK;
            rte_timer_stop(tim);
            return SUCCESS;
        case CODE_REJECT:
            *event = E_RECV_GOOD_CODE_PROTOCOL_REJECT;
            return SUCCESS;
        case PROTO_REJECT:
            *event = E_RECV_BAD_CODE_PROTOCOL_REJECT;
            return SUCCESS;
        case ECHO_REQUEST:
            if (s_ppp_ccb->phase < LCP_PHASE)
                return ERROR;
            rte_timer_stop(&(s_ppp_ccb->ppp_alive));
            rte_timer_reset(&(s_ppp_ccb->ppp_alive), ppp_interval*rte_get_timer_hz(), 
                SINGLE, fastrg_ccb->lcore.ctrl_thread, 
                (rte_timer_cb_t)PPP_bye_timer_cb, s_ppp_ccb);
            *event = E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST;
            return SUCCESS;
        case ECHO_REPLY:
            if (s_ppp_ccb->phase < LCP_PHASE)
                return ERROR;
            *event = E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST;
            return SUCCESS;
        default :
            *event = E_RECV_UNKNOWN_CODE;
    }

    return SUCCESS;
}

STATUS decode_ipcp(U16 ppp_hdr_len, U16 *event, struct rte_timer *tim, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t      *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    ppp_header_t  *ppp_hdr = &(s_ppp_ccb->ppp_phase[1].ppp_hdr);
    ppp_options_t *ppp_options = s_ppp_ccb->ppp_phase[1].ppp_options;

    switch(ppp_hdr->code) {
        case CONFIG_REQUEST : 
            switch (check_ipcp_nak_rej(CONFIG_NAK, s_ppp_ccb, ppp_hdr_len)) {
                case ERROR:
                    return ERROR;
                case 1:
                    *event = E_RECV_BAD_CONFIG_REQUEST;
                    return SUCCESS;
                default:
                    ;
            }
            switch (check_ipcp_nak_rej(CONFIG_REJECT, s_ppp_ccb, ppp_hdr_len)) {
                case ERROR:
                    return ERROR;
                case 1:
                    *event = E_RECV_BAD_CONFIG_REQUEST;
                    return SUCCESS;
                default:
                    ;
            }
            rte_memcpy(&(s_ppp_ccb->hsi_ipv4_gw), ppp_options->val,sizeof(s_ppp_ccb->hsi_ipv4_gw));
            *event = E_RECV_GOOD_CONFIG_REQUEST;
            ppp_hdr->length = rte_cpu_to_be_16(ppp_hdr_len);
            return SUCCESS;
        case CONFIG_ACK :
            if (ppp_hdr->identifier != s_ppp_ccb->identifier)
                return FALSE;
            rte_timer_stop(tim);
            *event = E_RECV_CONFIG_ACK;
            rte_memcpy(&(s_ppp_ccb->hsi_ipv4),ppp_options->val,sizeof(s_ppp_ccb->hsi_ipv4));
            return SUCCESS;
        case CONFIG_NAK : 
            // if we receive nak packet, the option field contains correct ip address we want
            rte_memcpy(&(s_ppp_ccb->hsi_ipv4),ppp_options->val,4);
            *event = E_RECV_CONFIG_NAK_REJ;
            return SUCCESS;
        case CONFIG_REJECT :
            *event = E_RECV_CONFIG_NAK_REJ;
            return SUCCESS;
        case TERMIN_REQUEST :
            *event = E_RECV_TERMINATE_REQUEST;
            return SUCCESS;
        case TERMIN_ACK :
            FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " vlan 0x%x recv termin ack.", 
                s_ppp_ccb->user_num, rte_atomic16_read(&s_ppp_ccb->vlan_id));
            rte_timer_stop(tim);
            *event = E_RECV_TERMINATE_ACK;
            return SUCCESS;
        case CODE_REJECT:
            *event = E_RECV_GOOD_CODE_PROTOCOL_REJECT;
            return SUCCESS;
        default :
            *event = E_RECV_UNKNOWN_CODE;
    }
    return SUCCESS;
}

STATUS build_padi(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    pppoe_header_tag_t   *pppoe_header_tag = (pppoe_header_tag_t *)(pppoe_header + 1);

    for(int i=0; i<RTE_ETHER_ADDR_LEN; i++) {
        eth_hdr->src_addr.addr_bytes[i] = fastrg_ccb->nic_info.hsi_wan_src_mac.addr_bytes[i];
        eth_hdr->dst_addr.addr_bytes[i] = 0xff;
    }
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header->tci_union.tci_struct.priority = 0;
    vlan_header->tci_union.tci_struct.DEI = 0;
    vlan_header->tci_union.tci_struct.vlan_id = rte_atomic16_read(&s_ppp_ccb->vlan_id);
    vlan_header->next_proto = rte_cpu_to_be_16(ETH_P_PPP_DIS);
    vlan_header->tci_union.tci_value = rte_cpu_to_be_16(vlan_header->tci_union.tci_value);

    pppoe_header->ver_type = VER_TYPE;
    pppoe_header->code = PADI;
    pppoe_header->session_id = 0; 

    pppoe_header_tag->type = rte_cpu_to_be_16(SERVICE_NAME); //padi tag type (service name)
    pppoe_header_tag->length = 0;

    pppoe_header->length = rte_cpu_to_be_16(sizeof(pppoe_header_tag_t));

    *mulen = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t) + sizeof(pppoe_header_tag_t);

    return SUCCESS;
}

STATUS build_padr(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    pppoe_header_tag_t   *pppoe_header_tag = (pppoe_header_tag_t *)(pppoe_header + 1);

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &s_ppp_ccb->eth_hdr.src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &s_ppp_ccb->eth_hdr.dst_addr);
    s_ppp_ccb->pppoe_header.code = PADR;

    U32 total_tag_length = 0;
    pppoe_header_tag_t *cur = s_ppp_ccb->pppoe_phase.pppoe_header_tag;
    pppoe_header_tag->length = 0;
    pppoe_header_tag->type = rte_cpu_to_be_16(SERVICE_NAME);
    pppoe_header_tag += 1;
    total_tag_length += sizeof(pppoe_header_tag_t);
    for(;;) {
        pppoe_header_tag->type = cur->type;
        pppoe_header_tag->length = cur->length;
        U16 tag_len = ntohs(cur->length);
        switch(ntohs(cur->type)) {
            case END_OF_LIST:
                break;
            case SERVICE_NAME:
                break;
            case AC_NAME:
                /* We dont need to add ac-name tag to PADR. */
                cur = (pppoe_header_tag_t *)((char *)cur + sizeof(pppoe_header_tag_t) + tag_len);
                continue;
            case HOST_UNIQ:
            case AC_COOKIE:
            case RELAY_ID:
                if (cur->length != 0) {
                    rte_memcpy(pppoe_header_tag->value, cur->value, tag_len);
                    total_tag_length = tag_len + sizeof(pppoe_header_tag_t) + total_tag_length;
                }
                break;
            case GENERIC_ERROR:
                FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "PPPoE discover generic error.");
                return FALSE;
            default:
                FastRG_LOG(WARN, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "Unknown PPPOE tag value.");
        }
        if (ntohs(cur->type) == END_OF_LIST)
            break;

        /* to caculate total pppoe header tags' length, we need to add tag type and tag length field in each tag scanning. */
        /* Fetch next tag field. */
        cur = (pppoe_header_tag_t *)((char *)cur + sizeof(pppoe_header_tag_t) + tag_len);
        pppoe_header_tag = (pppoe_header_tag_t *)((char *)pppoe_header_tag + sizeof(pppoe_header_tag_t) + tag_len);
    }

    s_ppp_ccb->pppoe_header.length = rte_cpu_to_be_16(total_tag_length);
    *mulen = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t) + total_tag_length;

    *eth_hdr = s_ppp_ccb->eth_hdr;
    *vlan_header = s_ppp_ccb->vlan_header;
    *pppoe_header = s_ppp_ccb->pppoe_header;

    return SUCCESS;
}

void build_padt(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header->tci_union.tci_struct.priority = 0;
    vlan_header->tci_union.tci_struct.DEI = 0;
    vlan_header->tci_union.tci_struct.vlan_id = rte_atomic16_read(&s_ppp_ccb->vlan_id);
    vlan_header->next_proto = rte_cpu_to_be_16(ETH_P_PPP_DIS);
    vlan_header->tci_union.tci_value = rte_cpu_to_be_16(vlan_header->tci_union.tci_value);

    pppoe_header->ver_type = VER_TYPE;
    pppoe_header->code = PADT;
    pppoe_header->session_id = s_ppp_ccb->session_id; 
    pppoe_header->length = 0;

    *mulen = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t);
}

void build_config_request(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    ppp_payload_t        *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
    ppp_header_t         *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);
    ppp_options_t        *ppp_options = (ppp_options_t *)(ppp_hdr + 1);
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);

    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header->tci_union.tci_struct.priority = 0;
    vlan_header->tci_union.tci_struct.DEI = 0;
    vlan_header->tci_union.tci_struct.vlan_id = rte_atomic16_read(&s_ppp_ccb->vlan_id);
    vlan_header->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
    vlan_header->tci_union.tci_value = rte_cpu_to_be_16(vlan_header->tci_union.tci_value);

    /* build ppp protocol and lcp header. */
    pppoe_header->ver_type = VER_TYPE;
    pppoe_header->code = 0;
    /* We don't convert seesion id to little endian at first */
    pppoe_header->session_id = s_ppp_ccb->session_id; 

    ppp_hdr->code = CONFIG_REQUEST;
    s_ppp_ccb->identifier = (s_ppp_ccb->identifier % UINT8_MAX) + 1;
    ppp_hdr->identifier = s_ppp_ccb->identifier;

    pppoe_header->length = sizeof(ppp_header_t) + sizeof(ppp_payload->ppp_protocol);
    ppp_hdr->length = sizeof(ppp_header_t);

    if (s_ppp_ccb->cp == 1) {
        ppp_payload->ppp_protocol = rte_cpu_to_be_16(IPCP_PROTOCOL);
        ppp_options->type = IP_ADDRESS;
        rte_memcpy(ppp_options->val, &(s_ppp_ccb->hsi_ipv4), 4);
        ppp_options->length = sizeof(s_ppp_ccb->hsi_ipv4) + sizeof(ppp_options_t);
        pppoe_header->length += ppp_options->length;
        ppp_hdr->length += ppp_options->length;
    } else if (s_ppp_ccb->cp == 0) {
        ppp_payload->ppp_protocol = rte_cpu_to_be_16(LCP_PROTOCOL);
        ppp_options_t *cur = ppp_options;
        /* option, auth */
        /*if (s_ppp_ccb->auth_method == PAP_PROTOCOL) {
            cur->type = AUTH;
            cur->length = 0x4;
            U16 auth_pro = rte_cpu_to_be_16(PAP_PROTOCOL);
            rte_memcpy(cur->val,&auth_pro,sizeof(U16));
            pppoe_header->length += 4;
            ppp_hdr->length += 4;

            cur = (ppp_options_t *)((char *)(cur + 1) + sizeof(auth_pro));
        } else if (s_ppp_ccb->auth_method == CHAP_PROTOCOL) {
            cur->type = AUTH;
            cur->length = 0x5;
            U16 auth_pro = rte_cpu_to_be_16(CHAP_PROTOCOL);
            rte_memcpy(cur->val,&auth_pro,sizeof(U16));
            U8 auth_method = 0x5; // CHAP with MD5
            rte_memcpy((cur->val)+2,&auth_method,sizeof(U8));
            pppoe_header->length += 5;
            ppp_hdr->length += 5;

            cur = (ppp_options_t *)((char *)(cur + 1) + sizeof(auth_pro) + sizeof(auth_method));
        }*/
        /* options, max recv units */

        cur->type = MRU;
        cur->length = 0x4;
        U16 max_recv_unit = rte_cpu_to_be_16(MAX_RECV_UNIT);
        rte_memcpy(cur->val, &max_recv_unit, sizeof(U16));
        pppoe_header->length += 4;
        ppp_hdr->length += 4;

        cur = (ppp_options_t *)((char *)(cur + 1) + sizeof(max_recv_unit));
        /* options, magic number */
        cur->type = MAGIC_NUM;
        cur->length = 0x6;
        *(U32 *)(cur->val) = s_ppp_ccb->magic_num;
        pppoe_header->length += 6;
        ppp_hdr->length += 6;
    }

    *mulen = pppoe_header->length + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t);

    pppoe_header->length = rte_cpu_to_be_16(pppoe_header->length);
    ppp_hdr->length = rte_cpu_to_be_16(ppp_hdr->length);

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " config request built.", s_ppp_ccb->user_num);
}

void build_config_ack(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    ppp_payload_t        *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
    ppp_header_t         *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);
    ppp_options_t        *ppp_options = (ppp_options_t *)(ppp_hdr + 1);

    *eth_hdr = s_ppp_ccb->eth_hdr;
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);

    *vlan_header = s_ppp_ccb->vlan_header;
    *pppoe_header = s_ppp_ccb->pppoe_header;
    *ppp_payload = s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_payload;
    *ppp_hdr = s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_hdr;
    ppp_hdr->code = CONFIG_ACK;
    rte_memcpy(ppp_options, s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_options, rte_cpu_to_be_16(ppp_hdr->length) - sizeof(ppp_header_t));

    *mulen = rte_be_to_cpu_16(pppoe_header->length) + sizeof(struct rte_ether_hdr) + sizeof(pppoe_header_t) + sizeof(vlan_header_t);

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " config ack built.", s_ppp_ccb->user_num);
}

void build_config_nak_rej(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    ppp_payload_t        *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
    ppp_header_t         *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);
    ppp_options_t        *ppp_options = (ppp_options_t *)(ppp_hdr + 1);

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = s_ppp_ccb->eth_hdr.ether_type;

    *vlan_header = s_ppp_ccb->vlan_header;
    *pppoe_header = s_ppp_ccb->pppoe_header;
    *ppp_payload = s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_payload;
    *ppp_hdr = s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_hdr;

    *mulen = rte_be_to_cpu_16(pppoe_header->length) + sizeof(struct rte_ether_hdr) + sizeof(pppoe_header_t) + sizeof(vlan_header_t);

    rte_memcpy(ppp_options, s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_options, rte_be_to_cpu_16(ppp_hdr->length) - sizeof(ppp_header_t));

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " config nak/rej built.", s_ppp_ccb->user_num);
}

void build_echo_reply(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    ppp_payload_t        *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
    ppp_header_t         *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);
    U8 *magic_num = (U8 *)(ppp_hdr + 1);
    U8 ppp_opt_len = rte_be_to_cpu_16(s_ppp_ccb->ppp_phase[0].ppp_hdr.length) - sizeof(ppp_header_t);

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = s_ppp_ccb->eth_hdr.ether_type;

    *vlan_header = s_ppp_ccb->vlan_header;
    *pppoe_header = s_ppp_ccb->pppoe_header;
    *ppp_payload = s_ppp_ccb->ppp_phase[0].ppp_payload;
    *ppp_hdr = s_ppp_ccb->ppp_phase[0].ppp_hdr;

    ppp_hdr->code = ECHO_REPLY;
    ppp_hdr->length = sizeof(ppp_header_t);
    pppoe_header->length = sizeof(ppp_payload_t) + sizeof(ppp_header_t);

    if (ppp_opt_len > 0) {
        *(U32 *)magic_num = s_ppp_ccb->magic_num;
        ppp_hdr->length += sizeof(s_ppp_ccb->magic_num);
        pppoe_header->length += sizeof(s_ppp_ccb->magic_num);
    }
    ppp_opt_len -= sizeof(s_ppp_ccb->magic_num);
    if (ppp_opt_len == sizeof(U32)/* echo requester's nmagic number */) {
        magic_num += sizeof(s_ppp_ccb->magic_num);
        *(U32 *)magic_num = *(U32 *)s_ppp_ccb->ppp_phase[0].ppp_options;
        ppp_hdr->length += ppp_opt_len;
        pppoe_header->length += ppp_opt_len;
    }

    *mulen = pppoe_header->length + sizeof(struct rte_ether_hdr) + sizeof(pppoe_header_t) + sizeof(vlan_header_t);
    ppp_hdr->length = rte_cpu_to_be_16(ppp_hdr->length);
    pppoe_header->length = rte_cpu_to_be_16(pppoe_header->length);
}

void build_terminate_ack(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    ppp_payload_t        *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
    ppp_header_t         *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = s_ppp_ccb->eth_hdr.ether_type;

    *vlan_header = s_ppp_ccb->vlan_header;
    *pppoe_header = s_ppp_ccb->pppoe_header;
    *ppp_payload = s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_payload;
    *ppp_hdr = s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_hdr;

    ppp_hdr->code = TERMIN_ACK;
    ppp_hdr->length = rte_cpu_to_be_16(sizeof(ppp_header_t));

    pppoe_header->length = rte_cpu_to_be_16(sizeof(ppp_header_t) + sizeof(ppp_payload_t));

    *mulen = rte_be_to_cpu_16(pppoe_header->length) + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t);

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " terminate ack built.", s_ppp_ccb->user_num);
}

void build_terminate_request(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    ppp_payload_t        *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
    ppp_header_t         *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header->tci_union.tci_struct.priority = 0;
    vlan_header->tci_union.tci_struct.DEI = 0;
    vlan_header->tci_union.tci_struct.vlan_id = rte_atomic16_read(&s_ppp_ccb->vlan_id);
    vlan_header->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
    vlan_header->tci_union.tci_value = rte_cpu_to_be_16(vlan_header->tci_union.tci_value);
    /* build ppp protocol and lcp/ipcp header. */

    pppoe_header->ver_type = VER_TYPE;
    pppoe_header->code = 0;
    /* We don't convert seesion id to little endian at first */
    pppoe_header->session_id = s_ppp_ccb->session_id;

    if (s_ppp_ccb->cp == 0) 
        ppp_payload->ppp_protocol = rte_cpu_to_be_16(LCP_PROTOCOL);
    else if (s_ppp_ccb->cp == 1)
        ppp_payload->ppp_protocol = rte_cpu_to_be_16(IPCP_PROTOCOL);

    ppp_hdr->code = TERMIN_REQUEST;
    ppp_hdr->identifier = ((rand() % 254) + 1);

    pppoe_header->length = sizeof(ppp_header_t) + sizeof(ppp_payload->ppp_protocol);
    ppp_hdr->length = sizeof(ppp_header_t); 	

    *mulen = pppoe_header->length + sizeof(struct rte_ether_hdr) + sizeof(pppoe_header_t) + sizeof(vlan_header_t);
    pppoe_header->length = rte_cpu_to_be_16(pppoe_header->length);
    ppp_hdr->length = rte_cpu_to_be_16(ppp_hdr->length);
    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " terminate request built.", s_ppp_ccb->user_num);
}

STATUS build_code_reject(__attribute__((unused)) unsigned char *buffer, ppp_ccb_t *s_ppp_ccb, __attribute__((unused)) U16 *mulen)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    /* TODO: support code reject */
    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "build code reject is called.");

    return SUCCESS;
}

void build_auth_request_pap(unsigned char *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    ppp_payload_t        *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
    ppp_header_t         *ppp_pap_header = (ppp_header_t *)(ppp_payload + 1);
    U8                   peer_id_length = strlen((const char *)(s_ppp_ccb->ppp_user_acc));
    U8                   peer_passwd_length = strlen((const char *)(s_ppp_ccb->ppp_passwd));
    U8                   *pap_account = (U8 *)(ppp_pap_header + 1);
    U8                   *pap_password = pap_account + peer_id_length + sizeof(U8)/* pap account length field */;

    s_ppp_ccb->phase = AUTH_PHASE;

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    *vlan_header = s_ppp_ccb->vlan_header;
    *pppoe_header = s_ppp_ccb->pppoe_header;
    ppp_payload->ppp_protocol = rte_cpu_to_be_16(PAP_PROTOCOL);

    ppp_pap_header->code = PAP_REQUEST;
    ppp_pap_header->identifier = s_ppp_ccb->identifier;

    *(U8 *)pap_account = peer_id_length;
    rte_memcpy(pap_account + sizeof(U8), s_ppp_ccb->ppp_user_acc, peer_id_length);
    *(U8 *)pap_password = peer_passwd_length;
    rte_memcpy(pap_password + sizeof(U8), s_ppp_ccb->ppp_passwd, peer_passwd_length);

    ppp_pap_header->length = 2 * sizeof(U8)/* for pap account length and pap password length */ 
    + peer_id_length + peer_passwd_length + sizeof(ppp_header_t);
    pppoe_header->length = ppp_pap_header->length + sizeof(ppp_payload_t);
    ppp_pap_header->length = rte_cpu_to_be_16(ppp_pap_header->length);
    pppoe_header->length = rte_cpu_to_be_16(pppoe_header->length);

    *mulen = ntohs(pppoe_header->length) + sizeof(struct rte_ether_hdr) + sizeof(pppoe_header_t) + sizeof(vlan_header_t);

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " pap request built.", s_ppp_ccb->user_num);
}

void build_auth_ack_pap(unsigned char *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb)
{
    const char           *login_msg = "Login ok";
    FastRG_t             *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t        *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t       *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    ppp_payload_t        *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
    ppp_header_t         *ppp_pap_header = (ppp_header_t *)(ppp_payload + 1);
    ppp_pap_ack_nak_t    *ppp_pap_ack_nak = (ppp_pap_ack_nak_t *)(ppp_pap_header + 1);

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&s_ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    *vlan_header = s_ppp_ccb->vlan_header;
    *pppoe_header = s_ppp_ccb->pppoe_header;
    ppp_payload->ppp_protocol = rte_cpu_to_be_16(PAP_PROTOCOL);

    ppp_pap_header->code = PAP_ACK;
    ppp_pap_header->identifier = s_ppp_ccb->identifier;

    ppp_pap_ack_nak->msg_length = strlen(login_msg);
    rte_memcpy(ppp_pap_ack_nak->msg, login_msg, ppp_pap_ack_nak->msg_length);

    ppp_pap_header->length = sizeof(ppp_header_t) + ppp_pap_ack_nak->msg_length + sizeof(ppp_pap_ack_nak->msg_length);
    pppoe_header->length = ppp_pap_header->length + sizeof(ppp_payload_t);
    *mulen = pppoe_header->length + sizeof(struct rte_ether_hdr) + sizeof(pppoe_header_t) + sizeof(vlan_header_t);

    ppp_pap_header->length = rte_cpu_to_be_16(ppp_pap_header->length);
    pppoe_header->length = rte_cpu_to_be_16(pppoe_header->length);

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " pap ack built.", s_ppp_ccb->user_num);
}

/* TODO: not yet well tested */
void build_auth_response_chap(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb, ppp_chap_data_t *ppp_chap_data)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    U8 chap_hash[16];
    U8 *buf_ptr = buffer;
    ppp_chap_data_t new_ppp_chap_data;
    struct rte_ether_addr tmp_mac;

    MD5_CTX  context;

    MD5Init(&context);
    MD5Update(&context, &s_ppp_ccb->ppp_phase[0].ppp_hdr.identifier, 1);
    MD5Update(&context, s_ppp_ccb->ppp_passwd, strlen((const char *)s_ppp_ccb->ppp_passwd));
    MD5Update(&context, ppp_chap_data->val, ppp_chap_data->val_size);
    MD5Final(chap_hash, &context);
    new_ppp_chap_data.val_size = 16;
    new_ppp_chap_data.val = chap_hash;
    new_ppp_chap_data.name = s_ppp_ccb->ppp_user_acc;

    rte_ether_addr_copy(&s_ppp_ccb->eth_hdr.src_addr, &tmp_mac);
    rte_ether_addr_copy(&s_ppp_ccb->eth_hdr.dst_addr, &s_ppp_ccb->eth_hdr.src_addr);
    rte_ether_addr_copy(&tmp_mac, &s_ppp_ccb->eth_hdr.dst_addr);

    *(struct rte_ether_hdr *)buf_ptr = s_ppp_ccb->eth_hdr;
    buf_ptr += sizeof(struct rte_ether_hdr);
    *(vlan_header_t *)buf_ptr = s_ppp_ccb->vlan_header;
    buf_ptr += sizeof(vlan_header_t);
    *(pppoe_header_t *)buf_ptr = s_ppp_ccb->pppoe_header;
    buf_ptr += sizeof(pppoe_header_t);
    *(ppp_payload_t *)buf_ptr = s_ppp_ccb->ppp_phase[0].ppp_payload;
    buf_ptr += sizeof(ppp_payload_t);
    s_ppp_ccb->ppp_phase[0].ppp_hdr.code = CHAP_RESPONSE;
    s_ppp_ccb->ppp_phase[0].ppp_hdr.length = sizeof(ppp_header_t) + 1 + 16 + strlen((const char *)new_ppp_chap_data.name);
    *(ppp_header_t *)buf_ptr = s_ppp_ccb->ppp_phase[0].ppp_hdr;
    buf_ptr += sizeof(ppp_header_t);
    ((ppp_chap_data_t *)buf_ptr)->val_size = new_ppp_chap_data.val_size;
    memcpy(((ppp_chap_data_t *)buf_ptr)->val, new_ppp_chap_data.val, new_ppp_chap_data.val_size);
    memcpy(((ppp_chap_data_t *)buf_ptr)->name, new_ppp_chap_data.name, strlen((const char *)new_ppp_chap_data.name));
    buf_ptr += 1 + 16 + strlen((const char *)new_ppp_chap_data.name);
    *mulen = buf_ptr - buffer;

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " chap response built.", s_ppp_ccb->user_num);
}

STATUS pppoe_send_pkt(U8 encode_type, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    U8 buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;

    switch (encode_type) {
    case ENCODE_PADI:
        if (s_ppp_ccb->pppoe_phase.timer_counter >= s_ppp_ccb->pppoe_phase.max_retransmit) {
            FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " timeout when sending PADI", s_ppp_ccb->user_num);
            exit_ppp(s_ppp_ccb);
            return ERROR;
        }
        if (build_padi(buffer, &mulen, s_ppp_ccb) == ERROR) {
            PPP_bye(s_ppp_ccb);
            return ERROR;
        }
        s_ppp_ccb->pppoe_phase.timer_counter++;
        wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
        break;
    case ENCODE_PADR:
        if (s_ppp_ccb->pppoe_phase.timer_counter >= s_ppp_ccb->pppoe_phase.max_retransmit) {
            FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 "timeout when sending PADR", s_ppp_ccb->user_num);
            exit_ppp(s_ppp_ccb);
            return ERROR;
        }
        if (build_padr(buffer, &mulen, s_ppp_ccb) == ERROR) {
            PPP_bye(s_ppp_ccb);
            return ERROR;
        }
        s_ppp_ccb->pppoe_phase.timer_counter++;
        wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
        break;
    case ENCODE_PADT:
        build_padt(buffer, &mulen, s_ppp_ccb);
        wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
        s_ppp_ccb->phase = PPPOE_PHASE;
        s_ppp_ccb->pppoe_phase.active = FALSE;
        FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %u PPPoE session closed successfully.", s_ppp_ccb->user_num);
        PPP_bye(s_ppp_ccb);
        break;
    default:
        return ERROR;
    }

    return SUCCESS;
}

STATUS get_ccb_id(FastRG_t *fastrg_ccb, U8 *pkt_buf, U16 *ccb_id)
{
    U16 vlan_id;
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_buf;

    vlan_id = ((vlan_header_t *)(eth_hdr + 1))->tci_union.tci_value;
    vlan_id = rte_be_to_cpu_16(vlan_id) & 0xFFF;
    if (vlan_id > MAX_VLAN_ID) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "recv invalid vlan id %u, discard.", vlan_id);
        return ERROR;
    }
    *ccb_id = rte_atomic16_read(&fastrg_ccb->vlan_userid_map[vlan_id - 1]);
    if (*ccb_id >= fastrg_ccb->user_count) {
        FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL, "recv not our PPPoE packet, discard.");
        return ERROR;
    }

    return SUCCESS;
}

int check_auth_result(ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    if (s_ppp_ccb->phase != AUTH_PHASE)
        return 0;

    U16 ppp_protocol = s_ppp_ccb->ppp_phase[0].ppp_payload.ppp_protocol;
    U8 ppp_hdr_code = s_ppp_ccb->ppp_phase[0].ppp_hdr.code;
    if (ppp_protocol == rte_cpu_to_be_16(PAP_PROTOCOL) || ppp_protocol == rte_cpu_to_be_16(CHAP_PROTOCOL)) {
        if (ppp_hdr_code == PAP_NAK || ppp_hdr_code == CHAP_FAILURE) {
            FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 
                "received auth info error and start closing connection.", s_ppp_ccb->user_num);
            s_ppp_ccb->cp = 0;
            PPP_FSM(&s_ppp_ccb->ppp, s_ppp_ccb, E_CLOSE);
            return 1;
        } else if (ppp_hdr_code == PAP_ACK || ppp_hdr_code == CHAP_SUCCESS) {
            s_ppp_ccb->cp = 1;
            s_ppp_ccb->phase = IPCP_PHASE;
            PPP_FSM(&s_ppp_ccb->ppp, s_ppp_ccb, E_OPEN);
            return 1;
        }
    }

    return 0;
}

/*============================ DECODE ===============================*/

STATUS decode_pppoe(pppoe_header_tag_t *pppoe_header_tag, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    U16 pppoe_header_tag_size = rte_be_to_cpu_16(pppoe_header_tag->length);

    switch(s_ppp_ccb->pppoe_header.code) {
    case PADO:
        if (s_ppp_ccb->pppoe_phase.active == TRUE)
            return ERROR;
        s_ppp_ccb->pppoe_phase.active = TRUE;
        rte_memcpy(s_ppp_ccb->pppoe_phase.pppoe_header_tag, pppoe_header_tag, 
            pppoe_header_tag_size > PPPoE_TAG_DEFAULT_MAX_LEN ? 
            PPPoE_TAG_DEFAULT_MAX_LEN : pppoe_header_tag_size);
        s_ppp_ccb->pppoe_phase.max_retransmit = MAX_RETRAN;
        s_ppp_ccb->pppoe_phase.timer_counter = 0;
        rte_timer_stop(&(s_ppp_ccb->pppoe));
        rte_ether_addr_copy(&s_ppp_ccb->eth_hdr.src_addr, &s_ppp_ccb->PPP_dst_mac);
        if (pppoe_send_pkt(ENCODE_PADR, s_ppp_ccb) == ERROR) {
            exit_ppp(s_ppp_ccb);
            return ERROR;
        }
        rte_timer_reset(&(s_ppp_ccb->pppoe), rte_get_timer_hz(), PERIODICAL, 
            fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)A_padr_timer_func, 
            s_ppp_ccb);
        return SUCCESS;
    case PADS:
        rte_timer_stop(&(s_ppp_ccb->pppoe));
        s_ppp_ccb->session_id = s_ppp_ccb->pppoe_header.session_id;
        s_ppp_ccb->cp = 0;
        PPP_FSM(&(s_ppp_ccb->ppp), s_ppp_ccb, E_OPEN);
        return SUCCESS;
    case PADT:
        rte_memcpy(s_ppp_ccb->pppoe_phase.pppoe_header_tag, pppoe_header_tag, 
            pppoe_header_tag_size > PPPoE_TAG_DEFAULT_MAX_LEN ? 
            PPPoE_TAG_DEFAULT_MAX_LEN : pppoe_header_tag_size);
        s_ppp_ccb->pppoe_phase.max_retransmit = MAX_RETRAN;

        FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "Session 0x%x connection disconnected.", rte_be_to_cpu_16(s_ppp_ccb->session_id));
        s_ppp_ccb->phase = END_PHASE;
        s_ppp_ccb->pppoe_phase.active = FALSE;
        s_ppp_ccb->ppp_phase[0].state = S_INIT;
        s_ppp_ccb->ppp_phase[1].state = S_INIT;
        PPP_bye(s_ppp_ccb);
        return SUCCESS;		
    case PADM:
        FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "recv active discovery message");
        return SUCCESS;
    default:
        FastRG_LOG(WARN, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "Unknown PPPoE discovery type %x", s_ppp_ccb->pppoe_header.code);
        return ERROR;
    }
}

STATUS decode_ppp(ppp_payload_t *ppp_payload, U16 *event, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    struct rte_timer *tim = &s_ppp_ccb->ppp;
    ppp_header_t *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);

    U16 ppp_hdr_len = rte_be_to_cpu_16(ppp_hdr->length);

    if (ppp_hdr_len < sizeof(ppp_header_t)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 
            " recv invalid ppp header length %u", s_ppp_ccb->user_num, ppp_hdr_len);
        return ERROR;
    }

    /* check the ppp is in LCP, AUTH or NCP phase */
    if (ppp_payload->ppp_protocol == rte_cpu_to_be_16(IPCP_PROTOCOL)) {
        s_ppp_ccb->ppp_phase[1].ppp_payload = *ppp_payload;
        s_ppp_ccb->ppp_phase[1].ppp_hdr = *ppp_hdr;
        if (s_ppp_ccb->ppp_phase[1].ppp_options != NULL) {
            fastrg_mfree(s_ppp_ccb->ppp_phase[1].ppp_options);
            s_ppp_ccb->ppp_phase[1].ppp_options = NULL;
        }
        s_ppp_ccb->ppp_phase[1].ppp_options = fastrg_malloc(ppp_options_t, ppp_hdr_len-sizeof(ppp_header_t), 0);
        if (s_ppp_ccb->ppp_phase[1].ppp_options == NULL && ppp_hdr_len != sizeof(ppp_header_t)) {
        	FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "fastrg_malloc error");
        	return ERROR;
        }
        rte_memcpy(s_ppp_ccb->ppp_phase[1].ppp_options, ppp_hdr+1, ppp_hdr_len-sizeof(ppp_header_t));
        if (s_ppp_ccb->phase != IPCP_PHASE)
            return ERROR;
        if (decode_ipcp(ppp_hdr_len, event, tim, s_ppp_ccb) == ERROR)
            return ERROR;
        s_ppp_ccb->cp = 1;
    } else if (ppp_payload->ppp_protocol == rte_cpu_to_be_16(LCP_PROTOCOL)) {
        s_ppp_ccb->ppp_phase[0].ppp_payload = *ppp_payload;
        s_ppp_ccb->ppp_phase[0].ppp_hdr = *ppp_hdr;
        if (s_ppp_ccb->ppp_phase[0].ppp_options != NULL) {
            fastrg_mfree(s_ppp_ccb->ppp_phase[0].ppp_options);
            s_ppp_ccb->ppp_phase[0].ppp_options = NULL;
        }
        s_ppp_ccb->ppp_phase[0].ppp_options = fastrg_malloc(ppp_options_t, ppp_hdr_len-sizeof(ppp_header_t), 0);
        if (s_ppp_ccb->ppp_phase[0].ppp_options == NULL && ppp_hdr_len != sizeof(ppp_header_t)) {
            FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "fastrg_malloc error.");
            return ERROR;
        }
        rte_memcpy(s_ppp_ccb->ppp_phase[0].ppp_options, ppp_hdr+1, ppp_hdr_len-sizeof(ppp_header_t));
        if (decode_lcp(ppp_hdr_len, event, tim, s_ppp_ccb) == ERROR)
            return ERROR;
    } else if (ppp_payload->ppp_protocol == rte_cpu_to_be_16(PAP_PROTOCOL)) {
        /* in AUTH phase, if the packet is not what we want, then send nak packet 
            and just close process */
        if (s_ppp_ccb->phase != AUTH_PHASE)
            return ERROR;
        // we don't care what msg pap server send to us, just check it's ack or nak
        if (ppp_hdr->code == PAP_ACK) {
            s_ppp_ccb->ppp_phase[0].ppp_payload = *ppp_payload;
            s_ppp_ccb->ppp_phase[0].ppp_hdr = *ppp_hdr;
            FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " auth success.", s_ppp_ccb->user_num);
            return SUCCESS;
        } else if (ppp_hdr->code == PAP_NAK) {
            s_ppp_ccb->phase = LCP_PHASE;
            PPP_FSM(&(s_ppp_ccb->ppp), s_ppp_ccb, E_CLOSE);
            FastRG_LOG(WARN, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " auth fail.", s_ppp_ccb->user_num);
            return SUCCESS;
        } else if (ppp_hdr->code == PAP_REQUEST) {
            U8 buffer[PPP_MSG_BUF_LEN];
            U16 mulen;
            ppp_ccb_t *tmp_s_ppp_ccb = fastrg_malloc(ppp_ccb_t, sizeof(ppp_ccb_t), 0);
            if (tmp_s_ppp_ccb == NULL) {
                FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "fastrg_malloc error.");
                return ERROR;
            }
            memset(tmp_s_ppp_ccb, 0, sizeof(ppp_ccb_t));

            s_ppp_ccb->phase = AUTH_PHASE;
            tmp_s_ppp_ccb->eth_hdr = s_ppp_ccb->eth_hdr;
            tmp_s_ppp_ccb->vlan_header = s_ppp_ccb->vlan_header;
            tmp_s_ppp_ccb->pppoe_header = s_ppp_ccb->pppoe_header;
            tmp_s_ppp_ccb->ppp_phase[0].ppp_payload = *ppp_payload;
            tmp_s_ppp_ccb->ppp_phase[0].ppp_hdr = *ppp_hdr;
            tmp_s_ppp_ccb->ppp_phase[0].ppp_options = NULL;
            tmp_s_ppp_ccb->cp = 0;
            tmp_s_ppp_ccb->session_id = s_ppp_ccb->session_id;

            build_auth_ack_pap(buffer, &mulen, tmp_s_ppp_ccb);
            wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
            fastrg_mfree(tmp_s_ppp_ccb);
            FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " recv pap request.\n", s_ppp_ccb->user_num);
            return SUCCESS;
        }
    } else if (ppp_payload->ppp_protocol == rte_cpu_to_be_16(CHAP_PROTOCOL)) {
        if (s_ppp_ccb->phase != AUTH_PHASE)
            return ERROR;
        ppp_chap_data_t *ppp_chap_data = (ppp_chap_data_t *)(ppp_hdr + 1);
        if (ppp_hdr->code == CHAP_CHALLENGE) {
            U8 buffer[PPP_MSG_BUF_LEN];
            U16 mulen;
            ppp_ccb_t *tmp_s_ppp_ccb = fastrg_malloc(ppp_ccb_t, sizeof(ppp_ccb_t), 0);
            if (tmp_s_ppp_ccb == NULL) {
                FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "fastrg_malloc error.");
                return ERROR;
            }
            memset(tmp_s_ppp_ccb, 0, sizeof(ppp_ccb_t));

            s_ppp_ccb->phase = AUTH_PHASE;
            tmp_s_ppp_ccb->eth_hdr = s_ppp_ccb->eth_hdr;
            tmp_s_ppp_ccb->vlan_header = s_ppp_ccb->vlan_header;
            tmp_s_ppp_ccb->pppoe_header = s_ppp_ccb->pppoe_header;
            tmp_s_ppp_ccb->ppp_phase[0].ppp_payload = *ppp_payload;
            tmp_s_ppp_ccb->ppp_phase[0].ppp_hdr = *ppp_hdr;
            tmp_s_ppp_ccb->ppp_phase[0].ppp_options = NULL;
            tmp_s_ppp_ccb->cp = 0;
            tmp_s_ppp_ccb->session_id = s_ppp_ccb->session_id;

            build_auth_response_chap(buffer, &mulen, tmp_s_ppp_ccb, ppp_chap_data);
            wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
            FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " recv chap challenge.", s_ppp_ccb->user_num);
            fastrg_mfree(tmp_s_ppp_ccb);
            return SUCCESS;
        } else if (ppp_hdr->code == CHAP_SUCCESS) {
            FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " auth success.", s_ppp_ccb->user_num);
            s_ppp_ccb->phase = IPCP_PHASE;
            return SUCCESS;
        } else if (ppp_hdr->code == CHAP_FAILURE) {
            s_ppp_ccb->phase = LCP_PHASE;
            FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " auth fail.", s_ppp_ccb->user_num);
            return SUCCESS;
        }
    } else {
        FastRG_LOG(WARN, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " recv unknown PPP protocol.", s_ppp_ccb->user_num);
        return ERROR;
    }

    return SUCCESS;
}

STATUS PPP_decode_frame(U8 *pkt_buf, int pkt_len, U16 *event, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    if (pkt_len > ETH_JUMBO) {
        FastRG_LOG(ERR, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "error! too large frame(%d)", pkt_len);
        /* TODO: store pkt buffer to log file, not just print out */
        PRINT_MESSAGE(pkt_buf, pkt_len);
        return ERROR;
    }

    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_buf;
    vlan_header_t *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    s_ppp_ccb->eth_hdr = *eth_hdr;
    s_ppp_ccb->vlan_header = *vlan_header;
    s_ppp_ccb->pppoe_header = *pppoe_header;

    /* we receive pppoe discovery packet and dont need to parse for ppp payload */
    if (s_ppp_ccb->vlan_header.next_proto == rte_cpu_to_be_16(ETH_P_PPP_DIS)) {
        if (s_ppp_ccb->pppoe_header.code == PADS)
            s_ppp_ccb->phase = LCP_PHASE;
        pppoe_header_tag_t *pppoe_header_tag = (pppoe_header_tag_t *)(pppoe_header + 1);
        decode_pppoe(pppoe_header_tag, s_ppp_ccb);
        return ERROR;
    }

    /* we receive pppoe session packet and need to parse for ppp payload */
    if (s_ppp_ccb->vlan_header.next_proto == rte_cpu_to_be_16(ETH_P_PPP_SES)) {
        ppp_payload_t *ppp_payload = (ppp_payload_t *)(pppoe_header + 1);
        if (decode_ppp(ppp_payload, event, s_ppp_ccb) == ERROR)
            return ERROR;
    }

    return SUCCESS;
}

void codec_cleanup_ppp_ccb(ppp_ccb_t *s_ppp_ccb)
{
    if (s_ppp_ccb == NULL)
        return;

    // Clean up LCP phase options
    if (s_ppp_ccb->ppp_phase[0].ppp_options != NULL) {
        fastrg_mfree(s_ppp_ccb->ppp_phase[0].ppp_options);
        s_ppp_ccb->ppp_phase[0].ppp_options = NULL;
    }

    // Clean up IPCP phase options
    if (s_ppp_ccb->ppp_phase[1].ppp_options != NULL) {
        fastrg_mfree(s_ppp_ccb->ppp_phase[1].ppp_options);
        s_ppp_ccb->ppp_phase[1].ppp_options = NULL;
    }
}
