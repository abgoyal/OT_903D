


#define WANT_SYS_IOCTL_H
#include <slirp.h>
#include "proxy_common.h"

/* patchable/settable parameters for tcp */
/* Don't do rfc1323 performance enhancements */
#define TCP_DO_RFC1323 0

void
tcp_init(void)
{
	tcp_iss = 1;		/* wrong */
	tcb.so_next = tcb.so_prev = &tcb;
}

/* struct tcpiphdr * */
void
tcp_template(struct tcpcb *tp)
{
	struct socket *so = tp->t_socket;
	register struct tcpiphdr *n = &tp->t_template;

	n->ti_mbuf = NULL;
	n->ti_x1 = 0;
	n->ti_pr = IPPROTO_TCP;
	n->ti_len = htons(sizeof (struct tcpiphdr) - sizeof (struct ip));
	n->ti_src = ip_seth(so->so_faddr_ip);
	n->ti_dst = ip_seth(so->so_laddr_ip);
	n->ti_sport = port_seth(so->so_faddr_port);
	n->ti_dport = port_seth(so->so_laddr_port);

	n->ti_seq = 0;
	n->ti_ack = 0;
	n->ti_x2 = 0;
	n->ti_off = 5;
	n->ti_flags = 0;
	n->ti_win = 0;
	n->ti_sum = 0;
	n->ti_urp = 0;
}

void
tcp_respond(struct tcpcb *tp, struct tcpiphdr *ti, struct mbuf *m,
            tcp_seq ack, tcp_seq seq, int flags)
{
	register int tlen;
	int win = 0;

	DEBUG_CALL("tcp_respond");
	DEBUG_ARG("tp = %lx", (long)tp);
	DEBUG_ARG("ti = %lx", (long)ti);
	DEBUG_ARG("m = %lx", (long)m);
	DEBUG_ARG("ack = %u", ack);
	DEBUG_ARG("seq = %u", seq);
	DEBUG_ARG("flags = %x", flags);

	if (tp)
		win = sbspace(&tp->t_socket->so_rcv);
        if (m == NULL) {
		if ((m = m_get()) == NULL)
			return;
#ifdef TCP_COMPAT_42
		tlen = 1;
#else
		tlen = 0;
#endif
		m->m_data += IF_MAXLINKHDR;
		*mtod(m, struct tcpiphdr *) = *ti;
		ti = mtod(m, struct tcpiphdr *);
		flags = TH_ACK;
	} else {
		/*
		 * ti points into m so the next line is just making
		 * the mbuf point to ti
		 */
		m->m_data = (caddr_t)ti;

		m->m_len = sizeof (struct tcpiphdr);
		tlen = 0;
#define xchg(a,b,type) { type t; t=a; a=b; b=t; }
		xchg(ti->ti_dst, ti->ti_src, ipaddr_t);
		xchg(ti->ti_dport, ti->ti_sport, port_t);
#undef xchg
	}
	ti->ti_len = htons((u_short)(sizeof (struct tcphdr) + tlen));
	tlen += sizeof (struct tcpiphdr);
	m->m_len = tlen;

        ti->ti_mbuf = NULL;
	ti->ti_x1 = 0;
	ti->ti_seq = htonl(seq);
	ti->ti_ack = htonl(ack);
	ti->ti_x2 = 0;
	ti->ti_off = sizeof (struct tcphdr) >> 2;
	ti->ti_flags = flags;
	if (tp)
		ti->ti_win = htons((u_int16_t) (win >> tp->rcv_scale));
	else
		ti->ti_win = htons((u_int16_t)win);
	ti->ti_urp = 0;
	ti->ti_sum = 0;
	ti->ti_sum = cksum(m, tlen);
	((struct ip *)ti)->ip_len = tlen;

	if(flags & TH_RST)
	  ((struct ip *)ti)->ip_ttl = MAXTTL;
	else
	  ((struct ip *)ti)->ip_ttl = IPDEFTTL;

	(void) ip_output((struct socket *)0, m);
}

