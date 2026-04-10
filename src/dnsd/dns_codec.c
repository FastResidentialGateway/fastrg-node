/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DNS_CODEC.C

  DNS protocol codec implementation.

  Designed by THE on Apr 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <ctype.h>
#include <string.h>

#include "dns_codec.h"

void dns_domain_to_lower(char *domain)
{
    for(char *p=domain; *p; p++)
        *p = (char)tolower((unsigned char)*p);
}

/**
 * Parse a DNS domain name from wire format with label compression support.
 * Returns the number of bytes consumed at `offset` (the wire size before any
 * pointer indirection). Writes a dot-separated name into name_buf.
 */
U16 dns_parse_name(const U8 *pkt, U16 pkt_len, U16 offset,
    char *name_buf, U16 name_max)
{
    U16 consumed = 0;
    U16 pos = offset;
    U16 out = 0;
    BOOL jumped = FALSE;
    int jump_count = 0;

    if (name_max == 0)
        return 0;

    name_buf[0] = '\0';

    while (pos < pkt_len) {
        U8 label_len = pkt[pos];

        if (label_len == 0) {
            if (!jumped)
                consumed = (pos - offset) + 1;
            break;
        }

        /* compression pointer (top 2 bits = 11) */
        if ((label_len & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt_len)
                return 0;
            if (!jumped)
                consumed = (pos - offset) + 2;
            U16 ptr = ((U16)(label_len & 0x3F) << 8) | pkt[pos + 1];
            if (ptr >= pkt_len)
                return 0;
            pos = ptr;
            jumped = TRUE;
            if (++jump_count > 128)
                return 0;  /* loop protection */
            continue;
        }

        /* normal label */
        if (label_len > DNS_MAX_LABEL_LEN)
            return 0;
        pos++;
        if (pos + label_len > pkt_len)
            return 0;

        /* add dot separator */
        if (out > 0) {
            if (out + 1 >= name_max)
                return 0;
            name_buf[out++] = '.';
        }

        if (out + label_len >= name_max)
            return 0;

        memcpy(name_buf + out, pkt + pos, label_len);
        out += label_len;
        pos += label_len;

        if (!jumped)
            consumed = pos - offset;
    }

    name_buf[out] = '\0';
    return consumed;
}

/**
 * Parse question section (single question).
 * Returns offset past the question section.
 */
static U16 parse_question(const U8 *pkt, U16 pkt_len, U16 offset,
    dns_question_t *question)
{
    U16 name_consumed = dns_parse_name(pkt, pkt_len, offset,
        question->name, sizeof(question->name));
    if (name_consumed == 0)
        return 0;

    question->name_wire_len = name_consumed;
    U16 pos = offset + name_consumed;

    if (pos + 4 > pkt_len)
        return 0;

    question->qtype = (U16)((pkt[pos] << 8) | pkt[pos + 1]);
    question->qclass = (U16)((pkt[pos + 2] << 8) | pkt[pos + 3]);

    dns_domain_to_lower(question->name);

    return pos + 4;
}

/**
 * Parse resource records (answers section).
 */
static U16 parse_rr_section(const U8 *pkt, U16 pkt_len, U16 offset,
    dns_rr_t *rrs, U16 max_rrs, U16 count, U32 *min_ttl)
{
    U16 pos = offset;
    U16 parsed = 0;

    for(U16 i=0; i<count && pos<pkt_len; i++) {
        dns_rr_t rr;
        memset(&rr, 0, sizeof(rr));

        U16 name_consumed = dns_parse_name(pkt, pkt_len, pos,
            rr.name, sizeof(rr.name));
        if (name_consumed == 0)
            return 0;
        pos += name_consumed;

        if (pos + 10 > pkt_len)
            return 0;

        rr.type = (U16)((pkt[pos] << 8) | pkt[pos + 1]);
        rr.rr_class = (U16)((pkt[pos + 2] << 8) | pkt[pos + 3]);
        rr.ttl = ((U32)pkt[pos + 4] << 24) | ((U32)pkt[pos + 5] << 16) |
                 ((U32)pkt[pos + 6] << 8) | (U32)pkt[pos + 7];
        rr.rdlength = (U16)((pkt[pos + 8] << 8) | pkt[pos + 9]);
        pos += 10;

        if (pos + rr.rdlength > pkt_len)
            return 0;

        if (rr.rdlength <= sizeof(rr.rdata))
            memcpy(rr.rdata, pkt + pos, rr.rdlength);
        pos += rr.rdlength;

        /* track minimum TTL */
        if (rr.ttl > 0 && (rr.ttl < *min_ttl || *min_ttl == 0))
            *min_ttl = rr.ttl;

        dns_domain_to_lower(rr.name);

        if (parsed < max_rrs)
            rrs[parsed] = rr;
        parsed++;
    }

    return pos;
}

