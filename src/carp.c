/*
 * Copyright (c) 2004-2015 Frank Denis. All rights reserved.
 *
 * This crucial part of UCARP is derived from the OpenBSD project.
 * Original copyright follows.
 *
 * Copyright (c) 2002 Michael Shalayeff. All rights reserved.
 * Copyright (c) 2003 Ryan McBride. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR OR HIS RELATIVES BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF MIND, USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>
#include "ucarp.h"
#include "crypto.h"
#ifndef USE_SYSTEM_CRYPT_SHA1
# include "crypto-sha1.h"
#else
# include <sha1.h>
#endif
#include "ip_carp.h"
#include "fillmac.h"
#include "garp.h"
#include "spawn.h"
#include "log.h"
#include "carp_p.h"

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

static void carp_set_state(struct carp_softc *sc, int state)
{
    if ((int) sc->sc_state == state) {
        return;
    }
    switch (state) {
    case INIT:
        logfile(LOG_INFO, _("Switching to state: INIT"));
        break;
    case BACKUP:
        logfile(LOG_WARNING, _("Switching to state: BACKUP"));
        if ((sc->sc_state != INIT) || (neutral != 1)) {
            (void) spawn_handler(dev_desc_fd, downscript);
        }
        break;
    case MASTER:
        logfile(LOG_WARNING, _("Switching to state: MASTER"));
        (void) spawn_handler(dev_desc_fd, upscript);
        gratuitous_arp(dev_desc_fd);
        break;
    default:
        logfile(LOG_ERR, _("Unknown state: [%d]"), (int) state);
        abort();
    }
    sc->sc_state = state;
}

static void carp_hmac_prepare(struct carp_softc *sc)
{
    unsigned char version = CARP_VERSION, type = CARP_ADVERTISEMENT;
    unsigned char vhid = sc->sc_vhid & 0xff;
    size_t i;
#ifdef INET6
    struct in6_addr in6;
#endif /* INET6 */

    /* compute ipad from key */
    memset(sc->sc_pad, 0, sizeof sc->sc_pad);
    memcpy(sc->sc_pad, sc->sc_key, sizeof sc->sc_key);
    for (i = 0; i < sizeof sc->sc_pad; i++) {
        sc->sc_pad[i] ^= 0x36;
    }
    /* precompute first part of inner hash */
    SHA1Init(&sc->sc_sha1);
    SHA1Update(&sc->sc_sha1, sc->sc_pad, sizeof sc->sc_pad);
    SHA1Update(&sc->sc_sha1, (void *) &version, sizeof version);
    SHA1Update(&sc->sc_sha1, (void *) &type, sizeof type);
    SHA1Update(&sc->sc_sha1, (void *) &vhid, sizeof vhid);
    SHA1Update(&sc->sc_sha1, (void *) &vaddr.s_addr, sizeof vaddr.s_addr);

    /* convert ipad to opad */
    for (i = 0; i < sizeof sc->sc_pad; i++) {
        sc->sc_pad[i] ^= 0x36 ^ 0x5c;
    }
}

static unsigned short cksum(const void * const buf_, const size_t len)
{
    const unsigned char *buf = (const unsigned char *) buf_;
    unsigned long sum = 0UL;
    size_t evenlen = len & ~ (size_t) 1U;
    size_t i = (size_t) 0U;

    if (len <= (size_t) 0U) {
        return 0U;
    }
    do {
        sum += (buf[i] << 8) | buf[i + 1];
        if (sum > 0xffff) {
            sum &= 0xffff;
            sum++;
        }
        i += 2;
    } while (i < evenlen);
    if (i != evenlen) {
        sum += buf[i] << 8;
        if (sum > 0xffff) {
            sum &= 0xffff;
            sum++;
        }
    }
    return (unsigned short) ~sum;
}

static void carp_hmac_generate(struct carp_softc *sc, u_int32_t counter[2],
                               unsigned char *md)
{
    SHA1_CTX ctx;

    /* fetch first half of inner hash */
    memcpy(&ctx, &sc->sc_sha1, sizeof ctx);

    SHA1Update(&ctx, (void *) counter, sizeof sc->sc_counter);

    SHA1Final(md, &ctx);

    /* outer hash */
    SHA1Init(&ctx);
    SHA1Update(&ctx, sc->sc_pad, sizeof sc->sc_pad);
    SHA1Update(&ctx, md, 20);
    SHA1Final(md, &ctx);
}

static int carp_prepare_ad(struct carp_header *ch, struct carp_softc *sc)
{
    if (sc->sc_init_counter != 0) {
        /* this could also be seconds since unix epoch */
#ifdef HAVE_ARC4RANDOM
        sc->sc_counter = arc4random();
#else
        sc->sc_counter = random();
#endif
        sc->sc_counter <<= 32;
#ifdef HAVE_ARC4RANDOM
        sc->sc_counter += arc4random();
#else
        sc->sc_counter += random();
#endif
    } else if (sc->sc_counter == 0xffffffffffffffffULL) {
        sc->sc_counter = 0ULL;
    } else {
        sc->sc_counter++;
    }
    ch->carp_counter[0] = htonl((sc->sc_counter >> 32) & 0xffffffff);
    ch->carp_counter[1] = htonl(sc->sc_counter & 0xffffffff);

    carp_hmac_generate(sc, ch->carp_counter, ch->carp_md);

    return 0;
}

