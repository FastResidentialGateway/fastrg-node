/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  PPP_CODEC.H

  Designed by THE on Jan 14, 2019
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _CODEC_H_
#define _CODEC_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <common.h>
#include <ip_codec.h>

#include <rte_timer.h>

#include "../fastrg.h"
#include "header.h"
#include "pppd.h"
#include "fsm.h"

/**
 * @fn PPP_decode_frame
 * 
 * @brief For decoding pppoe and ppp pkts
 * 
 * @param pkt_buf
 *      The buffer to be processed by the codec.
 * @param pkt_len
 *      The length of the buffer.
 * @param event
 *      The event to be set after decoding.
 * @param s_ppp_ccb
 *      The ppp ccb.
 * @return
 *      SUCCESS or FAILURE
 */
STATUS PPP_decode_frame(U8 *pkt_buf, int pkt_len, U16 *event, 
    ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_config_request
 *
 * @brief For build PPP configure request, either in NCP or LCP phase.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return 
 *      void
 */
void build_config_request(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_config_ack
 *
 * @brief For build PPP config ack, either in NCP or LCP phase.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return 
 *      void
 */
void build_config_ack(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_config_nak_rej
 *
 * @brief For build PPP config reject and nak, either in NCP or LCP phase.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return 
 *      void
 */
void build_config_nak_rej(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_terminate_ack
 *
 * @brief For build PPP terminate ack, either in NCP or LCP phase.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return 
 *      void
 */
void build_terminate_ack(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

STATUS build_code_reject(U8 *buffer, ppp_ccb_t *s_ppp_ccb, U16 *mulen);

/**
 * @fn build_proto_reject
 *
 * @brief Build an LCP Protocol-Reject (RFC 1661 §5.7). Sent in response to a
 *        PPP packet whose Protocol field we don't support (e.g. MPLSCP,
 *        IPV6CP) so the peer stops re-transmitting it. Protocol-Reject is
 *        stateless — caller must not drive the FSM off the rejected packet.
 *
 * @param buffer
 *      Frame buffer to fill (eth + vlan + pppoe + lcp + body).
 * @param mulen
 *      [out] Total bytes written into buffer.
 * @param s_ppp_ccb
 *      Subscriber control block — supplies MAC / VLAN / session.
 * @param rejected_proto
 *      PPP protocol number of the rejected packet (host order).
 * @param rejected_info
 *      Pointer to the rejected PPP information field
 *      (starts at the inner ppp_header_t).
 * @param rejected_info_len
 *      Bytes available at rejected_info; truncated to MRU.
 *
 * @return void
 */
void build_proto_reject(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb,
    U16 rejected_proto, const U8 *rejected_info, U16 rejected_info_len);

/**
 * @fn build_terminate_request
 *
 * @brief For build PPP terminate request, either in NCP or LCP phase.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return 
 *      void
 */
void build_terminate_request(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_echo_reply
 *
 * @brief For build PPP echo reply, only in LCP phase.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return 
 *      void
 */
void build_echo_reply(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_echo_request
 *
 * @brief Build a node-initiated LCP Echo-Request (keepalive probe) carrying our
 *        own magic number. Only valid once LCP is up.
 *
 * @param buffer
 *      The output packet buffer.
 * @param mulen
 *      The output frame length.
 * @param s_ppp_ccb
 *      The ppp ccb.
 *
 * @return
 *      void
 */
void build_echo_request(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_auth_request_pap
 *
 * @brief For PAP auth, send after LCP nego complete.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return 
 *      void
 */
void build_auth_request_pap(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_auth_ack_pap
 *
 * @brief For Spirent test center, in pap, we will receive pap request packet.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return 
 *      void
 */
void build_auth_ack_pap(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_auth_request_chap
 *
 * @brief For CHAP auth, starting after LCP nego complete.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * @param ppp_chap_data
 *      The chap data to be sent.
 * 
 * @return 
 *      void
 */
void build_auth_response_chap(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb, 
    ppp_chap_data_t *ppp_chap_data);

/**
 * @fn build_padi
 * 
 * @brief For build PPPoE init.
 *
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return SUCCESS or ERROR
 */
STATUS build_padi(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_padr
 * 
 * @brief For build PPPoE request.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return SUCCESS or ERROR
 */
STATUS build_padr(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn build_padt
 * 
 * @brief For build PPPoE termination.
 * 
 * @param buffer 
 *      The buffer to be processed by the codec.
 * @param mulen 
 *      The length of the buffer.
 * @param s_ppp_ccb 
 *      The ppp ccb.
 * 
 * @return void
 */
void build_padt(U8 *buffer, U16 *mulen, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn codec_cleanup_ppp_ccb
 * 
 * @brief Clean up all allocated memory in ppp_ccb
 * 
 * @param s_ppp_ccb 
 *      The ppp ccb to be cleaned
 * 
 * @return void
 */
void codec_cleanup_ppp_ccb(ppp_ccb_t *s_ppp_ccb);

STATUS pppoe_send_pkt(U8 encode_type, ppp_ccb_t *s_ppp_ccb);

STATUS get_ccb_id(FastRG_t *fastrg_ccb, U8 *pkt_buf, U16 *ccb_id);
int check_auth_result(ppp_ccb_t *s_ppp_ccb);

typedef enum {
    ENCODE_PADI,
    ENCODE_PADR,
    ENCODE_PADT,
}PPP_CODE_TYPE_t;

#ifdef __cplusplus
}
#endif

#endif
