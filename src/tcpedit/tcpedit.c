/* $Id$ */

/*
 * Copyright (c) 2001-2010 Aaron Turner.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright owners nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "defines.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdarg.h>

#include "tcpedit-int.h"
#include "tcpedit_stub.h"
#include "portmap.h"
#include "common.h"
#include "edit_packet.h"
#include "parse_args.h"
#include "plugins/dlt_plugins.h"


#include "lib/sll.h"
#include "dlt.h"

tOptDesc *const tcpedit_tcpedit_optDesc_p;

/**
 * \brief Edit the given packet
 *
 * Processs a given packet and edit the pkthdr/pktdata structures
 * according to the rules in tcpedit
 * Returns: TCPEDIT_ERROR on error
 *          TCPEDIT_SOFT_ERROR on remove packet
 *          0 on no change
 *          1 on change
 */
int
tcpedit_packet(tcpedit_t *tcpedit, struct pcap_pkthdr **pkthdr,
        u_char **pktdata, tcpr_dir_t direction)
{
    ipv4_hdr_t *ip_hdr = NULL;
    ipv6_hdr_t *ip6_hdr = NULL;
    arp_hdr_t *arp_hdr = NULL;
    int l2len = 0, l2proto, retval = 0, dst_dlt, src_dlt, pktlen, lendiff;
    int ipflags = 0, tclass = 0;
    int needtorecalc = 0;           /* did the packet change? if so, checksum */
    u_char *packet = *pktdata;
    assert(tcpedit);
    assert(pkthdr);
    assert(*pkthdr);
    assert(pktdata);
    assert(packet);
    assert(tcpedit->validated);


    tcpedit->runtime.packetnum++;
    dbgx(3, "packet " COUNTER_SPEC " caplen %d", 
            tcpedit->runtime.packetnum, (*pkthdr)->caplen);

    /*
     * remove the Ethernet FCS (checksum)?
     * note that this feature requires the end user to be smart and
     * only set this flag IFF the pcap has the FCS.  If not, then they
     * just removed 2 bytes of ACTUAL PACKET DATA.  Sucks to be them.
     */
    if (tcpedit->efcs > 0) {
        (*pkthdr)->caplen -= 4;
        (*pkthdr)->len -= 4;
    }

    src_dlt = tcpedit_dlt_src(tcpedit->dlt_ctx);
    
    /* not everything has a L3 header, so check for errors.  returns proto in network byte order */
    if ((l2proto = tcpedit_dlt_proto(tcpedit->dlt_ctx, src_dlt, packet, (*pkthdr)->caplen)) < 0) {
        dbg(2, "Packet has no L3+ header");
    } else {
        dbgx(2, "Layer 3 protocol type is: 0x%04x", ntohs(l2proto));
    }
        
    /* rewrite Layer 2 */
    if ((pktlen = tcpedit_dlt_process(tcpedit->dlt_ctx, pktdata, (*pkthdr)->caplen, direction)) == TCPEDIT_ERROR)
        errx(-1, "%s", tcpedit_geterr(tcpedit));

    /* unable to edit packet, most likely 802.11 management or data QoS frame */
    if (pktlen == TCPEDIT_SOFT_ERROR) {
        dbgx(3, "%s", tcpedit_geterr(tcpedit));
        return TCPEDIT_SOFT_ERROR;
    }

    /* update our packet lengths (real/captured) based on L2 length changes */
    lendiff = pktlen - (*pkthdr)->caplen;
    (*pkthdr)->caplen += lendiff;
    (*pkthdr)->len += lendiff;
    
    dst_dlt = tcpedit_dlt_dst(tcpedit->dlt_ctx);
    l2len = tcpedit_dlt_l2len(tcpedit->dlt_ctx, dst_dlt, packet, (*pkthdr)->caplen);

    dbgx(2, "dst_dlt = %04x\tsrc_dlt = %04x\tproto = %04x\tl2len = %d", dst_dlt, src_dlt, ntohs(l2proto), l2len);

    /* does packet have an IP header?  if so set our pointer to it */
    if (l2proto == htons(ETHERTYPE_IP)) {
        ip_hdr = (ipv4_hdr_t *)tcpedit_dlt_l3data(tcpedit->dlt_ctx, src_dlt, packet, (*pkthdr)->caplen);
        if (ip_hdr == NULL) {
            return TCPEDIT_ERROR;
        }        
        dbgx(3, "Packet has an IPv4 header: %p...", ip_hdr);
    } else if (l2proto == htons(ETHERTYPE_IP6)) {
        ip6_hdr = (ipv6_hdr_t *)tcpedit_dlt_l3data(tcpedit->dlt_ctx, src_dlt, packet, (*pkthdr)->caplen);
        if (ip6_hdr == NULL) {
            return TCPEDIT_ERROR;
        }
        dbgx(3, "Packet has an IPv6 header: %p...", ip6_hdr);
    } else {
        dbgx(3, "Packet isn't IPv4 or IPv6: 0x%04x", l2proto);
        /* non-IP packets have a NULL ip_hdr struct */
        ip_hdr = NULL;
    }

    /* The following edits only apply for IPv4 */
    if (ip_hdr != NULL) {
        
        /* set TOS ? */
        if (tcpedit->tos > -1) {
            ip_hdr->ip_tos = tcpedit->tos;
            needtorecalc += 1;
        }
            
        /* rewrite the TTL */
        needtorecalc += rewrite_ipv4_ttl(tcpedit, ip_hdr);

        /* rewrite TCP/UDP ports */
        if (tcpedit->portmap != NULL) {
            if ((retval = rewrite_ipv4_ports(tcpedit, &ip_hdr)) < 0)
                return TCPEDIT_ERROR;
            needtorecalc += retval;
        }
    }
    /* IPv6 edits */
    else if (ip6_hdr != NULL) {
        /* rewrite the hop limit */
        needtorecalc += rewrite_ipv6_hlim(tcpedit, ip6_hdr);

        /* set traffic class? */
        if (tcpedit->tclass > -1) {
            /* calculate the bits */
            tclass = tcpedit->tclass << 20;
            
            /* convert our 4 bytes to an int */
            memcpy(&ipflags, &ip6_hdr->ip_flags, 4);
            
            /* strip out the old tclass bits */
            ipflags = ntohl(ipflags) & 0xf00fffff;

            /* add the tclass bits back */
            ipflags += tclass; 
            ipflags = htonl(ipflags);
            memcpy(&ip6_hdr->ip_flags, &ipflags, 4);
            needtorecalc ++;
        }

        /* set the flow label? */
        if (tcpedit->flowlabel > -1) {
            memcpy(&ipflags, &ip6_hdr->ip_flags, 4);
            ipflags = ntohl(ipflags) & 0xfff00000;
            ipflags += tcpedit->flowlabel;
            ipflags = htonl(ipflags);
            memcpy(&ip6_hdr->ip_flags, &ipflags, 4);
            needtorecalc ++;
        }

        /* rewrite TCP/UDP ports */
        if (tcpedit->portmap != NULL) {
            if ((retval = rewrite_ipv6_ports(tcpedit, &ip6_hdr)) < 0)
                return TCPEDIT_ERROR;
            needtorecalc += retval;
        }
    }

    /* (Un)truncate or MTU truncate packet? */
    if (tcpedit->fixlen || tcpedit->mtu_truncate) {
        if ((retval = untrunc_packet(tcpedit, *pkthdr, packet, ip_hdr, ip6_hdr)) < 0)
            return TCPEDIT_ERROR;
        needtorecalc += retval;
    }
    
    /* rewrite IP addresses in IPv4/IPv6 or ARP */
    if (tcpedit->rewrite_ip) {
        /* IP packets */
        if (ip_hdr != NULL) {
            if ((retval = rewrite_ipv4l3(tcpedit, ip_hdr, direction)) < 0)
                return TCPEDIT_ERROR;
            needtorecalc += retval;
        } else if (ip6_hdr != NULL) {
            if ((retval = rewrite_ipv6l3(tcpedit, ip6_hdr, direction)) < 0)
                return TCPEDIT_ERROR;
            needtorecalc += retval;
        }

        /* ARP packets */
        else if (l2proto == htons(ETHERTYPE_ARP)) {
            arp_hdr = (arp_hdr_t *)&(packet[l2len]);
            /* unlike, rewrite_ipl3, we don't care if the packet changed
             * because we never need to recalc the checksums for an ARP
             * packet.  So ignore the return value
             */
            if (rewrite_iparp(tcpedit, arp_hdr, direction) < 0)
                return TCPEDIT_ERROR;
        }
    }


    /* do we need to spoof the src/dst IP address in IPv4 or ARP? */
    if (tcpedit->seed) {
        /* IPv4 Packets */
        if (ip_hdr != NULL) {
            if ((retval = randomize_ipv4(tcpedit, *pkthdr, packet, 
                    ip_hdr)) < 0)
                return TCPEDIT_ERROR;
            needtorecalc += retval;

        } else if (ip6_hdr != NULL) {
            if ((retval = randomize_ipv6(tcpedit, *pkthdr, packet,
                    ip6_hdr)) < 0)
                return TCPEDIT_ERROR;
            needtorecalc += retval;

        /* ARP packets */
        } else if (l2proto == htons(ETHERTYPE_ARP)) {
            if (direction == TCPR_DIR_C2S) {
                if (randomize_iparp(tcpedit, *pkthdr, packet, 
                        tcpedit->runtime.dlt1) < 0)
                    return TCPEDIT_ERROR;
            } else {
                if (randomize_iparp(tcpedit, *pkthdr, packet, 
                        tcpedit->runtime.dlt2) < 0)
                    return TCPEDIT_ERROR;
            }
        }
    }

    /* do we need to fix checksums? -- must always do this last! 
     * We recalc if:
     * user specified --fixcsum
     * packet was edited AND user did NOT specify --nofixcsum
     */
    if ((tcpedit->fixcsum == TCPEDIT_FIXCSUM_ON || 
            (needtorecalc && tcpedit->fixcsum != TCPEDIT_FIXCSUM_DISABLE))) {
        if (ip_hdr != NULL) {
            retval = fix_ipv4_checksums(tcpedit, *pkthdr, ip_hdr);
        } else if (ip6_hdr != NULL) {
            retval = fix_ipv6_checksums(tcpedit, *pkthdr, ip6_hdr);
        } else {
            retval = TCPEDIT_OK;
        }
        if (retval < 0) {
            return TCPEDIT_ERROR;
        } else if (retval == TCPEDIT_WARN) {
            warnx("%s", tcpedit_getwarn(tcpedit));
        }
    }

    tcpedit_dlt_merge_l3data(tcpedit->dlt_ctx, dst_dlt, packet, (*pkthdr)->caplen, (u_char *)ip_hdr);

    tcpedit->runtime.total_bytes += (*pkthdr)->caplen;
    tcpedit->runtime.pkts_edited ++;
    return retval;
}

