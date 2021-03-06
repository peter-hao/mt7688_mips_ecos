/*
 * Copyright (C) 1993-2002 by Darren Reed.
 *
 * See the IPFILTER.LICENCE file for details on licencing.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>

#include <sys/time.h>
#include <sys/param.h>
//#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <net/if.h>
#if __FreeBSD_version >= 300000
# include <net/if_var.h>
#endif
#include <net/netdb.h>
#include <arpa/nameser.h>
#include <arpa/inet.h>
//#include <resolv.h>
#include <ctype.h>
#include <ip_compat.h>
#include <ip_fil.h>
#include <ip_nat.h>
#include <ip_state.h>
#include <ip_proxy.h>
#include <ipf.h>

#define	STRERROR(x)	strerror(x)


extern	void	printnat __P((ipnat_t *, int));
extern	int	countbits __P((u_32_t));
extern	char	*proto;

ipnat_t	*natparse __P((char *, int));
void	natparsefile __P((int, char *, int));
void	nat_setgroupmap __P((struct ipnat *));
int	icmpidnum __P((char *, u_short *, int));	

//#define NATRULE_DEBUG			1
#ifdef NATRULE_DEBUG
#define DBGPRINTF(fmt, ...)		diag_printf(fmt, ## __VA_ARGS__)
#else
#define DBGPRINTF(fmt, ...)
#endif



void nat_setgroupmap(n)
ipnat_t *n;
{
	if (n->in_outmsk == n->in_inmsk)
		n->in_ippip = 1;
	else if (n->in_flags & IPN_AUTOPORTMAP) {
		n->in_ippip = ~ntohl(n->in_inmsk);
		if (n->in_outmsk != 0xffffffff)
			n->in_ippip /= (~ntohl(n->in_outmsk) + 1);
		n->in_ippip++;
		if (n->in_ippip == 0)
			n->in_ippip = 1;
		/* 12/07/05 Roger ++,  force to divide 256 parts */	
		if(n->in_ppip == 1)
			n->in_ppip = 256;
		/* 12/07/05 Roger --,  */	
		n->in_ppip = USABLE_PORTS / n->in_ippip;
	} else {
		n->in_space = USABLE_PORTS * ~ntohl(n->in_outmsk);
		n->in_nip = 0;
		if (!(n->in_ppip = n->in_pmin))
			n->in_ppip = 1;
		n->in_ippip = USABLE_PORTS / n->in_ppip;
	}
}


/*
 * Parse a line of input from the ipnat configuration file
 */
