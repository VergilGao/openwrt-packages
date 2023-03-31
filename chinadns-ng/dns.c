#define _GNU_SOURCE
#include "dns.h"
#include "net.h"
#include "log.h"
#include "misc.h"
#include <stddef.h>
#include <string.h>

/* "\3www\6google\3com\0" => "www.google.com" */
static bool decode_name(char *noalias out, const char *noalias src, int len) {
    /* root domain ? */
    if (len <= DNS_NAME_ENC_MINLEN) {
        out[0] = '.';
        out[1] = '\0';
        return true;
    }

    /* ignore last byte: src="\3www\6google\3com" */
    /* ignore first byte: out="www\6google\3com\0" */
    memcpy(out, src + 1, --len);

    /* foreach label (len:1byte | label) */
    for (int first = 1; len >= 2;) {
        if (first) first = 0; else *out++ = '.';
        int label_len = *(const ubyte *)src++; --len;
        unlikely_if (label_len < 1) {
            LOGE("label length is too short: %d", label_len);
            return false;
        }
        unlikely_if (label_len > DNS_NAME_LABEL_MAXLEN) {
            LOGE("label length is too long: %d", label_len);
            return false;
        }
        unlikely_if (label_len > len) {
            LOGE("label length is greater than remaining length: %d > %d", label_len, len);
            return false;
        }
        src += label_len;
        len -= label_len;
        out += label_len;
    }

    unlikely_if (len != 0) {
        LOGE("name format error, remaining length: %d", len);
        return false;
    }

    return true;
}

/* check dns packet */
static bool check_packet(bool is_query,
    const void *noalias packet_buf, ssize_t packet_len,
    char *noalias name_buf, int *noalias p_namelen)
{
    /* check packet length */
    unlikely_if (packet_len < (ssize_t)DNS_PACKET_MINSIZE) {
        LOGE("dns packet is too short: %zd", packet_len);
        return false;
    }
    unlikely_if (packet_len > DNS_PACKET_MAXSIZE) {
        LOGE("dns packet is too long: %zd", packet_len);
        return false;
    }

    /* check header */
    const dns_header_t *header = packet_buf;
    unlikely_if (header->qr != (is_query ? DNS_QR_QUERY : DNS_QR_REPLY)) {
        LOGE("this is a %s packet, but header->qr is %u", is_query ? "query" : "reply", (uint)header->qr);
        return false;
    }
    unlikely_if (header->opcode != DNS_OPCODE_QUERY) {
        LOGE("this is not a standard query, opcode: %u", (uint)header->opcode);
        return false;
    }
    unlikely_if (ntohs(header->question_count) != 1) {
        LOGE("there should be one and only one question section: %u", (uint)ntohs(header->question_count));
        return false;
    }

    /* move to question section (name + dns_query_t) */
    packet_buf += sizeof(dns_header_t);
    packet_len -= sizeof(dns_header_t);

    /* search the queried domain name */
    /* encoded name: "\3www\6google\3com\0" */
    const void *p = memchr(packet_buf, 0, packet_len);
    unlikely_if (!p) {
        LOGE("format error: domain name end byte not found");
        return false;
    }

    /* check name length */
    const int namelen = p + 1 - packet_buf;
    unlikely_if (namelen < DNS_NAME_ENC_MINLEN) {
        LOGE("encoded domain name is too short: %d", namelen);
        return false;
    }
    unlikely_if (namelen > DNS_NAME_ENC_MAXLEN) {
        LOGE("encoded domain name is too long: %d", namelen);
        return false;
    }

    /* decode to ASCII format */
    if (name_buf) {
        unlikely_if (!decode_name(name_buf, packet_buf, namelen))
            return false;
    }
    if (p_namelen)
        *p_namelen = namelen;

    /* move to dns_query_t pos */
    packet_buf += namelen;
    packet_len -= namelen;

    /* check remaining length */
    unlikely_if (packet_len < (ssize_t)sizeof(dns_query_t)) {
        LOGE("remaining length is less than sizeof(dns_query_t): %zd < %zu", packet_len, sizeof(dns_query_t));
        return false;
    }

    /* check query class */
    const dns_query_t *query_ptr = packet_buf;
    unlikely_if (ntohs(query_ptr->qclass) != DNS_CLASS_INTERNET) {
        LOGE("only supports standard internet query class: %u", (uint)ntohs(query_ptr->qclass));
        return false;
    }

    return true;
}