/**
 * initializes the tcpedit library.  returns 0 on success, -1 on error.
 */
int
tcpedit_init(tcpedit_t **tcpedit_ex, int dlt)
{
    tcpedit_t *tcpedit;
    
    *tcpedit_ex = safe_malloc(sizeof(tcpedit_t));
    tcpedit = *tcpedit_ex;

    if ((tcpedit->dlt_ctx = tcpedit_dlt_init(tcpedit, dlt)) == NULL)
        return TCPEDIT_ERROR;

    tcpedit->mtu = DEFAULT_MTU; /* assume 802.3 Ethernet */

    /* disabled by default */
    tcpedit->tos = -1;
    tcpedit->tclass = -1;
    tcpedit->flowlabel = -1;
 
    memset(&(tcpedit->runtime), 0, sizeof(tcpedit_runtime_t));
    tcpedit->runtime.dlt1 = dlt;
    tcpedit->runtime.dlt2 = dlt;
    
    dbgx(1, "Input file (1) datalink type is %s\n",
            pcap_datalink_val_to_name(dlt));

#ifdef FORCE_ALIGN
    tcpedit->runtime.l3buff = (u_char *)safe_malloc(MAXPACKET);
#endif

    return TCPEDIT_OK;
}

/**
 * return the output DLT type
 */
