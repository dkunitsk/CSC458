/**********************************************************************
 * file:  sr_router.c 
 * date:  Mon Feb 18 12:50:42 PST 2002  
 * Contact: casado@stanford.edu 
 *
 * Description:
 * 
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <glib.h>
#include <unistd.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_dumper.h"

/* Macros to convert byte order */
#define HOST_SHORT(PTR) (PTR = ntohs(PTR))
#define HOST_LONG(PTR)  (PTR = ntohl(PTR))

#define NET_SHORT(PTR)  (PTR = htons(PTR))
#define NET_LONG(PTR)   (PTR = htonl(PTR))

/* Offset of IPv4 Checksum */
#define IPv4_CHECKSUM_OFFSET 10
#define IPv4_CHECKSUM_LENGTH 2
#define IPv4_WORD_SIZE 4
#define IPV4_TTL 64

#define ICMP_CHECKSUM_OFFSET 2
#define ICMP_CHECKSUM_LENGTH 2

/* Forward declarations */
void sr_handleproto_ARP(struct sr_instance *sr, /* Native byte order */
        struct sr_if *eth_if, /* Network byte order */
        struct sr_ethernet_hdr *eth_hdr, /* Network byte order */
        struct sr_arphdr *arp_hdr /* Network byte order */);
void sr_handleproto_IP(struct sr_instance *sr, /* Native byte order */
        struct sr_if *eth_if, /* Network byte order */
        struct sr_ethernet_hdr *eth_hdr, /* Network byte order */
        struct ip *ip_hdr /* Network byte order */);
void populate_ethernet_header(uint8_t *buf, uint8_t *eth_shost, uint8_t *eth_dhost, uint16_t ether_type);
uint16_t header_checksum(uint8_t *buf, uint16_t len, uint16_t cksum_offset, uint16_t cksum_length);
uint16_t IP_header_checksum(struct ip *ip_hdr);
uint16_t ICMP_header_checksum(struct icmp_hdr *icmp_hdr, uint16_t icmp_len);
void populate_ip_header(uint8_t *buf, uint16_t data_len, uint8_t proto, struct in_addr src, struct in_addr dst);

/*--------------------------------------------------------------------- 
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 * 
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr) 
{
    /* REQUIRES */
    assert(sr);

    /* Add initialization code here! */

} /* -- sr_init -- */



/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr, 
        uint8_t * packet/* lent */,
        unsigned int len,
        char* interface/* lent */)
{
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(interface);

    printf("*** -> Received packet of length %d \n", len);

    /* Ethernet Interface */
    struct sr_if *eth_if = (struct sr_if *) sr_get_interface(sr, interface);

    if(eth_if) {
        printf("Interface: %s \n", eth_if->name);
    } else {
        printf("!!! Invalid Interface: %s \n", interface);
    }

    /* Ethernet Header */
    struct sr_ethernet_hdr *eth_hdr = (struct sr_ethernet_hdr *) packet;

    switch(ntohs(eth_hdr->ether_type)) {
        case ETHERTYPE_ARP:
            printf("Protocol: ARP. \n");

            /* Cast to ARP header by indexing into packet */
            struct sr_arphdr *arp_hdr = (struct sr_arphdr *)(packet + sizeof(struct sr_ethernet_hdr));

            sr_handleproto_ARP(sr, eth_if, eth_hdr, arp_hdr);
            break;

        case ETHERTYPE_IP:
            printf("Protocol: IP. \n");

            /* Cast to IP Header by indexing into packet */
            struct ip *ip_hdr = (struct ip *)(packet + sizeof(struct sr_ethernet_hdr));

            sr_handleproto_IP(sr, eth_if, eth_hdr, ip_hdr);
            break;
        default:
            printf("!!! Unrecognized Protocol Type: %d \n", eth_hdr->ether_type);
    }
}/* end sr_handlpacket */

/*
 * Handle ARP protocol packets
 */