struct tcpcb *
tcp_newtcpcb(struct socket *so)
{
	register struct tcpcb *tp;

	tp = (struct tcpcb *)malloc(sizeof(*tp));
	if (tp == NULL)
		return ((struct tcpcb *)0);

	memset((char *) tp, 0, sizeof(struct tcpcb));
	tp->seg_next = tp->seg_prev = (struct tcpiphdr*)tp;
	tp->t_maxseg = TCP_MSS;

	tp->t_flags = TCP_DO_RFC1323 ? (TF_REQ_SCALE|TF_REQ_TSTMP) : 0;
	tp->t_socket = so;

	/*
	 * Init srtt to TCPTV_SRTTBASE (0), so we can tell that we have no
	 * rtt estimate.  Set rttvar so that srtt + 2 * rttvar gives
	 * reasonable initial retransmit time.
	 */
	tp->t_srtt = TCPTV_SRTTBASE;
	tp->t_rttvar = TCPTV_SRTTDFLT << 2;
	tp->t_rttmin = TCPTV_MIN;

	TCPT_RANGESET(tp->t_rxtcur,
	    ((TCPTV_SRTTBASE >> 2) + (TCPTV_SRTTDFLT << 2)) >> 1,
	    TCPTV_MIN, TCPTV_REXMTMAX);

	tp->snd_cwnd = TCP_MAXWIN << TCP_MAX_WINSHIFT;
	tp->snd_ssthresh = TCP_MAXWIN << TCP_MAX_WINSHIFT;
	tp->t_state = TCPS_CLOSED;

	so->so_tcpcb = tp;

	return (tp);
}

struct tcpcb *tcp_drop(struct tcpcb *tp, int err)
{

	DEBUG_CALL("tcp_drop");
	DEBUG_ARG("tp = %lx", (long)tp);
	DEBUG_ARG("errno = %d", errno);

	if (TCPS_HAVERCVDSYN(tp->t_state)) {
		tp->t_state = TCPS_CLOSED;
		(void) tcp_output(tp);
		STAT(tcpstat.tcps_drops++);
	} else
		STAT(tcpstat.tcps_conndrops++);
/*	so->so_error = errno; */
	return (tcp_close(tp));
}

struct tcpcb *
tcp_close(struct tcpcb *tp)
{
	register struct tcpiphdr *t;
	struct socket *so = tp->t_socket;
	register struct mbuf *m;

	DEBUG_CALL("tcp_close");
	DEBUG_ARG("tp = %lx", (long )tp);

	/* free the reassembly queue, if any */
	t = tcpfrag_list_first(tp);
	while (!tcpfrag_list_end(t, tp)) {
		t = tcpiphdr_next(t);
		m = tcpiphdr_prev(t)->ti_mbuf;
		remque(tcpiphdr2qlink(tcpiphdr_prev(t)));
		m_freem(m);
	}
	/* It's static */
/*	free(tp, M_PCB);  */
	free(tp);
        so->so_tcpcb = NULL;
	soisfdisconnected(so);
	/* clobber input socket cache if we're closing the cached connection */
	if (so == tcp_last_so)
		tcp_last_so = &tcb;
	socket_close(so->s);
	sbfree(&so->so_rcv);
	sbfree(&so->so_snd);
	sofree(so);
	STAT(tcpstat.tcps_closed++);
	return ((struct tcpcb *)0);
}

#ifdef notdef
void
tcp_drain()
{
	/* XXX */
}

void
tcp_quench(i, errno)

	int errno;
{
	struct tcpcb *tp = intotcpcb(inp);

	if (tp)
		tp->snd_cwnd = tp->t_maxseg;
}

#endif /* notdef */