static void carp_send_ad(struct carp_softc *sc)
{
    struct carp_header ch;
    struct ether_header eh;
    struct timeval tv;
    struct ip ip;
    unsigned char *ip_ptr;
    unsigned char *pkt;
    unsigned short sum;
    size_t ip_len;
    size_t eth_len;
    int advbase;
    int advskew;
    int rc;
    struct sockaddr_in sock_in;

    logfile(LOG_DEBUG, "-> carp_send_ad()");

    advbase = sc->sc_advbase;
    if (carp_suppress_preempt == 0 ||
        sc->sc_advskew > CARP_BULK_UPDATE_MIN_DELAY) {
        advskew = sc->sc_advskew;
    } else {
        advskew = CARP_BULK_UPDATE_MIN_DELAY;
    }
    tv.tv_sec = advbase;
    tv.tv_usec = (unsigned int) (advskew * 1000000ULL / 256ULL);

    ch.carp_version = CARP_VERSION;
    ch.carp_type = CARP_ADVERTISEMENT;
    ch.carp_vhid = sc->sc_vhid;
    ch.carp_advbase = advbase;
    ch.carp_advskew = advskew;
    ch.carp_authlen = CARP_AUTHLEN;
    ch.carp_pad1 = 0;   /* must be zero */
    ch.carp_cksum = 0;

    ip_len = sizeof ip + sizeof ch;
    eth_len = ip_len + sizeof eh;
    if ((pkt = ALLOCA(eth_len)) == NULL) {
        logfile(LOG_ERR, _("Out of memory to create packet"));
        timeradd(&now, &tv, &sc->sc_ad_tmo);
        return;
    }
    ip.ip_v = IPVERSION;
    ip.ip_hl = (sizeof ip) >> 2;
    ip.ip_tos = IPTOS_LOWDELAY;
    ip.ip_len = htons(ip_len);
#ifdef HAVE_ARC4RANDOM
    ip.ip_id = htons(arc4random() & 0xffff);
#else
    ip.ip_id = htons(random() & 0xffff);
#endif
    ip.ip_off = htons(IP_DF);
    ip.ip_ttl = CARP_DFLTTL;
    ip.ip_p = IPPROTO_CARP;
    ip.ip_sum = 0;

    memcpy(&ip.ip_src, &srcip, sizeof ip.ip_src);
    memcpy(&ip.ip_dst.s_addr, &mcastip, sizeof ip.ip_dst.s_addr);

    carp_prepare_ad(&ch, sc);

    ch.carp_cksum = 0;
    sum = cksum(&ch, sizeof ch);
    ch.carp_cksum = htons(sum);

    eh.ether_shost[0] = 0x00;
    eh.ether_shost[1] = 0x00;
    eh.ether_shost[2] = 0x5e;
    eh.ether_shost[3] = 0x00;
    eh.ether_shost[4] = 0x00;
    eh.ether_shost[5] = vhid;

    if (no_mcast) {
        eh.ether_dhost[0] = 0xff;
        eh.ether_dhost[1] = 0xff;
        eh.ether_dhost[2] = 0xff;
        eh.ether_dhost[3] = 0xff;
        eh.ether_dhost[4] = 0xff;
        eh.ether_dhost[5] = 0xff;
    } else {
        unsigned int m = ntohl(mcastip.s_addr);
        eh.ether_dhost[0] = 0x01;
        eh.ether_dhost[1] = 0x00;
        eh.ether_dhost[2] = 0x5e;
        eh.ether_dhost[3] = m >> 16 & 0x7f;
        eh.ether_dhost[4] = m >>  8 & 0xff;
        eh.ether_dhost[5] = m       & 0xff;
    }
    eh.ether_type = htons(ETHERTYPE_IP);

    memcpy(pkt, &eh, sizeof eh);
    memcpy(pkt + sizeof eh, &ip, sizeof ip);
    memcpy(pkt + sizeof ip + sizeof eh, &ch, sizeof ch);

    ip_ptr = pkt + sizeof eh;
    sum = cksum(ip_ptr, ip_len);
    ip_ptr[offsetof(struct ip, ip_sum)] = (sum >> 8) & 0xff;
    ip_ptr[offsetof(struct ip, ip_sum) + 1] = sum & 0xff;

    if (udpu_addr==NULL && no_mcast) {
        sock_in.sin_addr.s_addr=htonl(-1);
        sock_in.sin_port = htons(port);
        sock_in.sin_family = PF_INET;
    }
    do {
		if(udpu_addr==NULL) {
            if (no_mcast) {
                rc = sendto(broadcast_fd, pkt, eth_len, 0, (struct sockaddr *)&sock_in, sizeof(struct sockaddr_in));
            } else {
                rc = write(dev_desc_fd, pkt, eth_len);
            }
		} else {
			rc = sendto(udpu_fd, pkt, eth_len, 0,
					(struct sockaddr *)&udpu_dest, sizeof(udpu_dest));
		}
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        logfile(LOG_WARNING, _("write() has failed: %s"), strerror(errno));
        if (sc->sc_sendad_errors < INT_MAX) {
            sc->sc_sendad_errors++;
        }
        if (sc->sc_sendad_errors == CARP_SENDAD_MAX_ERRORS) {
            logfile(LOG_ERR, _("write() error #%d/%d"),
                    carp_suppress_preempt, CARP_SENDAD_MAX_ERRORS);
            carp_suppress_preempt++;
            if (carp_suppress_preempt == 1) {
                carp_send_ad_all(sc);
            }
        }
        sc->sc_sendad_success = 0;
    } else {
        if (sc->sc_sendad_errors >= CARP_SENDAD_MAX_ERRORS) {
            if (++sc->sc_sendad_success >= CARP_SENDAD_MIN_SUCCESS) {
                carp_suppress_preempt--;
                sc->sc_sendad_errors = 0;
            }
        } else {
            sc->sc_sendad_errors = 0;
        }
    }

    logfile(LOG_DEBUG, _("* advertisement injected *"));

    ALLOCA_FREE(pkt);

    if (sc->sc_delayed_arp > 0)
        sc->sc_delayed_arp--;
    if (sc->sc_delayed_arp == 0) {
        if (sc->sc_state == MASTER) {
            gratuitous_arp(dev_desc_fd);
        }
        sc->sc_delayed_arp = -1;
    }
    if (advbase != 255 || advskew != 255) {
        timeradd(&now, &tv, &sc->sc_ad_tmo);
        /* IPv6 ? */
    }
}