void sr_handleproto_ARP(struct sr_instance *sr, /* Native byte order */
        struct sr_if *eth_if, /* Network byte order */
        struct sr_ethernet_hdr *eth_hdr, /* Network byte order */
        struct sr_arphdr *arp_hdr /* Network byte order */)
{
    assert(sr);
    assert(eth_if);
    assert(eth_hdr);
    assert(arp_hdr);

    /* Only handle ARP packets for Ethernet */
    switch(ntohs(arp_hdr->ar_hrd)) {
        case ARPHDR_ETHER:
            break;

        default:
            printf("Only handle ARP Packets for Ethernet. \n");
            return;
    }

    switch(ntohs(arp_hdr->ar_op)) {
        case ARP_REQUEST: /* Handle ARP Request */
            printf("ARP Operation: Request \n");

            /* If Request is for Router's IP then send a reply */
            struct sr_if *t_eth_if = sr_get_interface_for_ip(sr, arp_hdr->ar_tip);
            if(t_eth_if != 0) {
                unsigned int len = sizeof(struct sr_ethernet_hdr) + sizeof(struct sr_arphdr);
                uint8_t *buf = malloc(len);

                /* Populate reply Ethernet header */
                populate_ethernet_header(buf, t_eth_if->addr, eth_hdr->ether_shost, ETHERTYPE_ARP);

                /*
                 * Populate ARP reply, copy over source arp header since most
                 * of the fields are going to be the same
                 */
                struct sr_arphdr *rep_arp_hdr = (struct sr_arphdr *) (buf + sizeof(struct sr_ethernet_hdr));
                memcpy(rep_arp_hdr, arp_hdr, sizeof(struct sr_arphdr));

                /* Mark packet as reply*/
                rep_arp_hdr->ar_op = htons(ARP_REPLY);
                /* Sender in Request is Target in reply */
                memcpy(rep_arp_hdr->ar_tha, arp_hdr->ar_sha, sizeof(arp_hdr->ar_sha));
                rep_arp_hdr->ar_tip = arp_hdr->ar_sip;
                /* Sender in reply is our own address */
                memcpy(rep_arp_hdr->ar_sha, t_eth_if->addr, sizeof(t_eth_if->addr));
                rep_arp_hdr->ar_sip = t_eth_if->ip;

                /* Send packet and free buffer */
                int result = sr_send_packet(sr, buf, len, t_eth_if->name);
                free(buf);
                if(result == 0) {
                    printf("*** -> Sent Packet of length: %d \n", len);
                }
            }

            break;

        case ARP_REPLY: /* Handle ARP Reply */
            break;

        default:
            printf("Only handle ARP Request and Reply. \n");
    }
}

/* Handle IP Packets */
void sr_handleproto_IP(struct sr_instance *sr, /* Native byte order */
        struct sr_if *eth_if, /* Network byte order */
        struct sr_ethernet_hdr *eth_hdr, /* Network byte order */
        struct ip *ip_hdr /* Network byte order */)
{
    assert(sr);
    assert(eth_if);
    assert(eth_hdr);
    assert(ip_hdr);

    /* Check IP Protocol Version */
    switch(ip_hdr->ip_v) {
        case IPv4:
            break;

        default:
            printf("Only handle IP Version 4. \n");
            return;
    }

    /* Validate Checksum */
    if(ntohs(ip_hdr->ip_sum) != IP_header_checksum(ip_hdr)) {
        printf("!!! Invalid checksum. \n");
        return;
    }