int
tcpedit_get_output_dlt(tcpedit_t *tcpedit)
{
    assert(tcpedit);
    return tcpedit_dlt_output_dlt(tcpedit->dlt_ctx);
}

/**
 * \brief tcpedit option validator.  Call after tcpedit_init()
 *
 * Validates that given the current state of tcpedit that the given
 * pcap source and destination (based on DLT) can be properly rewritten
 * return 0 on sucess
 * return -1 on error
 * DO NOT USE!
 */
int
tcpedit_validate(tcpedit_t *tcpedit)
{
    assert(tcpedit);
    tcpedit->validated = 1;

    /* we used to do a bunch of things here, but not anymore...
     * maybe I should find something to do or just get ride of it
     */
    return 0;
}

/**
 * return the error string when a tcpedit() function returns
 * TCPEDIT_ERROR
 */
char *
tcpedit_geterr(tcpedit_t *tcpedit)
{

    assert(tcpedit);
    return tcpedit->runtime.errstr;

}

/**
 * \brief Internal function to set the tcpedit error string
 *
 * Used to set the error string when there is an error, result is retrieved
 * using tcpedit_geterr().  You shouldn't ever actually call this, but use
 * tcpedit_seterr() which is a macro wrapping this instead.
 */
void
__tcpedit_seterr(tcpedit_t *tcpedit, const char *func, const int line, const char *file, const char *fmt, ...)
{
    va_list ap;
    char errormsg[TCPEDIT_ERRSTR_LEN];
    
    assert(tcpedit);

    va_start(ap, fmt);
    if (fmt != NULL) {
        (void)vsnprintf(errormsg, 
              (TCPEDIT_ERRSTR_LEN - 1), fmt, ap);
    }

    va_end(ap);
    
    snprintf(tcpedit->runtime.errstr, (TCPEDIT_ERRSTR_LEN -1), "From %s:%s() line %d:\n%s",
        file, func, line, errormsg);
}