static void carp_send_ad_all(struct carp_softc *sc) {
    carp_send_ad(sc);
}

static void carp_setrun(struct carp_softc *sc, sa_family_t af)
{
    struct timeval tv;

    logfile(LOG_DEBUG, "carp_setrun()");
    if (gettimeofday(&now, NULL) != 0) {
        logfile(LOG_WARNING, _("initializing now to gettimeofday() failed: %s"),
                strerror(errno));
    }
    switch (sc->sc_state) {
    case INIT:
        carp_set_state(sc, BACKUP);
        carp_setrun(sc, 0);
        break;
    case BACKUP:
        sc->sc_ad_tmo.tv_sec = 0;
        tv.tv_sec = (unsigned int) sc->sc_advbase * dead_ratio;
        tv.tv_usec = (unsigned int) (sc->sc_advskew * 1000000ULL / 256ULL);
        switch (af) {
        case AF_INET:
            timeradd(&now, &tv, &sc->sc_md_tmo);
            break;
#ifdef INET6
        case AF_INET6:
            timeradd(&now, &tv, &sc->sc_md6_tmo);
            break;
#endif /* INET6 */
        default:
            timeradd(&now, &tv, &sc->sc_md_tmo);
#ifdef INET6
            timeradd(&now, &tv, &sc->sc_md6_tmo);
#endif
            break;
        }
        break;
    case MASTER:
        tv.tv_sec = (unsigned int) sc->sc_advbase;
        tv.tv_usec = (unsigned int) (sc->sc_advskew * 1000000ULL / 256ULL);
        timeradd(&now, &tv, &sc->sc_md_tmo);
        /* No IPv6 scheduling ? */
        break;
    }
}

static void carp_master_down(struct carp_softc *sc)
{
    logfile(LOG_DEBUG, "carp_master_down()");
    switch (sc->sc_state) {
    case INIT:
        logfile(LOG_DEBUG, _("master_down event in INIT state"));
        break;
    case MASTER:
        break;
    case BACKUP:
        carp_set_state(sc, MASTER);
        carp_send_ad(sc);
        /* Schedule a delayed ARP request to deal w/ some L3 switches */
        sc->sc_delayed_arp = 2;
#ifdef INET6
        carp_send_na(sc);
#endif /* INET6 */
        carp_setrun(sc, 0);
        break;
    }
}

