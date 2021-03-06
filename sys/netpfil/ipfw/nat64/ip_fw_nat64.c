/*-
 * Copyright (c) 2015-2016 Yandex LLC
 * Copyright (c) 2015-2016 Andrey V. Elsukov <ae@FreeBSD.org>
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/ip_var.h>
#include <netinet/ip_fw.h>

#include <netpfil/ipfw/ip_fw_private.h>
#include <netpfil/ipfw/nat64/ip_fw_nat64.h>
#include <netpfil/ipfw/nat64/nat64_translate.h>


int nat64_debug = 0;
SYSCTL_DECL(_net_inet_ip_fw);
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, nat64_debug, CTLFLAG_RW,
    &nat64_debug, 0, "Debug level for NAT64 module");

int nat64_allow_private = 0;
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, nat64_allow_private, CTLFLAG_RW,
    &nat64_allow_private, 0,
    "Allow use of non-global IPv4 addresses with NAT64");

static int
vnet_ipfw_nat64_init(const void *arg __unused)
{
	struct ip_fw_chain *ch;
	int first, error;

	ch = &V_layer3_chain;
	first = IS_DEFAULT_VNET(curvnet) ? 1: 0;
	error = nat64stl_init(ch, first);
	if (error != 0)
		return (error);
	error = nat64lsn_init(ch, first);
	if (error != 0) {
		nat64stl_uninit(ch, first);
		return (error);
	}
	return (0);
}

static int
vnet_ipfw_nat64_uninit(const void *arg __unused)
{
	struct ip_fw_chain *ch;
	int last;

	ch = &V_layer3_chain;
	last = IS_DEFAULT_VNET(curvnet) ? 1: 0;
	nat64stl_uninit(ch, last);
	nat64lsn_uninit(ch, last);
	return (0);
}

static int
ipfw_nat64_modevent(module_t mod, int type, void *unused)
{

	switch (type) {
	case MOD_LOAD:
	case MOD_UNLOAD:
		break;
	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static moduledata_t ipfw_nat64_mod = {
	"ipfw_nat64",
	ipfw_nat64_modevent,
	0
};

/* Define startup order. */
#define	IPFW_NAT64_SI_SUB_FIREWALL	SI_SUB_PROTO_IFATTACHDOMAIN
#define	IPFW_NAT64_MODEVENT_ORDER	(SI_ORDER_ANY - 128) /* after ipfw */
#define	IPFW_NAT64_MODULE_ORDER		(IPFW_NAT64_MODEVENT_ORDER + 1)
#define	IPFW_NAT64_VNET_ORDER		(IPFW_NAT64_MODEVENT_ORDER + 2)

DECLARE_MODULE(ipfw_nat64, ipfw_nat64_mod, IPFW_NAT64_SI_SUB_FIREWALL,
    SI_ORDER_ANY);
MODULE_DEPEND(ipfw_nat64, ipfw, 3, 3, 3);
MODULE_VERSION(ipfw_nat64, 1);

VNET_SYSINIT(vnet_ipfw_nat64_init, IPFW_NAT64_SI_SUB_FIREWALL,
    IPFW_NAT64_VNET_ORDER, vnet_ipfw_nat64_init, NULL);
VNET_SYSUNINIT(vnet_ipfw_nat64_uninit, IPFW_NAT64_SI_SUB_FIREWALL,
    IPFW_NAT64_VNET_ORDER, vnet_ipfw_nat64_uninit, NULL);
