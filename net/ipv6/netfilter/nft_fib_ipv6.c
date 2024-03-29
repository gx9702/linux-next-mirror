/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter_ipv6.h>
#include <net/netfilter/nf_tables_core.h>
#include <net/netfilter/nf_tables.h>
#include <net/netfilter/nft_fib.h>

#include <net/ip6_fib.h>
#include <net/ip6_route.h>

static bool fib6_is_local(const struct sk_buff *skb)
{
	const struct rt6_info *rt = (const void *)skb_dst(skb);

	return rt && (rt->rt6i_flags & RTF_LOCAL);
}

static int get_ifindex(const struct net_device *dev)
{
	return dev ? dev->ifindex : 0;
}

static int nft_fib6_flowi_init(struct flowi6 *fl6, const struct nft_fib *priv,
			       const struct nft_pktinfo *pkt,
			       const struct net_device *dev)
{
	const struct ipv6hdr *iph = ipv6_hdr(pkt->skb);
	int lookup_flags = 0;

	if (priv->flags & NFTA_FIB_F_DADDR) {
		fl6->daddr = iph->daddr;
		fl6->saddr = iph->saddr;
	} else {
		fl6->daddr = iph->saddr;
		fl6->saddr = iph->daddr;
	}

	if (ipv6_addr_type(&fl6->daddr) & IPV6_ADDR_LINKLOCAL) {
		lookup_flags |= RT6_LOOKUP_F_IFACE;
		fl6->flowi6_oif = get_ifindex(dev ? dev : pkt->skb->dev);
	}

	if (ipv6_addr_type(&fl6->saddr) & IPV6_ADDR_UNICAST)
		lookup_flags |= RT6_LOOKUP_F_HAS_SADDR;

	if (priv->flags & NFTA_FIB_F_MARK)
		fl6->flowi6_mark = pkt->skb->mark;

	fl6->flowlabel = (*(__be32 *)iph) & IPV6_FLOWINFO_MASK;

	return lookup_flags;
}

static u32 __nft_fib6_eval_type(const struct nft_fib *priv,
				const struct nft_pktinfo *pkt)
{
	const struct net_device *dev = NULL;
	const struct nf_ipv6_ops *v6ops;
	const struct nf_afinfo *afinfo;
	int route_err, addrtype;
	struct rt6_info *rt;
	struct flowi6 fl6 = {
		.flowi6_iif = LOOPBACK_IFINDEX,
		.flowi6_proto = pkt->tprot,
	};
	u32 ret = 0;

	afinfo = nf_get_afinfo(NFPROTO_IPV6);
	if (!afinfo)
		return RTN_UNREACHABLE;

	if (priv->flags & NFTA_FIB_F_IIF)
		dev = pkt->in;
	else if (priv->flags & NFTA_FIB_F_OIF)
		dev = pkt->out;

	nft_fib6_flowi_init(&fl6, priv, pkt, dev);

	v6ops = nf_get_ipv6_ops();
	if (dev && v6ops && v6ops->chk_addr(pkt->net, &fl6.daddr, dev, true))
		ret = RTN_LOCAL;

	route_err = afinfo->route(pkt->net, (struct dst_entry **)&rt,
				  flowi6_to_flowi(&fl6), false);
	if (route_err)
		goto err;

	if (rt->rt6i_flags & RTF_REJECT) {
		route_err = rt->dst.error;
		dst_release(&rt->dst);
		goto err;
	}

	if (ipv6_anycast_destination((struct dst_entry *)rt, &fl6.daddr))
		ret = RTN_ANYCAST;
	else if (!dev && rt->rt6i_flags & RTF_LOCAL)
		ret = RTN_LOCAL;

	dst_release(&rt->dst);

	if (ret)
		return ret;

	addrtype = ipv6_addr_type(&fl6.daddr);

	if (addrtype & IPV6_ADDR_MULTICAST)
		return RTN_MULTICAST;
	if (addrtype & IPV6_ADDR_UNICAST)
		return RTN_UNICAST;

	return RTN_UNSPEC;
 err:
	switch (route_err) {
	case -EINVAL:
		return RTN_BLACKHOLE;
	case -EACCES:
		return RTN_PROHIBIT;
	case -EAGAIN:
		return RTN_THROW;
	default:
		break;
	}

	return RTN_UNREACHABLE;
}

void nft_fib6_eval_type(const struct nft_expr *expr, struct nft_regs *regs,
			const struct nft_pktinfo *pkt)
{
	const struct nft_fib *priv = nft_expr_priv(expr);
	u32 *dest = &regs->data[priv->dreg];

	*dest = __nft_fib6_eval_type(priv, pkt);
}
EXPORT_SYMBOL_GPL(nft_fib6_eval_type);