static void packethandler(unsigned char *dummy,
                          const struct pcap_pkthdr *header,
                          const unsigned char *sp)
{
    struct ether_header etherhead;
    struct ip iphead;
    unsigned int source;
    unsigned int dest;
    unsigned char proto;
    unsigned int caplen;
    unsigned int ip_len;

    (void) dummy;
    if (header->caplen <= (sizeof etherhead + sizeof iphead)) {
        return;
    }
    memcpy(&etherhead, sp, sizeof etherhead);
    logfile(LOG_DEBUG, "Ethernet "
             "[%02x:%02x:%02x:%02x:%02x:%02x]->[%02x:%02x:%02x:%02x:%02x:%02x] "
             "type [%04x]",
            (unsigned int) etherhead.ether_shost[0],
            (unsigned int) etherhead.ether_shost[1],
            (unsigned int) etherhead.ether_shost[2],
            (unsigned int) etherhead.ether_shost[3],
            (unsigned int) etherhead.ether_shost[4],
            (unsigned int) etherhead.ether_shost[5],
            (unsigned int) etherhead.ether_dhost[0],
            (unsigned int) etherhead.ether_dhost[1],
            (unsigned int) etherhead.ether_dhost[2],
            (unsigned int) etherhead.ether_dhost[3],
            (unsigned int) etherhead.ether_dhost[4],
            (unsigned int) etherhead.ether_dhost[5],
            (unsigned int) ntohs(etherhead.ether_type));
    sp += sizeof etherhead;
    caplen = header->caplen - sizeof etherhead;
    memcpy(&iphead, sp, sizeof iphead);
    if (iphead.ip_src.s_addr == srcip.s_addr) {
        return;
    }
    ip_len = iphead.ip_hl << 2;
    source = ntohl(iphead.ip_src.s_addr);
    dest = ntohl(iphead.ip_dst.s_addr);
    proto = iphead.ip_p;
    switch (proto) {
    case IPPROTO_CARP: {
        struct carp_header ch;
        unsigned long long tmp_counter;
        struct timeval sc_tv;
        struct timeval ch_tv;

        logfile(LOG_DEBUG,
                "\n---------------------------\n\n"
                "carp [%d.%d.%d.%d] -> [%d.%d.%d.%d]",
                source >> 24 & 0xff, source >> 16 & 0xff,
                source >> 8 & 0xff, source & 0xff,
                dest >> 24 & 0xff, dest >> 16 & 0xff,
                dest >> 8 & 0xff, dest & 0xff);
        if (caplen < ip_len + sizeof ch) {
            logfile(LOG_DEBUG,
                    "Bogus size: caplen=[%u], ip_len=[%u] ch_len=[%u]",
                    (unsigned int) caplen, (unsigned int) ip_len,
                    (unsigned int) sizeof ch);
            return;
        }
        memcpy(&ch, sp + ip_len, sizeof ch);
#ifdef EXTRA_DEBUG
        logfile(LOG_DEBUG,
                "Capture len: [%u] carp_header: [%u] ip_header: [%u]",
                (unsigned int) caplen, (unsigned int) sizeof ch,
                (unsigned int) ip_len);
        logfile(LOG_DEBUG, "CARP type: [%u] version: [%u]",
                (unsigned) ch.carp_type, (unsigned) ch.carp_version);
        logfile(LOG_DEBUG, "CARP vhid: [%u] advskew: [%u]",
                (unsigned) ch.carp_vhid, (unsigned) ch.carp_advskew);
        logfile(LOG_DEBUG, "CARP advbase: [%u] cksum: [%u]",
                (unsigned) ch.carp_advbase, (unsigned) ch.carp_cksum);
        logfile(LOG_DEBUG, "CARP counter: [%lu][%lu]",
                (unsigned long) ch.carp_counter[0],
                (unsigned long) ch.carp_counter[1]);
#endif
        if (iphead.ip_ttl != CARP_DFLTTL) {
            logfile(LOG_WARNING, _("Bad TTL: [%u]"),
                    (unsigned int) iphead.ip_ttl);
            return;
        }
        if (ch.carp_version != CARP_VERSION) {
            logfile(LOG_WARNING, _("Bad version: [%u]"),
                    (unsigned int) ch.carp_version);
            return;
        }
        if (ch.carp_vhid != vhid) {
            logfile(LOG_DEBUG, _("Ignoring vhid: [%u]"),
                    (unsigned int) ch.carp_vhid);
            return;
        }
        if (iphead.ip_dst.s_addr != mcastip.s_addr) {
            logfile(LOG_DEBUG, _("Ignoring different multicast ip: [%s]"),
                    inet_ntoa(iphead.ip_dst));
            return;
        }
        if (cksum(sp, ip_len + sizeof ch) != 0) {
            logfile(LOG_WARNING, _("Bad IP checksum"));
            return;
        }
        {
            SHA1_CTX ctx;
            unsigned char md2[20];

            memcpy(&ctx, &sc.sc_sha1, sizeof ctx);
            SHA1Update(&ctx,
                       (void *) &ch.carp_counter, sizeof ch.carp_counter);
            SHA1Final(md2, &ctx);

            SHA1Init(&ctx);
            SHA1Update(&ctx, sc.sc_pad, sizeof(sc.sc_pad));
            SHA1Update(&ctx, md2, sizeof md2);
            SHA1Final(md2, &ctx);

            if (sizeof md2 != sizeof ch.carp_md) {
                logfile(LOG_ERR, "sizeof md2 != sizeof carp_md !!!");
                abort();
            }
            if (memcmp(md2, ch.carp_md, sizeof md2) != 0) {
                logfile(LOG_WARNING,
                        _("Bad digest - "
                          "md2=[%02x%02x%02x%02x...] md=[%02x%02x%02x%02x...] - "
                          "Check vhid, password and virtual IP address"),
                        (unsigned int) md2[0], (unsigned int) md2[1],
                        (unsigned int) md2[2], (unsigned int) md2[3],
                        (unsigned int) (ch.carp_md)[0],
                        (unsigned int) (ch.carp_md)[1],
                        (unsigned int) (ch.carp_md)[2],
                        (unsigned int) (ch.carp_md)[3]);
                return;
            }
        }
        tmp_counter = ntohl(ch.carp_counter[0]);
        tmp_counter = tmp_counter << 32;
        tmp_counter += ntohl(ch.carp_counter[1]);
        sc.sc_init_counter = 0;
        sc.sc_counter = tmp_counter;

        sc_tv.tv_sec = (unsigned int) sc.sc_advbase;
        if (carp_suppress_preempt != 0 &&
            sc.sc_advskew < CARP_BULK_UPDATE_MIN_DELAY) {
            sc_tv.tv_usec = (unsigned int)
                (CARP_BULK_UPDATE_MIN_DELAY * 1000000ULL / 256ULL);
        } else {
            sc_tv.tv_usec = (unsigned int)
                (sc.sc_advskew * 1000000ULL / 256ULL);
        }
        ch_tv.tv_sec = (unsigned int) ch.carp_advbase;
        ch_tv.tv_usec = (unsigned int)
            (ch.carp_advskew * 1000000ULL / 256ULL);

        logfile(LOG_DEBUG, "sc_tv(local) = [%lu] ch_tv(remote) = [%lu]",
                (unsigned long) sc_tv.tv_sec,
                (unsigned long) ch_tv.tv_sec);

        switch (sc.sc_state) {
        case INIT:
            break;
        case MASTER:
            /*
             * If we receive an advertisement from a master who's going to
             * be more frequent than us, or from a master who's advertising
             * with the same frequency as us but with a lower IP address,
             * go into BACKUP state.
             */
            if (timercmp(&sc_tv, &ch_tv, >) ||
                (timercmp(&sc_tv, &ch_tv, ==) &&
                    iphead.ip_src.s_addr < srcip.s_addr)) {
                /* To be really sure the new master knows that is
                 * has to reassert control of the VIP */
                carp_send_ad(&sc);

                carp_set_state(&sc, BACKUP);
                carp_setrun(&sc, 0);
                logfile(LOG_WARNING, _("Preferred master advertised: "
                                       "going back to BACKUP state"));
            }

            /*
             * If we receive an advertisement from a master who's advertising
             * less frequently than us, or with the same frequency as us but
             * with a higher IP address, reassert our dominance by issuing
             * another gratuitous arp.
             */
            if (timercmp(&sc_tv, &ch_tv, <) ||
                (timercmp(&sc_tv, &ch_tv, ==) &&
                    iphead.ip_src.s_addr > srcip.s_addr)) {
                gratuitous_arp(dev_desc_fd);
                sc.sc_delayed_arp = 2; /* and yet another in 2 ticks */
                logfile(LOG_WARNING, _("Non-preferred master advertising: "
                                       "reasserting control of VIP with another gratuitous arp"));
            }
            break;
        case BACKUP:
            /*
             * If we're pre-empting masters who advertise slower than us,
             * and this one claims to be slower, treat him as down.
             */
            if (preempt != 0 && timercmp(&sc_tv, &ch_tv, <)) {
                carp_master_down(&sc);
                logfile(LOG_WARNING, _("Putting MASTER down - preemption"));
                break;
            }

            /*
             *  If the master is going to advertise at such a low frequency
             *  that he's guaranteed to time out, we'd might as well just
             *  treat him as timed out now.
             */
            sc_tv.tv_sec = (unsigned int) sc.sc_advbase * dead_ratio;
            if (timercmp(&sc_tv, &ch_tv, <)) {
                carp_master_down(&sc);
                logfile(LOG_WARNING,
                        _("Putting MASTER DOWN (going to time out)"));
                break;
            }

            /*
             * Otherwise, we reset the counter and wait for the next
             * advertisement.
             */
            carp_setrun(&sc, AF_INET); /* XXX - TODO was af */
            break;
        }
        break;
    }
    default:
        return;
    }
}