/**
 * return the warning string when a tcpedit() function returns
 * TCPEDIT_WARN
 */
char *
tcpedit_getwarn(tcpedit_t *tcpedit)
{
    assert(tcpedit);

    return tcpedit->runtime.warnstr;
}

/**
 * used to set the warning string when there is an warning
 */
void
tcpedit_setwarn(tcpedit_t *tcpedit, const char *fmt, ...)
{
    va_list ap;
    assert(tcpedit);

    va_start(ap, fmt);
    if (fmt != NULL)
        (void)vsnprintf(tcpedit->runtime.warnstr, (TCPEDIT_ERRSTR_LEN - 1), fmt, ap);

    va_end(ap);
        
}

/**
 * \brief Checks the given error code and does the right thing
 * 
 * Generic function which checks the TCPEDIT_* error code
 * and always returns OK or ERROR.  For warnings, prints the 
 * warning message and returns OK.  For any other value, fails with
 * an assert.
 *
 * prefix is a string prepended to the error/warning
 */
int
tcpedit_checkerror(tcpedit_t *tcpedit, const int rcode, const char *prefix) {
    assert(tcpedit);
    
    switch (rcode) {
        case TCPEDIT_OK:
        case TCPEDIT_ERROR:
            return rcode;
            break;
        
        case TCPEDIT_SOFT_ERROR:
            if (prefix != NULL) {
                fprintf(stderr, "Error %s: %s\n", prefix, tcpedit_geterr(tcpedit));
            } else {
                fprintf(stderr, "Error: %s\n", tcpedit_geterr(tcpedit));
            }            
            break;
        case TCPEDIT_WARN:
            if (prefix != NULL) {
                fprintf(stderr, "Warning %s: %s\n", prefix, tcpedit_getwarn(tcpedit));
            } else {
                fprintf(stderr, "Warning: %s\n", tcpedit_getwarn(tcpedit));
            }
            return TCPEDIT_OK;
            break;
            
        default:
            assert(0 == 1); /* this should never happen! */
            break;
    }
    return TCPEDIT_ERROR;
}