void nft_fib6_eval(const struct nft_expr *expr, struct nft_regs *regs,
		   const struct nft_pktinfo *pkt)
{
	const struct nft_fib *priv = nft_expr_priv(expr);
	const struct net_device *oif = NULL;
	u32 *dest = &regs->data[priv->dreg];
	struct flowi6 fl6 = {
		.flowi6_iif = LOOPBACK_IFINDEX,
		.flowi6_proto = pkt->tprot,
	};
	struct rt6_info *rt;
	int lookup_flags;

	if (priv->flags & NFTA_FIB_F_IIF)
		oif = pkt->in;
	else if (priv->flags & NFTA_FIB_F_OIF)
		oif = pkt->out;

	lookup_flags = nft_fib6_flowi_init(&fl6, priv, pkt, oif);

	if (pkt->hook == NF_INET_PRE_ROUTING && fib6_is_local(pkt->skb)) {
		nft_fib_store_result(dest, priv->result, pkt, LOOPBACK_IFINDEX);
		return;
	}

	*dest = 0;
 again:
	rt = (void *)ip6_route_lookup(pkt->net, &fl6, lookup_flags);
	if (rt->dst.error)
		goto put_rt_err;

	/* Should not see RTF_LOCAL here */
	if (rt->rt6i_flags & (RTF_REJECT | RTF_ANYCAST | RTF_LOCAL))
		goto put_rt_err;

	if (oif && oif != rt->rt6i_idev->dev) {
		/* multipath route? Try again with F_IFACE */
		if ((lookup_flags & RT6_LOOKUP_F_IFACE) == 0) {
			lookup_flags |= RT6_LOOKUP_F_IFACE;
			fl6.flowi6_oif = oif->ifindex;
			ip6_rt_put(rt);
			goto again;
		}
	}

	switch (priv->result) {
	case NFT_FIB_RESULT_OIF:
		*dest = rt->rt6i_idev->dev->ifindex;
		break;
	case NFT_FIB_RESULT_OIFNAME:
		strncpy((char *)dest, rt->rt6i_idev->dev->name, IFNAMSIZ);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}

 put_rt_err:
	ip6_rt_put(rt);
}
EXPORT_SYMBOL_GPL(nft_fib6_eval);

static struct nft_expr_type nft_fib6_type;

static const struct nft_expr_ops nft_fib6_type_ops = {
	.type		= &nft_fib6_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_fib)),
	.eval		= nft_fib6_eval_type,
	.init		= nft_fib_init,
	.dump		= nft_fib_dump,
	.validate	= nft_fib_validate,
};

static const struct nft_expr_ops nft_fib6_ops = {
	.type		= &nft_fib6_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_fib)),
	.eval		= nft_fib6_eval,
	.init		= nft_fib_init,
	.dump		= nft_fib_dump,
	.validate	= nft_fib_validate,
};

static const struct nft_expr_ops *
nft_fib6_select_ops(const struct nft_ctx *ctx,
		    const struct nlattr * const tb[])
{
	enum nft_fib_result result;

	if (!tb[NFTA_FIB_RESULT])
		return ERR_PTR(-EINVAL);

	result = htonl(nla_get_be32(tb[NFTA_FIB_RESULT]));

	switch (result) {
	case NFT_FIB_RESULT_OIF:
		return &nft_fib6_ops;
	case NFT_FIB_RESULT_OIFNAME:
		return &nft_fib6_ops;
	case NFT_FIB_RESULT_ADDRTYPE:
		return &nft_fib6_type_ops;
	default:
		return ERR_PTR(-EOPNOTSUPP);
	}
}

static struct nft_expr_type nft_fib6_type __read_mostly = {
	.name		= "fib",
	.select_ops	= &nft_fib6_select_ops,
	.policy		= nft_fib_policy,
	.maxattr	= NFTA_FIB_MAX,
	.family		= NFPROTO_IPV6,
	.owner		= THIS_MODULE,
};

static int __init nft_fib6_module_init(void)
{
	return nft_register_expr(&nft_fib6_type);
}

static void __exit nft_fib6_module_exit(void)
{
	nft_unregister_expr(&nft_fib6_type);
}
module_init(nft_fib6_module_init);
module_exit(nft_fib6_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Florian Westphal <fw@strlen.de>");
MODULE_ALIAS_NFT_AF_EXPR(10, "fib");