static void udpu_dispatch(void)
{
	struct pcap_pkthdr header;
	unsigned char udpu_buf[1500];
	struct sockaddr_in peeraddr;
	unsigned int peeraddr_len = sizeof(peeraddr);
	int rc;

	rc = recvfrom(udpu_fd, udpu_buf, sizeof(udpu_buf), 0,
                    (struct sockaddr *)&peeraddr,
                    &peeraddr_len);
	if (rc < 0)
	{
		logfile(LOG_WARNING, "recvfrom() failed [%d]", rc);
		return;
	}

	logfile(LOG_DEBUG, "received frame size: <%d>\n", rc);
	logfile(LOG_DEBUG, "from address %s and port %d\n",
             inet_ntoa(peeraddr.sin_addr),
             ntohs(peeraddr.sin_port));
	/* todo: should check srcip and port although we have secret passwd */

	memset(&header, 0, sizeof(struct pcap_pkthdr));
	header.caplen = rc;
	header.len = rc;
#if 0
	/* timestamp seems to be unused */
	if (gettimeofday(&header.ts, NULL) != 0) {
        logfile(LOG_WARNING, _("gettimeofday() for udpu failed: %s"),
                        strerror(errno));
        return;
    }
#endif
	packethandler(NULL, &header, udpu_buf);
}