ipnat_t *natparse(line, linenum)
char *line;
int linenum;
{
	static ipnat_t ipn;
	struct protoent *pr;
	char *dnetm = NULL, *dport = NULL;
	char *s, *t, *cps[31], **cpp;
	int i, cnt;
	char *port1a = NULL, *port1b = NULL, *port2a = NULL;
	struct multip * mtport;
	char *mpstr1, *mpstr2;
	unsigned short tempp;
	

	proto = NULL;
	DBGPRINTF("[NAT] rule: %s", line);

	/*
	 * Search for end of line and comment marker, advance of leading spaces
	 */
	if ((s = strchr(line, '\n')))
		*s = '\0';
	if ((s = strchr(line, '#')))
		*s = '\0';
	while (*line && isspace(*line))
		line++;
	if (!*line)
		return NULL;

	bzero((char *)&ipn, sizeof(ipn));
	cnt = 0;
	
	/*
	 * split line upto into segments.
	 */
	for (i = 0, *cps = strtok(line, " \b\t\r\n"); cps[i] && i < 30; cnt++)
		cps[++i] = strtok(NULL, " \b\t\r\n");

	cps[i] = NULL;

	if (cnt < 3) {
		fprintf(stderr, "%d: not enough segments in line\n", linenum);
		return NULL;
	}

	cpp = cps;

	/*
	 * Check first word is a recognised keyword and then is the interface
	 */
	if (!strcasecmp(*cpp, "map"))
		ipn.in_redir = NAT_MAP;
	else if (!strcasecmp(*cpp, "map-block"))
		ipn.in_redir = NAT_MAPBLK;
	else if (!strcasecmp(*cpp, "rdr"))
		ipn.in_redir = NAT_REDIRECT;
	else if (!strcasecmp(*cpp, "bimap"))
		ipn.in_redir = NAT_BIMAP;
	else {
		fprintf(stderr, "%d: unknown mapping: \"%s\"\n",
			linenum, *cpp);
		return NULL;
	}

	cpp++;

	strncpy(ipn.in_ifname, *cpp, sizeof(ipn.in_ifname) - 1);
	ipn.in_ifname[sizeof(ipn.in_ifname) - 1] = '\0';
	cpp++;
	if (!strcmp(*cpp, "fromif")) {
		cpp++;
		ipn.from_ifp = (void *)GETUNIT(*cpp, 4);
		cpp++;
	}
	/*
	 * If the first word after the interface is "from" or is a ! then
	 * the expanded syntax is being used so parse it differently.
	 */
	if (!strcasecmp(*cpp, "from") || (**cpp == '!')) {
		if (!strcmp(*cpp, "!")) {
			cpp++;
			if (strcasecmp(*cpp, "from")) {
				fprintf(stderr, "Missing from after !\n");
				return NULL;
			}
			ipn.in_flags |= IPN_NOTSRC;
		} else if (**cpp == '!') {
			if (strcasecmp(*cpp + 1, "from")) {
				fprintf(stderr, "Missing from after !\n");
				return NULL;
			}
			ipn.in_flags |= IPN_NOTSRC;
		}
		if ((ipn.in_flags & IPN_NOTSRC) &&
		    (ipn.in_redir & (NAT_MAP|NAT_MAPBLK))) {
			fprintf(stderr, "Cannot use '! from' with map\n");
			return NULL;
		}

		ipn.in_flags |= IPN_FILTER;
		cpp++;
		if (ipn.in_redir == NAT_REDIRECT) {
			if (hostmask(&cpp, (u_32_t *)&ipn.in_srcip,
				     (u_32_t *)&ipn.in_srcmsk, NULL, &ipn.in_sport,
				     &ipn.in_scmp, &ipn.in_stop, linenum)) {
				return NULL;
			}
		} else {
			if (hostmask(&cpp, (u_32_t *)&ipn.in_inip,
				     (u_32_t *)&ipn.in_inmsk, NULL, &ipn.in_sport,
				     &ipn.in_scmp, &ipn.in_stop, linenum)) {
				return NULL;
			}
		}

		if (!strcmp(*cpp, "!")) {
			cpp++;
			ipn.in_flags |= IPN_NOTDST;
		} else if (**cpp == '!') {
			(*cpp)++;
			ipn.in_flags |= IPN_NOTDST;
		}

		if (strcasecmp(*cpp, "to")) {
			fprintf(stderr, "%d: unexpected keyword (%s) - to\n",
				linenum, *cpp);
			return NULL;
		}
		if ((ipn.in_flags & IPN_NOTDST) &&
		    (ipn.in_redir & (NAT_REDIRECT))) {
			fprintf(stderr, "Cannot use '! to' with rdr\n");
			return NULL;
		}

		if (!*++cpp) {
			fprintf(stderr, "%d: missing host after to\n", linenum);
			return NULL;
		}
		if (ipn.in_redir == NAT_REDIRECT) {
			if (hostmask(&cpp, (u_32_t *)&ipn.in_outip,
				     (u_32_t *)&ipn.in_outmsk, NULL, &ipn.in_dport,
				     &ipn.in_dcmp, &ipn.in_dtop, linenum)) {
				return NULL;
			}
			ipn.in_pmin = htons(ipn.in_dport);
		} else {
			if (hostmask(&cpp, (u_32_t *)&ipn.in_srcip,
				     (u_32_t *)&ipn.in_srcmsk, NULL, &ipn.in_dport,
				     &ipn.in_dcmp, &ipn.in_dtop, linenum)) {
				return NULL;
			}
		}
	} else {
		s = *cpp;
		if (!s) {
			fprintf(stderr, "%d: short line\n", linenum);
			return NULL;
		}
		t = strchr(s, '/');
		if (!t) {
			fprintf(stderr, "%d: no netmask on LHS\n", linenum);
			return NULL;
		}
		*t++ = '\0';
		if (ipn.in_redir == NAT_REDIRECT) {
			if (hostnum((u_32_t *)&ipn.in_outip, s, linenum) == -1)
				return NULL;
			if (genmask(t, (u_32_t *)&ipn.in_outmsk) == -1) {
				return NULL;
			}
		} else {
			if (hostnum((u_32_t *)&ipn.in_inip, s, linenum) == -1)
				return NULL;
			if (genmask(t, (u_32_t *)&ipn.in_inmsk) == -1) {
				return NULL;
			}
		}
		cpp++;
		if (!*cpp) {
			fprintf(stderr, "%d: short line\n", linenum);
			return NULL;
		}
	}

	/*
	 * If it is a standard redirect then we expect it to have a port
	 * match after the hostmask.
	 */
	if ((ipn.in_redir == NAT_REDIRECT) && !(ipn.in_flags & IPN_FILTER)) {
		if (strcasecmp(*cpp, "port")) {
			fprintf(stderr, "%d: missing fields - 1st port\n",
				linenum);
			return NULL;
		}

		cpp++;
		
		/* 26/01/04 Roger ++,  add support port "multi-range" */	
		if(!strcasecmp(*cpp, "multirange")) {
			cpp++;
			ipn.in_flags |= IPN_MULTIP;
			mtport = (struct multip *)malloc(sizeof(struct multip)*32);
			memset(mtport, 0, sizeof(struct multip)*32);
			
			mpstr1 = *cpp;
			diag_printf("mpstr1:%s\n", mpstr1);
			
			for( i = 0; i<32 ; i++) {
				mpstr2 = strchr(mpstr1, ',');
				if(mpstr2)
					*mpstr2++ = '\0';
				
				if (isdigit(*mpstr1) && (s = strchr(mpstr1, '-')))
					*s++ = '\0';
				else
					s = NULL;
					
				if (!portnum(mpstr1, &(mtport[i].in_port[0]), linenum))
					return NULL;
				mtport[i].in_port[0] = htons(mtport[i].in_port[0]);
				
				if (s != NULL) {
					if (!portnum(s, &(mtport[i].in_port[1]), linenum))
						return NULL;
					mtport[i].in_port[1] = htons(mtport[i].in_port[1]);
				} else
					mtport[i].in_port[1] = mtport[i].in_port[0];
				
				if(mtport[i].in_port[1] < mtport[i].in_port[0]){
					tempp = mtport[i].in_port[1];
					mtport[i].in_port[1] = mtport[i].in_port[0];
					mtport[i].in_port[0] = tempp;
				}
				if(!mpstr2)
					break;
					
				mpstr1 = mpstr2;
			}
			cpp++;
#if 0
			i=0;
			do{
				diag_printf("mtport[%d][0]=%x, mtport[%d][1]=%x\n", i, mtport[i].in_port[0], i, mtport[i].in_port[1]);
				i++;
			}while(mtport[i].in_port[0]);
#endif
			ipn.in_pmax = mtport[0].in_port[1];
			ipn.in_pmin = mtport[0].in_port[0];
			//return NULL; /* 26/01/04 Roger,  */	
		}else {
		/* 26/01/04 Roger --,  */	
		
		if (!*cpp) {
			fprintf(stderr,
				"%d: missing fields (destination port)\n",
				linenum);
			return NULL;
		}

		if (isdigit(**cpp) && (s = strchr(*cpp, '-')))
			*s++ = '\0';
		else
			s = NULL;

		port1a = *cpp++;

		if (!strcmp(*cpp, "-")) {
			cpp++;
			s = *cpp++;
		}

		if (s)
			port1b = s;
		else
			ipn.in_pmax = ipn.in_pmin;
		} /* 26/01/04 Roger,  */	
		
		/* 13/11/03 Roger ++,  add to support port trigger*/	
		if(!strcasecmp(*cpp, "trigger")) {
			//breakpoint();
			ipn.in_flags |= IPN_TRIG;
			
			cpp++;
			
			if (!strcasecmp(*cpp, "tcp"))
				ipn.in_flags |= IPN_TCP;
			else if (!strcasecmp(*cpp, "udp"))
				ipn.in_flags |= IPN_UDP;
			else if (!strcasecmp(*cpp, "tcpudp"))
				ipn.in_flags |= IPN_TCPUDP;
			else if (!strcasecmp(*cpp, "tcp/udp"))
				ipn.in_flags |= IPN_TCPUDP;
			else {
				fprintf(stderr,
					"%d: expected protocol name - got \"%s\"\n",
					linenum, *cpp);
				return NULL;
			}

			if((pr = getprotobyname(*cpp)))
				ipn.in_p = pr->p_proto;
			
			if(!(ipn.in_flags & IPN_MULTIP)) {
				if (!portnum(port1a, &ipn.in_pmin, linenum))
					return NULL;
				ipn.in_pmin = htons(ipn.in_pmin);
				if (port1b != NULL) {
					if (!portnum(port1b, &ipn.in_pmax, linenum))
						return NULL;
					ipn.in_pmax = htons(ipn.in_pmax);
				} else
					ipn.in_pmax = ipn.in_pmin;
				
				//ipn.in_pnext = ipn.in_pmin; /* 26/01/04 Roger,  */	
			}else
				ipn.in_mport = mtport;
			
			cpp++;
			
			if (strcasecmp(*cpp, "port")) {
				fprintf(stderr, "%d: expected \"port\"\n",
					linenum);
				return NULL;
			}
			cpp++;
			if (!*cpp) {
				fprintf(stderr,
					"%d: missing fields (triggering port)\n",
				linenum);
				return NULL;
			}

			if (isdigit(**cpp) && (s = strchr(*cpp, '-')))
				*s++ = '\0';
			else
				s = NULL;
			
			if (!portnum(*cpp, &ipn.in_trigpmin, linenum))
				return NULL;	
			//ipn.in_trigpmin = htons(ipn.in_trigpmin);
			
			cpp++;
			if (!strcmp(*cpp, "-")) {
				cpp++;
				s = *cpp++;
			}
			if (s){
				if (!portnum(s, &ipn.in_trigpmax, linenum))
					return NULL;
				//ipn.in_trigpmax = htons(ipn.in_trigpmax);
			}
			else
				ipn.in_trigpmax = ipn.in_trigpmin;
				
			if (!strcasecmp(*cpp, "tcp"))
				ipn.in_trigp |= IPN_TCP;
			else if (!strcasecmp(*cpp, "udp"))
				ipn.in_trigp |= IPN_UDP;
			else if (!strcasecmp(*cpp, "tcpudp"))
				ipn.in_trigp |= IPN_TCPUDP;
			else if (!strcasecmp(*cpp, "tcp/udp"))
				ipn.in_trigp |= IPN_TCPUDP;
			else {
				fprintf(stderr,
					"%d: expected protocol name - got \"%s\"\n",
					linenum, *cpp);
				return NULL;
			}
			
			cpp++;
				
			if (*cpp && !strcasecmp(*cpp, "age")){
				cpp++;
				if (*cpp) {
					ipn.in_trigage = atoi(*cpp);
					cpp++;
				} else {
					fprintf(stderr,
					   "%d: trigger age with no parameters\n",
						linenum);
					return NULL;
				}
			}
			
			return &ipn;
		}
		/* 13/11/03 Roger --,  */	
	}

	/*
	 * In the middle of the NAT rule syntax is -> to indicate the
	 * direction of translation.
	 */
	if (!*cpp) {
		fprintf(stderr, "%d: missing fields (->)\n", linenum);
		return NULL;
	}
	if (strcmp(*cpp, "->")) {
		fprintf(stderr, "%d: missing ->\n", linenum);
		return NULL;
	}
	cpp++;

	if (!*cpp) {
		fprintf(stderr, "%d: missing fields (%s)\n",
			linenum, ipn.in_redir ? "destination" : "target");
		return NULL;
	}

	if (ipn.in_redir == NAT_MAP) {
		if (!strcasecmp(*cpp, "range")) {
			cpp++;
			ipn.in_flags |= IPN_IPRANGE;
			if (!*cpp) {
				fprintf(stderr, "%d: missing fields (%s)\n",
					linenum,
					ipn.in_redir ? "destination":"target");
				return NULL;
			}
		}
	}

	if (ipn.in_flags & IPN_IPRANGE) {
		dnetm = strrchr(*cpp, '-');
		if (dnetm == NULL) {
			cpp++;
			if (*cpp && !strcmp(*cpp, "-") && *(cpp + 1))
					dnetm = *(cpp + 1);
		} else
			*dnetm++ = '\0';
		if (dnetm == NULL || *dnetm == '\0') {
			fprintf(stderr,
				"%d: desination range not specified\n",
				linenum);
			return NULL;
		}
	} else if (ipn.in_redir != NAT_REDIRECT) {
		dnetm = strrchr(*cpp, '/');
		if (dnetm == NULL) {
			cpp++;
			if (*cpp && !strcasecmp(*cpp, "netmask"))
				dnetm = *++cpp;
		}
		if (dnetm == NULL) {
			fprintf(stderr,
				"%d: missing fields (dest netmask)\n",
				linenum);
			return NULL;
		}
		if (*dnetm == '/')
			*dnetm++ = '\0';
	}

	if (ipn.in_redir == NAT_REDIRECT) {
		dnetm = strchr(*cpp, ',');
		if (dnetm != NULL) {
			ipn.in_flags |= IPN_SPLIT;
			*dnetm++ = '\0';
		}
		if (hostnum((u_32_t *)&ipn.in_inip, *cpp, linenum) == -1)
			return NULL;
	} else {
		if (!strcmp(*cpp, ipn.in_ifname))
			*cpp = "0";
		if (hostnum((u_32_t *)&ipn.in_outip, *cpp, linenum) == -1)
			return NULL;
	}
	cpp++;

	if (ipn.in_redir & NAT_MAPBLK) {
		if (*cpp) {
			if (strcasecmp(*cpp, "ports")) {
				fprintf(stderr,
					"%d: expected \"ports\" - got \"%s\"\n",
					linenum, *cpp);
				return NULL;
			}
			cpp++;
			if (*cpp == NULL) {
				fprintf(stderr,
					"%d: missing argument to \"ports\"\n",
					linenum);
				return NULL;
			}
			if (!strcasecmp(*cpp, "auto"))
				ipn.in_flags |= IPN_AUTOPORTMAP;
			else
				ipn.in_pmin = atoi(*cpp);
			cpp++;
		} else
			ipn.in_pmin = 0;
	} else if ((ipn.in_redir & NAT_BIMAP) == NAT_REDIRECT) {
		if (*cpp && (strrchr(*cpp, '/') != NULL)) {
			fprintf(stderr, "%d: No netmask supported in %s\n",
				linenum, "destination host for redirect");
			return NULL;
		}

		if (!*cpp) {
			fprintf(stderr, "%d: Missing destination port %s\n",
				linenum, "in redirect");
			return NULL;
		}

		/* If it's a in_redir, expect target port */

		if (strcasecmp(*cpp, "port")) {
			fprintf(stderr, "%d: missing fields - 2nd port (%s)\n",
				linenum, *cpp);
			return NULL;
		}
		cpp++;
		if (!*cpp) {
			fprintf(stderr,
				"%d: missing fields (destination port)\n",
				linenum);
			return NULL;
		}

		port2a = *cpp++;
	} 
	if (dnetm && *dnetm == '/')
		*dnetm++ = '\0';

	if (ipn.in_redir & (NAT_MAP|NAT_MAPBLK)) {
		if (ipn.in_flags & IPN_IPRANGE) {
			if (hostnum((u_32_t *)&ipn.in_outmsk, dnetm,
				    linenum) == -1)
				return NULL;
		} else if (genmask(dnetm, (u_32_t *)&ipn.in_outmsk))
			return NULL;
	} else {
		if (ipn.in_flags & IPN_SPLIT) {
			if (hostnum((u_32_t *)&ipn.in_inmsk, dnetm,
				    linenum) == -1)
				return NULL;
		} else if (genmask("255.255.255.255", (u_32_t *)&ipn.in_inmsk))
			return NULL;
		if (!*cpp) {
			ipn.in_flags |= IPN_TCP; /* XXX- TCP only by default */
			proto = "tcp";
		} else {
			proto = *cpp++;
			if (!strcasecmp(proto, "tcp"))
				ipn.in_flags |= IPN_TCP;
			else if (!strcasecmp(proto, "udp"))
				ipn.in_flags |= IPN_UDP;
			else if (!strcasecmp(proto, "tcp/udp"))
				ipn.in_flags |= IPN_TCPUDP;
			else if (!strcasecmp(proto, "tcpudp")) {
				ipn.in_flags |= IPN_TCPUDP;
				proto = "tcp/udp";
			} else if (!strcasecmp(proto, "ip"))
				ipn.in_flags |= IPN_ANY;
			else {
				ipn.in_flags |= IPN_ANY;
				if ((pr = getprotobyname(proto)))
					ipn.in_p = pr->p_proto;
				else {
					if (!isdigit(*proto)) {
						fprintf(stderr,
						"%d: Unknown protocol %s\n",
							linenum, proto);
						return NULL;
					} else
						ipn.in_p = atoi(proto);
				}
			}
			if ((ipn.in_flags & IPN_TCPUDP) == 0) {
				port1a = "0";
				port2a = "0";
			}
			
			if (ipn.in_redir != NAT_BIMAP && *cpp && !strcasecmp(*cpp, "proxy")) {
				if (ipn.in_redir == NAT_BIMAP) {
					fprintf(stderr, "%d: cannot use proxy with bimap\n",
						linenum);
					return NULL;
				}
				cpp++;
				if (!*cpp) {
					fprintf(stderr,
						"%d: missing parameter for \"proxy\"\n",
						linenum);
					return NULL;
				}
				dport = NULL;

				if (!strcasecmp(*cpp, "port")) {
					cpp++;
					if (!*cpp) {
						fprintf(stderr,
							"%d: missing parameter for \"port\"\n",
							linenum);
						return NULL;
					}

					dport = *cpp;
					cpp++;

					if (!*cpp) {
						fprintf(stderr,
							"%d: missing parameter for \"proxy\"\n",
							linenum);
						return NULL;
					}
				} else {
					fprintf(stderr,
						"%d: missing keyword \"port\"\n", linenum);
					return NULL;
				}

				if ((proto = index(*cpp, '/'))) {
					*proto++ = '\0';
					if ((pr = getprotobyname(proto)))
						ipn.in_p = pr->p_proto;
					else
						ipn.in_p = atoi(proto);
				} else
					ipn.in_p = 0;

				if (dport && !portnum(dport, &ipn.in_dport, linenum))
					return NULL;
				ipn.in_dport = htons(ipn.in_dport);
		
				(void) strncpy(ipn.in_plabel, *cpp, sizeof(ipn.in_plabel));
				cpp++;
			}
			
			if (*cpp && !strcasecmp(*cpp, "dmz")) {
				cpp++;
				ipn.in_flags |= IPN_DMZ;
			}
			
			/* 05/11/04 Roger ++,  */			
			if (*cpp && !strcasecmp(*cpp, "act-time")) {
				int curtime = GMTtime(0);;

				cpp++;
				if (*cpp && !strcasecmp(*cpp, "period")) {
					ipn.fr_actm = 1;
					cpp++;
					ipn.fr_actd = atoi(*cpp);
				}else if(!strcasecmp(*cpp, "one")){
					ipn.fr_actm = 2;
				}else {
					fprintf(stderr, "%d: unexpected keyword (%s) - act-time\n",
						linenum, *cpp);
					return NULL;
				}
				cpp++;
				add_acttime(&cpp, &(ipn.fr_actst), &(ipn.fr_actet));
				cpp++;
		
				if((curtime >= ipn.fr_actst) && (curtime <= (ipn.fr_actet?:86400)))
					ipn.in_flags &= ~FR_INACTIVE_SCHED;
				else
					ipn.in_flags |= FR_INACTIVE_SCHED;
			}
			/* 05/11/04 Roger --,  */		
			if (*cpp && !strcasecmp(*cpp, "round-robin")) {
				cpp++;
				ipn.in_flags |= IPN_ROUNDR;
			}

			if (*cpp && !strcasecmp(*cpp, "frag")) {
				cpp++;
				ipn.in_flags |= IPN_FRAG;
			}

			if (*cpp && !strcasecmp(*cpp, "age")) {
				cpp++;
				if (!*cpp) {
					fprintf(stderr,
						"%d: age with no parameters\n",
						linenum);
					return NULL;
				}

				ipn.in_age[0] = atoi(*cpp);
				s = index(*cpp, '/');
				if (s != NULL)
					ipn.in_age[1] = atoi(s + 1);
				else
					ipn.in_age[1] = ipn.in_age[0];
				cpp++;
			}

			if (*cpp && !strcasecmp(*cpp, "mssclamp")) {
				cpp++;
				if (*cpp) {
					ipn.in_mssclamp = atoi(*cpp);
					cpp++;
				} else {
					fprintf(stderr,
					   "%d: mssclamp with no parameters\n",
						linenum);
					return NULL;
				}
			}

			if (*cpp) {
				fprintf(stderr,
				"%d: extra junk at the end of the line: %s\n",
					linenum, *cpp);
				return NULL;
			}
		}
	}

	if ((ipn.in_redir == NAT_REDIRECT) && !(ipn.in_flags & IPN_FILTER)) {
		if (!portnum(port1a, &ipn.in_pmin, linenum))
			return NULL;
		ipn.in_pmin = htons(ipn.in_pmin);
		if (port1b != NULL) {
			if (!portnum(port1b, &ipn.in_pmax, linenum))
				return NULL;
			ipn.in_pmax = htons(ipn.in_pmax);
		} else
			ipn.in_pmax = ipn.in_pmin;
	}

	if ((ipn.in_redir & NAT_BIMAP) == NAT_REDIRECT) {
		if (!portnum(port2a, &ipn.in_pnext, linenum))
			return NULL;
		ipn.in_pnext = htons(ipn.in_pnext);
	}

	if (!(ipn.in_flags & IPN_SPLIT))
		ipn.in_inip &= ipn.in_inmsk;
	if ((ipn.in_flags & IPN_IPRANGE) == 0)
		ipn.in_outip &= ipn.in_outmsk;
	ipn.in_srcip &= ipn.in_srcmsk;

	if ((ipn.in_redir & NAT_MAPBLK) != 0)
		nat_setgroupmap(&ipn);

	if (*cpp && !*(cpp+1) && !strcasecmp(*cpp, "frag")) {
		cpp++;
		ipn.in_flags |= IPN_FRAG;
	}

	if (!*cpp)
		return &ipn;

	if (ipn.in_redir != NAT_BIMAP && !strcasecmp(*cpp, "proxy")) {
		if (ipn.in_redir == NAT_BIMAP) {
			fprintf(stderr, "%d: cannot use proxy with bimap\n",
				linenum);
			return NULL;
		}
		cpp++;
		if (!*cpp) {
			fprintf(stderr,
				"%d: missing parameter for \"proxy\"\n",
				linenum);
			return NULL;
		}
		dport = NULL;

		if (!strcasecmp(*cpp, "port")) {
			cpp++;
			if (!*cpp) {
				fprintf(stderr,
					"%d: missing parameter for \"port\"\n",
					linenum);
				return NULL;
			}

			dport = *cpp;
			cpp++;

			if (!*cpp) {
				fprintf(stderr,
					"%d: missing parameter for \"proxy\"\n",
					linenum);
				return NULL;
			}
		} else {
			fprintf(stderr,
				"%d: missing keyword \"port\"\n", linenum);
			return NULL;
		}

		if ((proto = index(*cpp, '/'))) {
			*proto++ = '\0';
			if ((pr = getprotobyname(proto)))
				ipn.in_p = pr->p_proto;
			else
				ipn.in_p = atoi(proto);
		} else
			ipn.in_p = 0;

		if (dport && !portnum(dport, &ipn.in_dport, linenum))
			return NULL;
		ipn.in_dport = htons(ipn.in_dport);

		(void) strncpy(ipn.in_plabel, *cpp, sizeof(ipn.in_plabel));
		cpp++;
	/* Added by Eddy */	
		} else if (!strcasecmp(*cpp, "icmpidmap")) {
		cpp++;
		if (!*cpp) {
			fprintf(stderr,
				"%d: icmpidmap misses protocol and range\n",
				linenum);
			return NULL;
		};

		if (!strcasecmp(*cpp, "icmp"))
			ipn.in_flags = IPN_ICMPQUERY;
		else {
			fprintf(stderr,
				"%d: icmpidmap only valid for the icmp protocol\n",
				linenum);
			return NULL;
		}
		cpp++;

		if (!*cpp) {
			fprintf(stderr, "%d: no icmp id argument found\n", linenum);
			return NULL;
		}

		if (!(t = strchr(*cpp, ':'))) {
			fprintf(stderr, "%d: no icmp id range detected in \"%s\"\n",
				linenum, *cpp);
			return NULL;
		}
		*t++ = '\0';
		
		if (!icmpidnum(*cpp, &ipn.in_pmin, linenum) || 
		    !icmpidnum(t, &ipn.in_pmax, linenum))
			return NULL;
		
		ipn.in_pmin = htons(ipn.in_pmin);
		ipn.in_pmax = htons(ipn.in_pmax);
		return &ipn;
	} else if (ipn.in_redir != NAT_BIMAP && !strcasecmp(*cpp, "portmap")) {
		if (ipn.in_redir == NAT_BIMAP) {
			fprintf(stderr, "%d: cannot use portmap with bimap\n",
				linenum);
			return NULL;
		}
		cpp++;
		if (!*cpp) {
			fprintf(stderr,
				"%d: missing expression following portmap\n",
				linenum);
			return NULL;
		}

		if (!strcasecmp(*cpp, "tcp"))
			ipn.in_flags |= IPN_TCP;
		else if (!strcasecmp(*cpp, "udp"))
			ipn.in_flags |= IPN_UDP;
		else if (!strcasecmp(*cpp, "tcpudp"))
			ipn.in_flags |= IPN_TCPUDP;
		else if (!strcasecmp(*cpp, "tcp/udp"))
			ipn.in_flags |= IPN_TCPUDP;
		else {
			fprintf(stderr,
				"%d: expected protocol name - got \"%s\"\n",
				linenum, *cpp);
			return NULL;
		}
		proto = *cpp;
		cpp++;

		if (!*cpp) {
			fprintf(stderr, "%d: no port range found\n", linenum);
			return NULL;
		}

		if (!strcasecmp(*cpp, "auto")) {
			ipn.in_flags |= IPN_AUTOPORTMAP;
			ipn.in_pmin = htons(1024);
			ipn.in_pmax = htons(65535);
			nat_setgroupmap(&ipn);
			cpp++;
		} else {
			if (!(t = strchr(*cpp, ':'))) {
				fprintf(stderr,
					"%d: no port range in \"%s\"\n",
					linenum, *cpp);
				return NULL;
			}
			*t++ = '\0';
			if (!portnum(*cpp, &ipn.in_pmin, linenum) ||
			    !portnum(t, &ipn.in_pmax, linenum))
				return NULL;
			ipn.in_pmin = htons(ipn.in_pmin);
			ipn.in_pmax = htons(ipn.in_pmax);
			cpp++;
		}
	}
	
	if (*cpp && !strcasecmp(*cpp, "dmz")) {
		cpp++;
		ipn.in_flags |= IPN_DMZ;
	}
	
	if (*cpp && !strcasecmp(*cpp, "frag")) {
		cpp++;
		ipn.in_flags |= IPN_FRAG;
	}

	if (*cpp && !strcasecmp(*cpp, "age")) {
		cpp++;
		if (!*cpp) {
			fprintf(stderr, "%d: age with no parameters\n",
				linenum);
			return NULL;
		}
		ipn.in_age[0] = atoi(*cpp);
		s = index(*cpp, '/');
		if (s != NULL)
			ipn.in_age[1] = atoi(s + 1);
		else
			ipn.in_age[1] = ipn.in_age[0];
		cpp++;
	}

	if (*cpp && !strcasecmp(*cpp, "mssclamp")) {
		cpp++;
		if (*cpp) {
			ipn.in_mssclamp = atoi(*cpp);
			cpp++;
		} else {
			fprintf(stderr, "%d: mssclamp with no parameters\n",
				linenum);
			return NULL;
		}
	}
	
	if (*cpp && !strcasecmp(*cpp, "default")) {
		cpp++;
		ipn.in_flags |= IPN_DEFAULT_RULE;
	}
	
	if (*cpp) {
		fprintf(stderr, "%d: extra junk at the end of the line: %s\n",
			linenum, *cpp);
		return NULL;
	}
	return &ipn;
}

/* Added by Eddy */	
int	icmpidnum(str, id, linenum)
char	*str;
u_short *id;
int     linenum;
{
	int i;

	
	i = atoi(str);

	if ((i<0) || (i>65535)) {
		fprintf(stderr, "%d: invalid icmp id\"%s\".\n", linenum, str);
		return 0;
	}
	
	*id = (u_short)i;

	return 1;
}