    /* Handle packet if Router is destination */
    if(sr_get_interface_for_ip(sr, ip_hdr->ip_dst.s_addr) != 0) {
        switch(ip_hdr->ip_p) {
            case IPPROTO_ICMP:
            {
                printf("IP Protocol: ICMP \n");

                struct icmp_hdr *icmp_hdr = (struct icmp_hdr *)(((uint8_t *)ip_hdr) + ip_hdr->ip_hl * IPv4_WORD_SIZE);
                uint16_t icmp_len = ntohs(ip_hdr->ip_len) - ip_hdr->ip_hl * IPv4_WORD_SIZE;

                /* Validate Checksum */
                if(ntohs(icmp_hdr->ic_sum) != ICMP_header_checksum(icmp_hdr, icmp_len)) {
                    printf("!!! Invalid checksum. \n");
                    return;
                }

                switch(icmp_hdr->ic_type) {
                    case ICMP_ECHO_REQUEST:
                        printf("ICMP Message Type: Echo Request \n");

                        int len = sizeof(struct sr_ethernet_hdr) + sizeof(struct ip) + icmp_len;
                        uint8_t *buf = (uint8_t *)malloc(len);

                        /* Populate ICMP Header */
                        struct icmp_hdr *rep_icmp_hdr =
                                (struct icmp_hdr *)(buf + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip));

                        /* Copy existing packet since most of the info is the same */
                        memcpy(rep_icmp_hdr, icmp_hdr, icmp_len);

                        /* Update Type */
                        rep_icmp_hdr->ic_type = ICMP_ECHO_REPLY;

                        /* Calculate ICMP Checksum */
                        rep_icmp_hdr->ic_sum = htons(ICMP_header_checksum(icmp_hdr, icmp_len));

                        /* Populate IP Header */
                        populate_ip_header(
                                buf + sizeof(struct sr_ethernet_hdr),
                                icmp_len,
                                IPPROTO_ICMP,
                                ip_hdr->ip_dst,
                                ip_hdr->ip_src);

                        /* Populate Ethernet Header */
                        populate_ethernet_header(buf, eth_hdr->ether_dhost, eth_hdr->ether_shost, ETHERTYPE_IP);

                        /* Send packet and free buffer */
                        int result = sr_send_packet(sr, buf, len, eth_if->name);
                        free(buf);
                        if(result == 0) {
                            printf("*** -> Sent Packet of length: %d \n", len);
                        }

                        return;

                    default:
                        printf("Unhandled ICMP Message Type. \n");
                }

                return;
            }

            case IPPROTO_TCP:
            case IPPROTO_UDP:
            {
                printf("IP Protocol: TCP/UDP. \n");
                printf("Sending 'ICMP: Port Unreachable' to Source. \n");

                int ip_len = (ip_hdr->ip_hl * IPv4_WORD_SIZE)
                        + min(8, ip_hdr->ip_len - ip_hdr->ip_hl * IPv4_WORD_SIZE);
                int icmp_len = sizeof(struct icmp_hdr)
                        + sizeof(struct icmp_echo_hdr)
                        + ip_len;
                int len = sizeof(struct sr_ethernet_hdr)
                        + sizeof(struct ip) + icmp_len;

                uint8_t *buf = malloc(len);

                /* Populate ICMP Header and Data*/
                struct icmp_hdr *rep_icmp_hdr =
                        (struct icmp_hdr *)(buf + sizeof(struct sr_ethernet_hdr) + sizeof(struct ip));

                /* Copy old IP Header + 8 Data bytes into ICMP Data section */
                memcpy(((uint8_t *)rep_icmp_hdr)
                        + sizeof(struct icmp_hdr) + sizeof(struct icmp_echo_hdr),
                        (uint8_t *)ip_hdr,
                        ip_len);

                /* Set ICMP Header Type and Checksum */
                rep_icmp_hdr->ic_type = ICMP_DESTINATION_UNREACHABLE;
                rep_icmp_hdr->ic_code = ICMP_CODE_PORT_UNREACHABLE;
                rep_icmp_hdr->ic_sum = htons(ICMP_header_checksum(rep_icmp_hdr, icmp_len));

                /* Populate IP Header */
                populate_ip_header(buf + sizeof(struct sr_ethernet_hdr),
                        icmp_len,
                        IPPROTO_ICMP,
                        ip_hdr->ip_dst,
                        ip_hdr->ip_src);

                /* Populate Ethernet Header */
                populate_ethernet_header(buf, eth_hdr->ether_dhost, eth_hdr->ether_shost, ETHERTYPE_IP);

                /* Send packet and free buffer */
                int result = sr_send_packet(sr, buf, len, eth_if->name);
                free(buf);
                if(result == 0) {
                    printf("*** -> Sent Packet of length: %d \n", len);
                }

                return;
            }

            default:
                printf("!!! Unhandled IP Protocol. \n");
        }