static RETSIGTYPE sighandler_exit(const int sig)
{
    received_signal=15;
}

static RETSIGTYPE sighandler_usr(const int sig)
{
    switch (sig) {
    case SIGUSR1:
        received_signal=1;
        break;
    case SIGUSR2:
        received_signal=2;
        break;
    }
}

static char *build_bpf_rule(void)
{
    static char rule[256];
    const char *srcip_s;

    if ((srcip_s = inet_ntoa(srcip)) == NULL) {
        logfile(LOG_ERR, "inet_ntoa: [%s]", strerror(errno));
        return NULL;
    }

#if 0
	/* use a pcap fitering rule that is very unlikely to match ?!? */
	if(udpu_addr==NULL)
		snprintf(rule, sizeof rule, "proto %u and src port 9 and src port 10",
             (unsigned int) IPPROTO_CARP);
	else
#endif

	snprintf(rule, sizeof rule, "proto %u and src host not %s",
             (unsigned int) IPPROTO_CARP, srcip_s);
    logfile(LOG_DEBUG, "BPF rule: [%s]", rule);

    return rule;
}

static void udp_broadcast_dispatch(void)
{
  char buffer[3200];
  int status, buflen;
  unsigned sinlen;
  struct sockaddr_in sock_in;
  int rc = 0;
  struct pcap_pkthdr header;
  fd_set readset;

  sinlen = sizeof(struct sockaddr_in);
  memset(&sock_in, 0, sinlen);

  buflen = sizeof(buffer);
  memset(buffer, 0, buflen);


  if ( (rc = recvfrom(broadcast_fd, buffer, buflen, 0, (struct sockaddr *)&sock_in, &sinlen)) < 0 ) {
    logfile(LOG_ERR, "recvfrom failed: %d", rc);
    return;
  }
  if ( rc > 0 ) {
    memset(&header, 0, sizeof(struct pcap_pkthdr));
    header.caplen = rc;
    header.len = rc;
    packethandler(NULL, &header, buffer);
  }
}

int docarp(void)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    struct ip_mreq req_add;
    struct bpf_program bpfp;
    struct pollfd pfds[1];
    struct ifreq iface;
    int fd;
    int nfds;
    int iface_running = 1;
    int poll_sleep_time;
    struct timeval time_until_advert;
    struct sigaction usr_action;
    struct sigaction term_action;
    int yes = 1;
    struct sockaddr_in sock_in;
	struct sockaddr_in udpu_orig;

    sc.sc_vhid = vhid;
    sc.sc_advbase = advbase;
    sc.sc_advskew = advskew;
    sc.sc_init_counter = 1;
    sc.sc_delayed_arp = -1;
#ifdef INET6
    sc.sc_im6o.im6o_multicast_hlim = CARP_DFLTTL;