void
tcp_sockclosed(struct tcpcb *tp)
{

	DEBUG_CALL("tcp_sockclosed");
	DEBUG_ARG("tp = %lx", (long)tp);

	switch (tp->t_state) {

	case TCPS_CLOSED:
	case TCPS_LISTEN:
	case TCPS_SYN_SENT:
		tp->t_state = TCPS_CLOSED;
		tp = tcp_close(tp);
		break;

	case TCPS_SYN_RECEIVED:
	case TCPS_ESTABLISHED:
		tp->t_state = TCPS_FIN_WAIT_1;
		break;

	case TCPS_CLOSE_WAIT:
		tp->t_state = TCPS_LAST_ACK;
		break;
	}
/*	soisfdisconnecting(tp->t_socket); */
	if (tp && tp->t_state >= TCPS_FIN_WAIT_2)
		soisfdisconnected(tp->t_socket);
	if (tp)
		tcp_output(tp);
}

static void
tcp_proxy_event( struct socket*  so,
                 int             s,
                 ProxyEvent      event )
{
    so->so_state &= ~SS_PROXIFIED;

    if (event == PROXY_EVENT_CONNECTED) {
        so->s         = s;
        so->so_state &= ~(SS_ISFCONNECTING);
    }
    else {
        so->so_state = SS_NOFDREF;
    }

    /* continue the connect */
    tcp_input(NULL, sizeof(struct ip), so);
}

int tcp_fconnect(struct socket *so)
{
  int ret=0;
    int try_proxy = 1;
    SockAddress    sockaddr;
    uint32_t       sock_ip;
    uint16_t       sock_port;

    DEBUG_CALL("tcp_fconnect");
    DEBUG_ARG("so = %lx", (long )so);

    sock_ip   = so->so_faddr_ip;
    sock_port = so->so_faddr_port;

    if ((sock_ip & 0xffffff00) == special_addr_ip) {
      /* It's an alias */
      int  last_byte = sock_ip & 0xff;

      if (CTL_IS_DNS(last_byte))
        sock_ip = dns_addr[last_byte - CTL_DNS];
      else
        sock_ip = loopback_addr_ip;
      try_proxy = 0;
    }

    sock_address_init_inet( &sockaddr, sock_ip, sock_port );

    DEBUG_MISC((dfd, " connect()ing, addr=%s, proxy=%d\n",
                sock_address_to_string(&sockaddr), try_proxy));

    if (try_proxy) {
        if (!proxy_manager_add(&sockaddr, SOCKET_STREAM, (ProxyEventFunc) tcp_proxy_event, so)) {
            soisfconnecting(so);
            so->s         = -1;
            so->so_state |= SS_PROXIFIED;
            return 0;
        }
    }

    if ((ret=so->s=socket_create_inet(SOCKET_STREAM)) >= 0) 
    {
        int  s = so->s;

        socket_set_nonblock(s);
        socket_set_xreuseaddr(s);
        socket_set_oobinline(s);

        /* We don't care what port we get */
        socket_connect(s, &sockaddr);

        /*
        * If it's not in progress, it failed, so we just return 0,
        * without clearing SS_NOFDREF
        */
        soisfconnecting(so);
  }

  return(ret);
}

