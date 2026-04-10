/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DNS_CODEC.H

  DNS protocol codec for FastRG DNS proxy.
  Parse DNS queries, build DNS responses, handle label compression.

  Designed by THE on Apr 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DNS_CODEC_H_
#define _DNS_CODEC_H_

#include <common.h>

#define DNS_PORT                53

#define DNS_MAX_DOMAIN_LEN      253
#define DNS_MAX_LABEL_LEN       63
#define DNS_MAX_PACKET_LEN      512   /* standard UDP DNS */
#define DNS_TCP_MAX_PACKET_LEN  65535

#define DNS_HDR_LEN             12
#define DNS_MAX_ANSWERS         16

/* DNS header flags */
#define DNS_FLAG_QR             0x8000  /* query/response */
#define DNS_FLAG_AA             0x0400  /* authoritative answer */
#define DNS_FLAG_TC             0x0200  /* truncated */
#define DNS_FLAG_RD             0x0100  /* recursion desired */
#define DNS_FLAG_RA             0x0080  /* recursion available */

/* DNS opcodes (bits 11-14) */
#define DNS_OPCODE_QUERY        0
#define DNS_OPCODE_IQUERY       1
#define DNS_OPCODE_STATUS       2

/* DNS response codes (bits 0-3 of flags) */
#define DNS_RCODE_OK            0
#define DNS_RCODE_FORMERR       1
#define DNS_RCODE_SERVFAIL      2
#define DNS_RCODE_NXDOMAIN      3
#define DNS_RCODE_NOTIMP        4
#define DNS_RCODE_REFUSED       5

/* DNS record types */
#define DNS_TYPE_A              1
#define DNS_TYPE_NS             2
#define DNS_TYPE_CNAME          5
#define DNS_TYPE_SOA            6
#define DNS_TYPE_PTR            12
#define DNS_TYPE_MX             15
#define DNS_TYPE_TXT            16
#define DNS_TYPE_AAAA           28
#define DNS_TYPE_SRV            33
#define DNS_TYPE_ANY            255

/* DNS classes */
#define DNS_CLASS_IN            1

/* DNS header (wire format, big-endian) */
typedef struct __attribute__((packed)) dns_header {
    U16 id;
    U16 flags;
    U16 qdcount;    /* question count */
    U16 ancount;    /* answer count */
    U16 nscount;    /* authority count */
    U16 arcount;    /* additional count */
} dns_header_t;

/* Parsed DNS question */
typedef struct dns_question {
    char    name[DNS_MAX_DOMAIN_LEN + 1];
    U16     qtype;
    U16     qclass;
    U16     name_wire_len;  /* length of name in wire format (for building responses) */
} dns_question_t;

/* Parsed DNS resource record (answer/authority/additional) */
typedef struct dns_rr {
    char    name[DNS_MAX_DOMAIN_LEN + 1];
    U16     type;
    U16     rr_class;
    U32     ttl;
    U16     rdlength;
    U8      rdata[256];
} dns_rr_t;

/* Parsed DNS message */
typedef struct dns_message {
    dns_header_t    header;
    dns_question_t  question;       /* we only handle single-question queries */
    dns_rr_t        answers[DNS_MAX_ANSWERS];
    U16             answer_count;
    U32             min_ttl;        /* minimum TTL from all answer RRs */
} dns_message_t;

/**
 * @fn dns_parse_query
 *
 * @brief Parse a DNS query from wire format
 * @param data
 *      Pointer to DNS packet data (after UDP/TCP header)
 * @param data_len
 *      Length of DNS data
 * @param msg
 *      Output parsed DNS message
 * @return SUCCESS on success, ERROR on parse failure
 */
STATUS dns_parse_query(const U8 *data, U16 data_len, dns_message_t *msg);

/**
 * @fn dns_parse_response
 *
 * @brief Parse a DNS response from wire format (extracts answers for caching)
 * @param data
 *      Pointer to DNS packet data
 * @param data_len
 *      Length of DNS data
 * @param msg
 *      Output parsed DNS message
 * @return SUCCESS on success, ERROR on parse failure
 */
STATUS dns_parse_response(const U8 *data, U16 data_len, dns_message_t *msg);

/**
 * @fn dns_build_response_a
 *
 * @brief Build a DNS response for a single A record
 * @param query_data
 *      Original query data (to copy question section)
 * @param query_len
 *      Length of original query
 * @param ip_addr
 *      IPv4 address for the answer (network byte order)
 * @param ttl
 *      TTL for the answer
 * @param resp_buf
 *      Output buffer for response
 * @param resp_buf_len
 *      Size of output buffer
 * @return Length of built response, or 0 on error
 */
U16 dns_build_response_a(const U8 *query_data, U16 query_len,
    U32 ip_addr, U32 ttl, U8 *resp_buf, U16 resp_buf_len);

/**
 * @fn dns_build_response_nxdomain
 *
 * @brief Build an NXDOMAIN response for a DNS query
 * @param query_data
 *      Original query data
 * @param query_len
 *      Length of original query
 * @param resp_buf
 *      Output buffer for response
 * @param resp_buf_len
 *      Size of output buffer
 * @return Length of built response, or 0 on error
 */
U16 dns_build_response_nxdomain(const U8 *query_data, U16 query_len,
    U8 *resp_buf, U16 resp_buf_len);

/**
 * @fn dns_build_response_servfail
 *
 * @brief Build a SERVFAIL response for a DNS query
 * @param query_data
 *      Original query data
 * @param query_len
 *      Length of original query
 * @param resp_buf
 *      Output buffer for response
 * @param resp_buf_len
 *      Size of output buffer
 * @return Length of built response, or 0 on error
 */
U16 dns_build_response_servfail(const U8 *query_data, U16 query_len,
    U8 *resp_buf, U16 resp_buf_len);

/**
 * @fn dns_domain_to_lower
 *
 * @brief Convert domain name to lowercase in-place
 * @param domain
 *      Domain name string
 */
void dns_domain_to_lower(char *domain);

/**
 * @fn dns_parse_name
 *
 * @brief Parse a DNS name from wire format (handles compression pointers)
 * @param pkt
 *      Start of DNS packet
 * @param pkt_len
 *      Total packet length
 * @param offset
 *      Current offset into packet
 * @param name_buf
 *      Output buffer for domain name (dot-separated)
 * @param name_max
 *      Size of name buffer
 * @return Number of bytes consumed from wire at offset, or 0 on error
 */
U16 dns_parse_name(const U8 *pkt, U16 pkt_len, U16 offset,
    char *name_buf, U16 name_max);

#endif