#endif /* INET6 */
    carp_set_state(&sc, INIT);
    {
        const size_t passlen = strlen(pass) + (size_t) 1U;

        if (passlen > sizeof sc.sc_key) {
            logfile(LOG_ERR, _("Password too long"));
            return -1;
        }
        memcpy(sc.sc_key, pass, passlen);
    }
    sc.sc_ad_tmo.tv_sec = 0;
    sc.sc_ad_tmo.tv_usec = 0;
    sc.sc_md_tmo.tv_sec = 0;
    sc.sc_md6_tmo.tv_usec = 0;

    carp_hmac_prepare(&sc);

    if (fill_mac_address() != 0) {
        logfile(LOG_ERR, _("Unable to find MAC address of [%s]"),
                interface == NULL ? "-" : interface);
        return -1;
    }
    logfile(LOG_INFO, _("Local advertised ethernet address is "
                        "[%02x:%02x:%02x:%02x:%02x:%02x]"),
            (unsigned int) hwaddr[0], (unsigned int) hwaddr[1],
            (unsigned int) hwaddr[2], (unsigned int) hwaddr[3],
            (unsigned int) hwaddr[4], (unsigned int) hwaddr[5]);
    if ((dev_desc = pcap_open_live(interface, ETHERNET_MTU, 0,
                                   CAPTURE_TIMEOUT, errbuf)) == NULL) {
        logfile(LOG_ERR, _("Unable to open interface [%s]: %s"),
                interface == NULL ? "-" : interface, errbuf);
        return -1;
    }
    if (pcap_compile(dev_desc, &bpfp, build_bpf_rule(),
                     1, (bpf_u_int32) 0) != 0) {
        logfile(LOG_ERR, _("Unable to compile pcap rule: %s [%s] (%s)"),
                errbuf, interface == NULL ? "-" : interface,
				pcap_geterr(dev_desc));
        return -1;
    }
    pcap_setfilter(dev_desc, &bpfp);
    dev_desc_fd = pcap_fileno(dev_desc);
    pfds[0].fd = dev_desc_fd;
    pfds[0].events = POLLIN | POLLERR | POLLHUP;
	pfds[0].revents = 0;
    
    if (shutdown_at_exit != 0) {
        (void) sigemptyset(&term_action.sa_mask);
        term_action.sa_handler = sighandler_exit;
        term_action.sa_flags = SA_NODEFER;

        if (sigaction(SIGINT, &term_action, NULL) < 0) {
            logfile(LOG_ERR,
               "Error when trying register SIGINT handler: %s",
               strerror(errno));
            return -1;
        }
        if (sigaction(SIGQUIT, &term_action, NULL) < 0) {
            logfile(LOG_ERR,
               "Error when trying register SIGQUIT handler: %s",
               strerror(errno));
            return -1;
        }
        if (sigaction(SIGTERM, &term_action, NULL) < 0) {
            logfile(LOG_ERR,
               "Error when trying register SIGTERM handler: %s",
               strerror(errno));
            return -1;
        }
        if (sigaction(SIGHUP, &term_action, NULL) < 0) {
            logfile(LOG_ERR,
               "Error when trying register SIGHUP handler: %s",
               strerror(errno));
            return -1;
        }
    }

    (void) sigemptyset(&usr_action.sa_mask);
    usr_action.sa_handler = sighandler_usr;
    usr_action.sa_flags = SA_NODEFER;

    if (sigaction(SIGUSR1, &usr_action, NULL) < 0) {
        logfile(LOG_ERR,
           "Error when trying register SIGUSR1 handler: %s",
           strerror(errno));
        return -1;
    }
    if (sigaction(SIGUSR2, &usr_action, NULL) < 0) {
        logfile(LOG_ERR,
           "Error when trying register SIGUSR2 handler: %s",
           strerror(errno));
        return -1;
    }

    if (gettimeofday(&now, NULL) != 0) {
        logfile(LOG_WARNING, _("initializing now to gettimeofday() failed: %s"),
                strerror(errno));
    }
    carp_setrun(&sc, 0);

    if ((fd = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
        logfile(LOG_ERR, _("Error opening socket for interface [%s]: %s"),
                interface == NULL ? "-" : interface, strerror(errno));
        return -1;
    }
    if (!no_mcast) {
        memset(&req_add, 0, sizeof req_add);
        req_add.imr_multiaddr.s_addr = mcastip.s_addr;
        req_add.imr_interface.s_addr = srcip.s_addr;
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &req_add, sizeof req_add) < 0) {
            logfile(LOG_ERR, "Can't do IP_ADD_MEMBERSHIP errno=%s (%d)",
                    strerror(errno), errno);
            close(fd);
            return -1;
        }
    } else {
      if ((broadcast_fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
          logfile(LOG_ERR, _("Error opening socket for interface [%s]: %s"),
                  interface == NULL ? "-" : interface, strerror(errno));
          return -1;
      }
      sock_in.sin_addr.s_addr = htonl(INADDR_ANY);
      sock_in.sin_port = htons(port);
      sock_in.sin_family = PF_INET;

      if ((bind(broadcast_fd, (struct sockaddr *)&sock_in, sizeof(struct sockaddr_in))) == -1) {
        logfile(LOG_ERR, _("Error binding to socket: %s"), strerror(errno));
        return -1;
      }

      if ( setsockopt(broadcast_fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(int) ) == -1 ) {
        logfile(LOG_ERR, _("Error setting broadcast: %s"), strerror(errno));
        return -1;
      }
       pfds[0].fd = broadcast_fd;

    }
	if(udpu_addr!=NULL) {
		/* socket for udpu communication */
		if ((udpu_fd = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
			logfile(LOG_ERR, _("Error opening socket for udpu on [%s]: %s"),
               interface == NULL ? "-" : interface, strerror(errno));
			return -1;
		}

		/* bind to local interface and port */
		memset(&udpu_orig, 0, sizeof(struct sockaddr_in));
		udpu_orig.sin_family = AF_INET;
		udpu_orig.sin_addr.s_addr = inet_addr(srcip_str);
		udpu_orig.sin_port = htons(udpu_port);
		if (bind(udpu_fd, (struct sockaddr *)&udpu_orig,
								sizeof(struct sockaddr_in)) == -1)
		{
			logfile(LOG_ERR,
				"Can't do bind to local socket (%s:%u) [errno=%s (%d)]",
					srcip_str, udpu_port,
                    strerror(errno), errno);
			close(udpu_fd);
			return -1;
		}
		/* build udpu destination address */
		memset(&udpu_dest, 0, sizeof(struct sockaddr_in));
		udpu_dest.sin_family = AF_INET;
		udpu_dest.sin_addr.s_addr = inet_addr(udpu_addr);
		udpu_dest.sin_port = htons(udpu_port);

		/* poll switched to listen on this socket */
		pfds[0].fd = udpu_fd;
	}
#ifdef SIOCGIFFLAGS    
    if (strlen(interface) >= sizeof iface.ifr_name) {
        logfile(LOG_ERR, _("Interface name too long"));
        return -1;
    }
    strncpy(iface.ifr_name, interface, sizeof iface.ifr_name);
#endif
    for (;;) {
#ifdef SIOCGIFFLAGS
        if (ioctl(fd, SIOCGIFFLAGS, &iface) != 0) {
            break;
        }
        if ((iface.ifr_flags & IFF_RUNNING) == 0) {
            if (ignoreifstate == 0) {
                carp_set_state(&sc, BACKUP);
                sc.sc_ad_tmo.tv_sec = 0;
                sc.sc_ad_tmo.tv_usec = 0;
                sc.sc_md_tmo.tv_sec = 0;
                sc.sc_md6_tmo.tv_usec = 0;
                if (iface_running) {
                    iface_running = 0;
                }
                sleep(SECONDS_TO_WAIT_AFTER_INTERFACE_IS_DOWN);
                continue;
            }
        } else {
            if (!iface_running) {
                iface_running = 1;
                carp_setrun(&sc, 0);
            }
        }
#endif
        if (received_signal != 0) {
            int flag = received_signal;

            received_signal = 0;
            switch (flag) {
            case 1:
                logfile(LOG_INFO, "%s on %s id %d",
                        (sc.sc_state == BACKUP ? "BACKUP" : "MASTER"),
                        interface, sc.sc_vhid);
                break;
            case 2:
                logfile(LOG_DEBUG, "Caught signal (USR2) considering going down");
                if (sc.sc_state != BACKUP) {
                    carp_set_state(&sc, BACKUP);
                    sleep(3); /* hold up a sec... */
                    carp_setrun(&sc, 0); /* now listen for 3 heartbeats, as usual */
                    continue;
                }
                break;
            case 15:
                logfile(LOG_DEBUG, "sighandler_exit(): Calling [%s] and exiting",
                        downscript);
                if (sc.sc_state != BACKUP) {
                    (void) spawn_handler(dev_desc_fd, downscript);
                }
                _exit(EXIT_SUCCESS);
                break;
            }
        }

        if (sc.sc_ad_tmo.tv_sec == 0) {
            unsigned int tmpskew = advskew * 1000 / 256;
            poll_sleep_time = sc.sc_advbase * 1000 + tmpskew;
        } else {
            if (gettimeofday(&now, NULL) != 0) {
                logfile(LOG_WARNING, _("gettimeofday() failed: %s"),
                        strerror(errno));
                continue;
            }
            timersub(&sc.sc_ad_tmo, &now, &time_until_advert);
            poll_sleep_time = (time_until_advert.tv_sec * 1000) +
                (time_until_advert.tv_usec / 1000);
        }
        nfds = poll(pfds, (nfds_t) 1, MAX(1, poll_sleep_time));
        if (nfds == -1) {
            if (errno == EINTR) {
               continue;
            }
            logfile(LOG_ERR, _("exiting: poll() error: %s"), strerror(errno));
        }
        if ((pfds[0].revents & (POLLERR | POLLHUP)) != 0) {
            logfile(LOG_ERR, _("exiting: pfds[0].revents = %d"),
                    pfds[0].revents);
            if ((sc.sc_state != BACKUP) && (shutdown_at_exit != 0)) {
                (void) spawn_handler(dev_desc_fd, downscript);
            }
            break;
        }
        if (gettimeofday(&now, NULL) != 0) {
            logfile(LOG_WARNING, _("gettimeofday() failed: %s"),
                    strerror(errno));
            continue;
        }
        if (nfds == 1) {
			if(udpu_addr==NULL) {
                if ( no_mcast ) {
                    udp_broadcast_dispatch();
                } else {
                    pcap_dispatch(dev_desc, 1, packethandler, NULL);
                }
            } else {
				udpu_dispatch();
            }
        }
        if (sc.sc_md_tmo.tv_sec != 0 && timercmp(&now, &sc.sc_md_tmo, >)) {
            carp_master_down(&sc);
        }
#ifdef INET6
        if (sc.sc_md6_tmo.tv_sec != 0 && timercmp(&now, &sc.sc_md6_tmo, >)) {
            carp_master_down(&sc);
        }
#endif
        if (sc.sc_ad_tmo.tv_sec != 0) {
           if (timercmp(&now, &sc.sc_ad_tmo, >)) {
                carp_send_ad(&sc);
           } else {
               timersub(&sc.sc_ad_tmo, &now, &time_until_advert);
               int diff_ms = (time_until_advert.tv_sec * 1000) +
                   (time_until_advert.tv_usec / 1000);
               if (abs(diff_ms) <= 1) {
                    carp_send_ad(&sc);
               }
           }
       }
    }
    pcap_close(dev_desc);
    pcap_freecode(&bpfp);

    return 0;
}