        return;
    }

    /* Else, forward packet after lookup in Routing Table */
}

/*
 * Populate allocated buffer *buf to be sent to destination eth_dhost
 * from eth_shost with protocol type ether_type
 */
void populate_ethernet_header(uint8_t *buf, uint8_t *eth_shost, uint8_t *eth_dhost, uint16_t ether_type)
{
    struct sr_ethernet_hdr *rep_eth_hdr = (struct sr_ethernet_hdr *) buf;
    memcpy(rep_eth_hdr->ether_shost, eth_shost, sizeof(uint8_t) * ETHER_ADDR_LEN);
    memcpy(rep_eth_hdr->ether_dhost, eth_dhost, sizeof(uint8_t) * ETHER_ADDR_LEN);
    rep_eth_hdr->ether_type = htons(ether_type);
}

/*
 * Populate allocated buf with a fresh IP Header. You should call this method
 * once the data has been put in the buffer so that the generated checksum is
 * valid.
 *
 * buf:  Buffer with enough space for header
 * data_len:  Length of data (excluding IP header which will be added automatically) (Host Order)
 * proto:  IP Protocol
 * src:  Source Address (Network Order)
 * dst:  Destination Address (Network Order)
 */
void populate_ip_header(uint8_t *buf, uint16_t data_len, uint8_t proto, struct in_addr src, struct in_addr dst)
{
    struct ip *ip_hdr = (struct ip *)buf;

    ip_hdr->ip_v = IPv4;
    ip_hdr->ip_hl = sizeof(struct ip)/IPv4_WORD_SIZE;
    ip_hdr->ip_len = htons(sizeof(struct ip) + data_len);
    ip_hdr->ip_id = 0x0000;
    ip_hdr->ip_off = 0x0000;
    ip_hdr->ip_ttl = IPV4_TTL;
    ip_hdr->ip_p = proto;
    ip_hdr->ip_src = src;
    ip_hdr->ip_dst = dst;

    /* Calculate Checksum */
    ip_hdr->ip_sum = htons(IP_header_checksum(ip_hdr));
}

uint16_t IP_header_checksum(struct ip *ip_hdr)
{
    return header_checksum((uint8_t *)ip_hdr, ip_hdr->ip_hl * IPv4_WORD_SIZE, IPv4_CHECKSUM_OFFSET, IPv4_CHECKSUM_LENGTH);
}

uint16_t ICMP_header_checksum(struct icmp_hdr *icmp_hdr, uint16_t icmp_len)
{
    return header_checksum((uint8_t *)icmp_hdr, icmp_len, ICMP_CHECKSUM_OFFSET, ICMP_CHECKSUM_LENGTH);
}

uint16_t header_checksum(uint8_t *buf, uint16_t len, uint16_t cksum_offset, uint16_t cksum_length)
{
    uint32_t sum = 0;
    uint8_t *header = malloc(sizeof(uint8_t) * len);

    /* Copy over buffer */
    memcpy(header, buf, len);

    /* Set header checksum to zero */
    memset(header + cksum_offset, 0x0, cksum_length);

    /* Calculate 16 bit sum */
    for(int i=0; i < len; i+=2) {
        sum += (uint32_t)(header[i] << 8 | header[i+1]);
    }

    /* Fold 32 bit number to 16 bit */
    while(sum >> 16)
        sum = (sum >> 16) + (sum & 0xffff);

    /* One's Complement */
    sum = ~sum;

    /* Free up allocated buffer */
    free(header);

    return sum;
}