void
tcp_connect(struct socket *inso)
{
	struct socket *so;
	SockAddress   addr;
	uint32_t      addr_ip;
	struct tcpcb *tp;
	int s;

	DEBUG_CALL("tcp_connect");
	DEBUG_ARG("inso = %lx", (long)inso);

	/*
	 * If it's an SS_ACCEPTONCE socket, no need to socreate()
	 * another socket, just use the accept() socket.
	 */
	if (inso->so_state & SS_FACCEPTONCE) {
		/* FACCEPTONCE already have a tcpcb */
		so = inso;
	} else {
		if ((so = socreate()) == NULL) {
			/* If it failed, get rid of the pending connection */
			socket_close(socket_accept(inso->s, NULL));
			return;
		}
		if (tcp_attach(so) < 0) {
			free(so); /* NOT sofree */
			return;
		}
		so->so_laddr_ip   = inso->so_laddr_ip;
		so->so_laddr_port = inso->so_laddr_port;
	}

	(void) tcp_mss(sototcpcb(so), 0);

	if ((s = socket_accept(inso->s, &addr)) < 0) {
		tcp_close(sototcpcb(so)); /* This will sofree() as well */
		return;
	}
	socket_set_nonblock(s);
	socket_set_xreuseaddr(s);
	socket_set_oobinline(s);
	socket_set_nodelay(s);

	so->so_faddr_port = sock_address_get_port(&addr);

	addr_ip = sock_address_get_ip(&addr);

	so->so_faddr_ip = addr_ip;
	/* Translate connections from localhost to the real hostname */
	if (addr_ip == 0 || addr_ip == loopback_addr_ip)
	   so->so_faddr_ip = alias_addr_ip;

	/* Close the accept() socket, set right state */
	if (inso->so_state & SS_FACCEPTONCE) {
		socket_close(so->s); /* If we only accept once, close the accept() socket */
		so->so_state = SS_NOFDREF; /* Don't select it yet, even though we have an FD */
					   /* if it's not FACCEPTONCE, it's already NOFDREF */
	}
	so->s = s;

	so->so_iptos = tcp_tos(so);
	tp = sototcpcb(so);

	tcp_template(tp);

	/* Compute window scaling to request.  */

/*	soisconnecting(so); */ /* NOFDREF used instead */
	STAT(tcpstat.tcps_connattempt++);

	tp->t_state = TCPS_SYN_SENT;
	tp->t_timer[TCPT_KEEP] = TCPTV_KEEP_INIT;
	tp->iss = tcp_iss;
	tcp_iss += TCP_ISSINCR/2;
	tcp_sendseqinit(tp);
	tcp_output(tp);
}

int
tcp_attach(struct socket *so)
{
	if ((so->so_tcpcb = tcp_newtcpcb(so)) == NULL)
	   return -1;

	insque(so, &tcb);

	return 0;
}

static const struct tos_t tcptos[] = {
	  {0, 20, IPTOS_THROUGHPUT, 0},	/* ftp data */
	  {21, 21, IPTOS_LOWDELAY,  EMU_FTP},	/* ftp control */
	  {0, 23, IPTOS_LOWDELAY, 0},	/* telnet */
	  {0, 80, IPTOS_THROUGHPUT, 0},	/* WWW */
	  {0, 513, IPTOS_LOWDELAY, EMU_RLOGIN|EMU_NOCONNECT},	/* rlogin */
	  {0, 514, IPTOS_LOWDELAY, EMU_RSH|EMU_NOCONNECT},	/* shell */
	  {0, 544, IPTOS_LOWDELAY, EMU_KSH},		/* kshell */
	  {0, 543, IPTOS_LOWDELAY, 0},	/* klogin */
	  {0, 6667, IPTOS_THROUGHPUT, EMU_IRC},	/* IRC */
	  {0, 6668, IPTOS_THROUGHPUT, EMU_IRC},	/* IRC undernet */
	  {0, 7070, IPTOS_LOWDELAY, EMU_REALAUDIO }, /* RealAudio control */
	  {0, 113, IPTOS_LOWDELAY, EMU_IDENT }, /* identd protocol */
	  {0, 0, 0, 0}
};

#ifdef CONFIG_QEMU
static
#endif
struct emu_t *tcpemu = NULL;