/*          \0 => root domain */
/*      \2cn\0 => normal domain */
/*     [ptr:2] => fully compress */
/* \2cn[ptr:2] => partial compress */
static bool skip_name(const void *noalias *noalias p_ptr, ssize_t *noalias p_len) {
    const void *noalias ptr = *p_ptr;
    ssize_t len = *p_len;

    while (len > 0) {
        int label_len = *(const ubyte *)ptr;
        if (label_len == 0) {
            ++ptr;
            --len;
            break;
        } else if (label_len >= DNS_NAME_PTR_MINVAL) {
            ptr += 2;
            len -= 2;
            break;
        } else if (label_len <= DNS_NAME_LABEL_MAXLEN) {
            ptr += 1 + label_len;
            len -= 1 + label_len;
        } else {
            LOGE("label length is too long: %d", label_len);
            return false;
        }
    }

    unlikely_if (len < (ssize_t)sizeof(dns_record_t)) {
        LOGE("remaining length is less than sizeof(dns_record_t): %zd < %zu", len, sizeof(dns_record_t));
        return false;
    }

    *p_ptr = ptr;
    *p_len = len;
    return true;
}

int dns_chnip_check(const void *noalias packet_buf, ssize_t packet_len, int namelen) {
    const dns_header_t *h = packet_buf;
    uint16_t answer_count = ntohs(h->answer_count);

    /* move to answer section */
    packet_buf += sizeof(dns_header_t) + namelen + sizeof(dns_query_t);
    packet_len -= sizeof(dns_header_t) + namelen + sizeof(dns_query_t);

    /* find the first A/AAAA record */
    for (uint16_t i = 0; i < answer_count; ++i) {
        unlikely_if (!skip_name(&packet_buf, &packet_len))
            return DNS_IPCHK_BAD_PACKET;

        const dns_record_t *record = packet_buf;
        unlikely_if (ntohs(record->rclass) != DNS_CLASS_INTERNET) {
            LOGE("only supports standard internet query class: %u", (uint)ntohs(record->rclass));
            return DNS_IPCHK_BAD_PACKET;
        }

        uint16_t rdatalen = ntohs(record->rdatalen);
        ssize_t recordlen = sizeof(dns_record_t) + rdatalen;
        unlikely_if (packet_len < recordlen) {
            LOGE("remaining length is less than sizeof(record): %zd < %zd", packet_len, recordlen);
            return DNS_IPCHK_BAD_PACKET;
        }

        switch (ntohs(record->rtype)) {
            case DNS_RECORD_TYPE_A:
                unlikely_if (rdatalen != IPV4_BINADDR_LEN) {
                    LOGE("rdatalen is not equal to sizeof(ipv4): %u != %d", (uint)rdatalen, IPV4_BINADDR_LEN);
                    return DNS_IPCHK_BAD_PACKET;
                }
                return ipset_addr_is_exists(record->rdata, true) ? DNS_IPCHK_IS_CHNIP : DNS_IPCHK_NOT_CHNIP; /* in chnroute ? */
            case DNS_RECORD_TYPE_AAAA:
                unlikely_if (rdatalen != IPV6_BINADDR_LEN) {
                    LOGE("rdatalen is not equal to sizeof(ipv6): %u != %d", (uint)rdatalen, IPV6_BINADDR_LEN);
                    return DNS_IPCHK_BAD_PACKET;
                }
                return ipset_addr_is_exists(record->rdata, false) ? DNS_IPCHK_IS_CHNIP : DNS_IPCHK_NOT_CHNIP; /* in chnroute6 ? */
        }

        packet_buf += recordlen;
        packet_len -= recordlen;
    }

    /* not found A/AAAA record */
    return DNS_IPCHK_NOT_FOUND;
}

bool dns_query_check(const void *noalias packet_buf, ssize_t packet_len, char *noalias name_buf, int *noalias p_namelen) {
    return check_packet(true, packet_buf, packet_len, name_buf, p_namelen);
}

bool dns_reply_check(const void *noalias packet_buf, ssize_t packet_len, char *noalias name_buf, int *noalias p_namelen) {
    return check_packet(false, packet_buf, packet_len, name_buf, p_namelen);
}