STATUS dns_parse_query(const U8 *data, U16 data_len, dns_message_t *msg)
{
    if (data == NULL || msg == NULL || data_len < DNS_HDR_LEN)
        return ERROR;

    memset(msg, 0, sizeof(*msg));

    /* parse header */
    msg->header.id = (U16)((data[0] << 8) | data[1]);
    msg->header.flags = (U16)((data[2] << 8) | data[3]);
    msg->header.qdcount = (U16)((data[4] << 8) | data[5]);
    msg->header.ancount = (U16)((data[6] << 8) | data[7]);
    msg->header.nscount = (U16)((data[8] << 8) | data[9]);
    msg->header.arcount = (U16)((data[10] << 8) | data[11]);

    /* must be a query (QR=0) */
    if (msg->header.flags & DNS_FLAG_QR)
        return ERROR;

    /* we only handle single-question queries */
    if (msg->header.qdcount != 1)
        return ERROR;

    U16 end = parse_question(data, data_len, DNS_HDR_LEN, &msg->question);
    if (end == 0)
        return ERROR;

    return SUCCESS;
}

STATUS dns_parse_response(const U8 *data, U16 data_len, dns_message_t *msg)
{
    if (data == NULL || msg == NULL || data_len < DNS_HDR_LEN)
        return ERROR;

    memset(msg, 0, sizeof(*msg));

    /* parse header */
    msg->header.id = (U16)((data[0] << 8) | data[1]);
    msg->header.flags = (U16)((data[2] << 8) | data[3]);
    msg->header.qdcount = (U16)((data[4] << 8) | data[5]);
    msg->header.ancount = (U16)((data[6] << 8) | data[7]);
    msg->header.nscount = (U16)((data[8] << 8) | data[9]);
    msg->header.arcount = (U16)((data[10] << 8) | data[11]);

    /* must be a response (QR=1) */
    if (!(msg->header.flags & DNS_FLAG_QR))
        return ERROR;

    U16 pos = DNS_HDR_LEN;

    /* skip question section */
    for(U16 i=0; i<msg->header.qdcount; i++) {
        if (i == 0) {
            pos = parse_question(data, data_len, pos, &msg->question);
        } else {
            dns_question_t dummy;
            pos = parse_question(data, data_len, pos, &dummy);
        }
        if (pos == 0)
            return ERROR;
    }

    /* parse answers */
    msg->min_ttl = 0;
    if (msg->header.ancount > 0) {
        U16 end = parse_rr_section(data, data_len, pos,
            msg->answers, DNS_MAX_ANSWERS, msg->header.ancount,
            &msg->min_ttl);
        if (end == 0)
            return ERROR;
        msg->answer_count = (msg->header.ancount > DNS_MAX_ANSWERS) ?
            DNS_MAX_ANSWERS : msg->header.ancount;
    }

    return SUCCESS;
}

/**
 * Build a DNS response by copying the query and modifying flags + appending answer.
 */