u_int8_t
tcp_tos(struct socket *so)
{
	int i = 0;
	struct emu_t *emup;

	while(tcptos[i].tos) {
		if ((tcptos[i].fport && so->so_faddr_port == tcptos[i].fport) ||
		    (tcptos[i].lport && so->so_laddr_port == tcptos[i].lport)) {
			so->so_emu = tcptos[i].emu;
			return tcptos[i].tos;
		}
		i++;
	}

	/* Nope, lets see if there's a user-added one */
	for (emup = tcpemu; emup; emup = emup->next) {
		if ((emup->fport && (so->so_faddr_port == emup->fport)) ||
		    (emup->lport && (so->so_laddr_port == emup->lport))) {
			so->so_emu = emup->emu;
			return emup->tos;
		}
	}
	return 0;
}

#if 0
int do_echo = -1;
#endif

int
tcp_emu(struct socket *so, struct mbuf *m)
{
	u_int n1, n2, n3, n4, n5, n6;
        char buff[257];
	u_int32_t laddr;
	u_int lport;
	char *bptr;

	DEBUG_CALL("tcp_emu");
	DEBUG_ARG("so = %lx", (long)so);
	DEBUG_ARG("m = %lx", (long)m);

	switch(so->so_emu) {
		int x, i;

	 case EMU_IDENT:
		/*
		 * Identification protocol as per rfc-1413
		 */

		{
			struct socket *tmpso;
			SockAddress    addr;
			struct sbuf *so_rcv = &so->so_rcv;

			memcpy(so_rcv->sb_wptr, m->m_data, m->m_len);
			so_rcv->sb_wptr += m->m_len;
			so_rcv->sb_rptr += m->m_len;
			m->m_data[m->m_len] = 0; /* NULL terminate */
			if (strchr(m->m_data, '\r') || strchr(m->m_data, '\n')) {
				if (sscanf(so_rcv->sb_data, "%u%*[ ,]%u", &n1, &n2) == 2) {
					/* n2 is the one on our host */
					for (tmpso = tcb.so_next; tmpso != &tcb; tmpso = tmpso->so_next) {
						if (tmpso->so_laddr_ip == so->so_laddr_ip &&
						    tmpso->so_laddr_port == n2 &&
						    tmpso->so_faddr_ip == so->so_faddr_ip &&
						    tmpso->so_faddr_port == n1) {
							if (socket_get_address(tmpso->s, &addr) == 0)
							   n2 = sock_address_get_port(&addr);
							break;
						}
					}
				}
                                so_rcv->sb_cc = snprintf(so_rcv->sb_data,
                                                         so_rcv->sb_datalen,
                                                         "%d,%d\r\n", n1, n2);
				so_rcv->sb_rptr = so_rcv->sb_data;
				so_rcv->sb_wptr = so_rcv->sb_data + so_rcv->sb_cc;
			}
			m_free(m);
			return 0;
		}

        case EMU_FTP: /* ftp */
                *(m->m_data+m->m_len) = 0; /* NUL terminate for strstr */
		if ((bptr = (char *)strstr(m->m_data, "ORT")) != NULL) {
			/*
			 * Need to emulate the PORT command
			 */
			x = sscanf(bptr, "ORT %u,%u,%u,%u,%u,%u\r\n%256[^\177]",
				   &n1, &n2, &n3, &n4, &n5, &n6, buff);
			if (x < 6)
			   return 1;

			laddr = (n1 << 24) | (n2 << 16) | (n3 << 8) | (n4);
			lport = (n5 << 8) | (n6);

			if ((so = solisten(0, laddr, lport, SS_FACCEPTONCE)) == NULL)
			   return 1;

			n6 = so->so_faddr_port;

			n5 = (n6 >> 8) & 0xff;
			n6 &= 0xff;

			laddr = so->so_faddr_ip;

			n1 = ((laddr >> 24) & 0xff);
			n2 = ((laddr >> 16) & 0xff);
			n3 = ((laddr >> 8)  & 0xff);
			n4 =  (laddr & 0xff);

			m->m_len = bptr - m->m_data; /* Adjust length */
                        m->m_len += snprintf(bptr, m->m_hdr.mh_size - m->m_len,
                                             "ORT %d,%d,%d,%d,%d,%d\r\n%s",
                                             n1, n2, n3, n4, n5, n6, x==7?buff:"");
			return 1;
		} else if ((bptr = (char *)strstr(m->m_data, "27 Entering")) != NULL) {
			/*
			 * Need to emulate the PASV response
			 */
			x = sscanf(bptr, "27 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n%256[^\177]",
				   &n1, &n2, &n3, &n4, &n5, &n6, buff);
			if (x < 6)
			   return 1;

			laddr = (n1 << 24) | (n2 << 16) | (n3 << 8) | (n4);
			lport = (n5 << 8) | (n6);

			if ((so = solisten(0, laddr, lport, SS_FACCEPTONCE)) == NULL)
			   return 1;

			n6 = so->so_faddr_port;

			n5 = (n6 >> 8) & 0xff;
			n6 &= 0xff;

			laddr = so->so_faddr_ip;

			n1 = ((laddr >> 24) & 0xff);
			n2 = ((laddr >> 16) & 0xff);
			n3 = ((laddr >> 8)  & 0xff);
			n4 =  (laddr & 0xff);

			m->m_len = bptr - m->m_data; /* Adjust length */
			m->m_len += snprintf(bptr, m->m_hdr.mh_size - m->m_len,
                                             "27 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\r\n%s",
                                             n1, n2, n3, n4, n5, n6, x==7?buff:"");

			return 1;
		}

		return 1;

	 case EMU_KSH:
		/*
		 * The kshell (Kerberos rsh) and shell services both pass
		 * a local port port number to carry signals to the server
		 * and stderr to the client.  It is passed at the beginning
		 * of the connection as a NUL-terminated decimal ASCII string.
		 */
		so->so_emu = 0;
		for (lport = 0, i = 0; i < m->m_len-1; ++i) {
			if (m->m_data[i] < '0' || m->m_data[i] > '9')
				return 1;       /* invalid number */
			lport *= 10;
			lport += m->m_data[i] - '0';
		}
		if (m->m_data[m->m_len-1] == '\0' && lport != 0 &&
		    (so = solisten(0, so->so_laddr_ip, lport, SS_FACCEPTONCE)) != NULL)
			m->m_len = snprintf(m->m_data, m->m_hdr.mh_size, "%d",
                                so->so_faddr_port) + 1;
		return 1;

	 case EMU_IRC:
		/*
		 * Need to emulate DCC CHAT, DCC SEND and DCC MOVE
		 */
		*(m->m_data+m->m_len) = 0; /* NULL terminate the string for strstr */
		if ((bptr = (char *)strstr(m->m_data, "DCC")) == NULL)
			 return 1;

		/* The %256s is for the broken mIRC */
		if (sscanf(bptr, "DCC CHAT %256s %u %u", buff, &laddr, &lport) == 3) {
			if ((so = solisten(0, laddr, lport, SS_FACCEPTONCE)) == NULL)
				return 1;

			m->m_len = bptr - m->m_data; /* Adjust length */
                        m->m_len += snprintf(bptr, m->m_hdr.mh_size,
                                             "DCC CHAT chat %lu %u%c\n",
			     (unsigned long) so->so_faddr_ip,
			     so->so_faddr_port, 1);
		} else if (sscanf(bptr, "DCC SEND %256s %u %u %u", buff, &laddr, &lport, &n1) == 4) {
			if ((so = solisten(0, laddr, lport, SS_FACCEPTONCE)) == NULL)
				return 1;

			m->m_len = bptr - m->m_data; /* Adjust length */
                        m->m_len += snprintf(bptr, m->m_hdr.mh_size,
                                             "DCC SEND %s %u %u %u%c\n", buff,
			      so->so_faddr_ip, so->so_faddr_port, n1, 1);
		} else if (sscanf(bptr, "DCC MOVE %256s %u %u %u", buff, &laddr, &lport, &n1) == 4) {
			if ((so = solisten(0, laddr, lport, SS_FACCEPTONCE)) == NULL)
				return 1;

			m->m_len = bptr - m->m_data; /* Adjust length */
                        m->m_len += snprintf(bptr, m->m_hdr.mh_size,
                                             "DCC MOVE %s %lu %u %u%c\n", buff,
			      (unsigned long)so->so_faddr_ip,
			      so->so_faddr_port, n1, 1);
		}
		return 1;

	 case EMU_REALAUDIO:
                /*
		 * RealAudio emulation - JP. We must try to parse the incoming
		 * data and try to find the two characters that contain the
		 * port number. Then we redirect an udp port and replace the
		 * number with the real port we got.
		 *
		 * The 1.0 beta versions of the player are not supported
		 * any more.
		 *
		 * A typical packet for player version 1.0 (release version):
		 *
		 * 0000:50 4E 41 00 05
		 * 0000:00 01 00 02 1B D7 00 00 67 E6 6C DC 63 00 12 50 .....×..gælÜc..P
		 * 0010:4E 43 4C 49 45 4E 54 20 31 30 31 20 41 4C 50 48 NCLIENT 101 ALPH
		 * 0020:41 6C 00 00 52 00 17 72 61 66 69 6C 65 73 2F 76 Al..R..rafiles/v
		 * 0030:6F 61 2F 65 6E 67 6C 69 73 68 5F 2E 72 61 79 42 oa/english_.rayB
		 *
		 * Now the port number 0x1BD7 is found at offset 0x04 of the
		 * Now the port number 0x1BD7 is found at offset 0x04 of the
		 * second packet. This time we received five bytes first and
		 * then the rest. You never know how many bytes you get.
		 *
		 * A typical packet for player version 2.0 (beta):
		 *
		 * 0000:50 4E 41 00 06 00 02 00 00 00 01 00 02 1B C1 00 PNA...........Á.
		 * 0010:00 67 75 78 F5 63 00 0A 57 69 6E 32 2E 30 2E 30 .guxõc..Win2.0.0
		 * 0020:2E 35 6C 00 00 52 00 1C 72 61 66 69 6C 65 73 2F .5l..R..rafiles/
		 * 0030:77 65 62 73 69 74 65 2F 32 30 72 65 6C 65 61 73 website/20releas
		 * 0040:65 2E 72 61 79 53 00 00 06 36 42                e.rayS...6B
		 *
		 * Port number 0x1BC1 is found at offset 0x0d.
		 *
		 * This is just a horrible switch statement. Variable ra tells
		 * us where we're going.
		 */

		bptr = m->m_data;
		while (bptr < m->m_data + m->m_len) {
			u_short p;
			static int ra = 0;
			char ra_tbl[4];

			ra_tbl[0] = 0x50;
			ra_tbl[1] = 0x4e;
			ra_tbl[2] = 0x41;
			ra_tbl[3] = 0;

			switch (ra) {
			 case 0:
			 case 2:
			 case 3:
				if (*bptr++ != ra_tbl[ra]) {
					ra = 0;
					continue;
				}
				break;

			 case 1:
				/*
				 * We may get 0x50 several times, ignore them
				 */
				if (*bptr == 0x50) {
					ra = 1;
					bptr++;
					continue;
				} else if (*bptr++ != ra_tbl[ra]) {
					ra = 0;
					continue;
				}
				break;

			 case 4:
				/*
				 * skip version number
				 */
				bptr++;
				break;

			 case 5:
				/*
				 * The difference between versions 1.0 and
				 * 2.0 is here. For future versions of
				 * the player this may need to be modified.
				 */
				if (*(bptr + 1) == 0x02)
				   bptr += 8;
				else
				   bptr += 4;
				break;

			 case 6:
				/* This is the field containing the port
				 * number that RA-player is listening to.
				 */
				lport = (((u_char*)bptr)[0] << 8)
				+ ((u_char *)bptr)[1];
				if (lport < 6970)
				   lport += 256;   /* don't know why */
				if (lport < 6970 || lport > 7170)
				   return 1;       /* failed */

				/* try to get udp port between 6970 - 7170 */
				for (p = 6970; p < 7071; p++) {
					if (udp_listen( p,
						       so->so_laddr_ip,
						       lport,
						       SS_FACCEPTONCE)) {
						break;
					}
				}
				if (p == 7071)
				   p = 0;
				*(u_char *)bptr++ = (p >> 8) & 0xff;
				*(u_char *)bptr++ = p & 0xff;
				ra = 0;
				return 1;   /* port redirected, we're done */
				break;

			 default:
				ra = 0;
			}
			ra++;
		}
		return 1;

	 default:
		/* Ooops, not emulated, won't call tcp_emu again */
		so->so_emu = 0;
		return 1;
	}
}