/**
 * \brief Cleans up after ourselves.  Return 0 on success. 
 * 
 * Clean up after ourselves, but does not actually free the ptr.
 */
int
tcpedit_close(tcpedit_t *tcpedit)
{

    assert(tcpedit);
    dbgx(1, "tcpedit processed " COUNTER_SPEC " bytes in " COUNTER_SPEC
            " packets.\n", tcpedit->runtime.total_bytes, 
            tcpedit->runtime.pkts_edited);

    /* free buffer if required */
#ifdef FORCE_ALIGN
    safe_free(tcpedit->runtime.l3buff);
#endif

    return 0;
}

/**
 * Return a ptr to the Layer 3 data.  Returns TCPEDIT_ERROR on error
 */
const u_char *
tcpedit_l3data(tcpedit_t *tcpedit, tcpedit_coder_t code, u_char *packet, const int pktlen)
{
    u_char *result = NULL;
    if (code == BEFORE_PROCESS) {
        result = tcpedit_dlt_l3data(tcpedit->dlt_ctx, tcpedit->dlt_ctx->decoder->dlt, packet, pktlen);
    } else {
        result = tcpedit_dlt_l3data(tcpedit->dlt_ctx, tcpedit->dlt_ctx->encoder->dlt, packet, pktlen);
    }
    return result;
}

/**
 * return the length of the layer 2 header.  Returns TCPEDIT_ERROR on error
 */
int 
tcpedit_l2len(tcpedit_t *tcpedit, tcpedit_coder_t code, u_char *packet, const int pktlen)
{
    int result = 0;
    if (code == BEFORE_PROCESS) {
        result = tcpedit_dlt_l2len(tcpedit->dlt_ctx, tcpedit->dlt_ctx->decoder->dlt, packet, pktlen);
    } else {
        result = tcpedit_dlt_l2len(tcpedit->dlt_ctx, tcpedit->dlt_ctx->encoder->dlt, packet, pktlen);
    }
    return result;
}

/**
 * Returns the layer 3 type, often encoded as the layer2.proto field
 */
int 
tcpedit_l3proto(tcpedit_t *tcpedit, tcpedit_coder_t code, const u_char *packet, const int pktlen)
{
    int result = 0;
    if (code == BEFORE_PROCESS) {
        result = tcpedit_dlt_proto(tcpedit->dlt_ctx, tcpedit->dlt_ctx->decoder->dlt, packet, pktlen);        
    } else {
        result = tcpedit_dlt_proto(tcpedit->dlt_ctx, tcpedit->dlt_ctx->encoder->dlt, packet, pktlen);
    }
    return ntohs(result);
}

/*
u_char *
tcpedit_srcmac(tcpedit_t *tcpedit, tcpedit_coder_t code, u_char *packet, const int pktlen)
{
   
}

u_char *
tcpedit_dstmac(tcpedit_t *tcpedit, tcpedit_coder_t code, u_char *packet, const int pktlen)
{
    
}

int 
tcpedit_maclen(tcpedit_t *tcpedit, tcpedit_coder_t code)
{
    
}

*/