static U16 build_response_base(const U8 *query_data, U16 query_len,
    U16 rcode, U8 *resp_buf, U16 resp_buf_len)
{
    if (query_len < DNS_HDR_LEN || resp_buf_len < query_len)
        return 0;

    memcpy(resp_buf, query_data, query_len);

    /* set QR=1, RA=1, keep RD, set rcode */
    U16 flags = (U16)((query_data[2] << 8) | query_data[3]);
    flags |= DNS_FLAG_QR | DNS_FLAG_RA;
    flags = (flags & 0xFFF0) | (rcode & 0x0F);
    resp_buf[2] = (U8)(flags >> 8);
    resp_buf[3] = (U8)(flags & 0xFF);

    return query_len;
}

U16 dns_build_response_a(const U8 *query_data, U16 query_len,
    U32 ip_addr, U32 ttl, U8 *resp_buf, U16 resp_buf_len)
{
    /* need room for query + answer RR (2 name ptr + 2 type + 2 class + 4 ttl + 2 rdlen + 4 rdata = 16) */
    if (resp_buf_len < query_len + 16)
        return 0;

    U16 len = build_response_base(query_data, query_len, DNS_RCODE_OK,
        resp_buf, resp_buf_len);
    if (len == 0)
        return 0;

    /* set ancount = 1 */
    resp_buf[6] = 0;
    resp_buf[7] = 1;
    /* clear nscount and arcount */
    resp_buf[8] = 0;
    resp_buf[9] = 0;
    resp_buf[10] = 0;
    resp_buf[11] = 0;

    /* find end of question section */
    U16 pos = DNS_HDR_LEN;
    dns_question_t q;
    U16 qend = parse_question(query_data, query_len, pos, &q);
    if (qend == 0)
        return 0;

    /* overwrite past question (discard any extra data from query) */
    pos = qend;
    if (pos + 16 > resp_buf_len)
        return 0;

    /* answer: name pointer to question name at offset 12 */
    resp_buf[pos++] = 0xC0;
    resp_buf[pos++] = 0x0C;

    /* type A */
    resp_buf[pos++] = 0;
    resp_buf[pos++] = DNS_TYPE_A;

    /* class IN */
    resp_buf[pos++] = 0;
    resp_buf[pos++] = DNS_CLASS_IN;

    /* TTL */
    resp_buf[pos++] = (U8)(ttl >> 24);
    resp_buf[pos++] = (U8)(ttl >> 16);
    resp_buf[pos++] = (U8)(ttl >> 8);
    resp_buf[pos++] = (U8)(ttl);

    /* rdlength = 4 */
    resp_buf[pos++] = 0;
    resp_buf[pos++] = 4;

    /* IPv4 address (already in network byte order) */
    memcpy(resp_buf + pos, &ip_addr, 4);
    pos += 4;

    return pos;
}

U16 dns_build_response_nxdomain(const U8 *query_data, U16 query_len,
    U8 *resp_buf, U16 resp_buf_len)
{
    U16 len = build_response_base(query_data, query_len, DNS_RCODE_NXDOMAIN,
        resp_buf, resp_buf_len);
    if (len == 0)
        return 0;

    /* clear answer/authority/additional counts */
    resp_buf[6] = 0;
    resp_buf[7] = 0;
    resp_buf[8] = 0;
    resp_buf[9] = 0;
    resp_buf[10] = 0;
    resp_buf[11] = 0;

    /* truncate to just header + question */
    dns_question_t q;
    U16 qend = parse_question(query_data, query_len, DNS_HDR_LEN, &q);
    if (qend == 0)
        return 0;

    return qend;
}

U16 dns_build_response_servfail(const U8 *query_data, U16 query_len,
    U8 *resp_buf, U16 resp_buf_len)
{
    U16 len = build_response_base(query_data, query_len, DNS_RCODE_SERVFAIL,
        resp_buf, resp_buf_len);
    if (len == 0)
        return 0;

    /* clear answer/authority/additional counts */
    resp_buf[6] = 0;
    resp_buf[7] = 0;
    resp_buf[8] = 0;
    resp_buf[9] = 0;
    resp_buf[10] = 0;
    resp_buf[11] = 0;

    dns_question_t q;
    U16 qend = parse_question(query_data, query_len, DNS_HDR_LEN, &q);
    if (qend == 0)
        return 0;

    return qend;
}