int
tcp_ctl(struct socket *so)
{
	struct sbuf *sb = &so->so_snd;
	int command;
 	struct ex_list *ex_ptr;
	int do_pty;
        //	struct socket *tmpso;

	DEBUG_CALL("tcp_ctl");
	DEBUG_ARG("so = %lx", (long )so);

#if 0
	/*
	 * Check if they're authorised
	 */
	if (ctl_addr_ip && (ctl_addr_ip == -1 || (so->so_laddr_ip != ctl_addr_ip))) {
		sb->sb_cc = sprintf(sb->sb_wptr,"Error: Permission denied.\r\n");
		sb->sb_wptr += sb->sb_cc;
		return 0;
	}
#endif
	command = (so->so_faddr_ip & 0xff);

	switch(command) {
	default: /* Check for exec's */

		/*
		 * Check if it's pty_exec
		 */
		for (ex_ptr = exec_list; ex_ptr; ex_ptr = ex_ptr->ex_next) {
			if (ex_ptr->ex_fport == so->so_faddr_port &&
			    command == ex_ptr->ex_addr) {
				if (ex_ptr->ex_pty == 3) {
					so->s = -1;
					so->extra = (void *)ex_ptr->ex_exec;
					return 1;
				}
				do_pty = ex_ptr->ex_pty;
				goto do_exec;
			}
		}

		/*
		 * Nothing bound..
		 */
		/* tcp_fconnect(so); */

		/* FALLTHROUGH */
	case CTL_ALIAS:
          sb->sb_cc = snprintf(sb->sb_wptr, sb->sb_datalen - (sb->sb_wptr - sb->sb_data),
                               "Error: No application configured.\r\n");
	  sb->sb_wptr += sb->sb_cc;
	  return(0);

	do_exec:
		DEBUG_MISC((dfd, " executing %s \n",ex_ptr->ex_exec));
		return(fork_exec(so, ex_ptr->ex_exec, do_pty));

#if 0
	case CTL_CMD:
	   for (tmpso = tcb.so_next; tmpso != &tcb; tmpso = tmpso->so_next) {
	     if (tmpso->so_emu == EMU_CTL &&
		 !(tmpso->so_tcpcb?
		   (tmpso->so_tcpcb->t_state & (TCPS_TIME_WAIT|TCPS_LAST_ACK))
		   :0)) {
	       /* Ooops, control connection already active */
	       sb->sb_cc = sprintf(sb->sb_wptr,"Sorry, already connected.\r\n");
	       sb->sb_wptr += sb->sb_cc;
	       return 0;
	     }
	   }
	   so->so_emu = EMU_CTL;
	   ctl_password_ok = 0;
	   sb->sb_cc = sprintf(sb->sb_wptr, "Slirp command-line ready (type \"help\" for help).\r\nSlirp> ");
	   sb->sb_wptr += sb->sb_cc;
	   do_echo=-1;
	   return(2);
#endif
	}
}
