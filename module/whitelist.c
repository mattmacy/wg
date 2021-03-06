#include <sys/endian.h>

#include <sys/whitelist.h>
#include <sys/peer.h>

static void
native_endian(uint8_t *dst, const uint8_t *src, uint8_t bits)
{
	if (bits == 32) {
		*(uint32_t *)dst = ntohl(*(const uint32_t *)src);
	} else if (bits == 128) {
		((uint64_t *)dst)[0] = be64toh(((const uint64_t *)src)[0]);
		((uint64_t *)dst)[1] = be64toh(((const uint64_t *)src)[1]);
	}
}

static void
copy_and_assign_cidr(struct whitelist_node *node, const uint8_t *src,
    uint8_t cidr, uint8_t bits)
{
	node->cidr = cidr;
	node->bit_at_a = cidr / 8U;
#ifdef __LITTLE_ENDIAN
	node->bit_at_a ^= (bits / 8U - 1U) % 8U;
#endif
	node->bit_at_b = 7U - (cidr % 8U);
	node->bitlen = bits;
	memcpy(node->wn_bits, src, bits / 8U);
}
#define CHOOSE_NODE(parent, key) \
	parent->bit[(key[parent->bit_at_a] >> parent->bit_at_b) & 1]

static void
node_free_deferred(epoch_context_t ctx)
{
	struct whitelist_node *node;

	node = __containerof(ctx, struct whitelist_node, wn_epoch_ctx);
	free(node, M_WG);
}

static void
push_rcu(struct whitelist_node **stack,
		     volatile struct whitelist_node *p, unsigned int *len)
{
	if (p != NULL) {
		MPASS(*len < 128);
		stack[(*len)++] = __DEVOLATILE(void *, p);
	}
}

static void
root_free_rcu(epoch_context_t ctx)
{
	struct whitelist_node *node, *stack[128] = {
		__containerof(ctx, struct whitelist_node, wn_epoch_ctx) };
	unsigned int len = 1;

	while (len > 0 && (node = stack[--len])) {
		push_rcu(stack, node->wn_bit[0], &len);
		push_rcu(stack, node->wn_bit[1], &len);
		free(node, M_WG);
	}
}

static void
root_remove_peer_lists(struct whitelist_node *root)
{
	struct whitelist_node *node, *stack[128] = { root };
	unsigned int len = 1;

	while (len > 0 && (node = stack[--len])) {
		push_rcu(stack, node->wn_bit[0], &len);
		push_rcu(stack, node->wn_bit[1], &len);
		if (atomic_load_acq(node->peer))
			list_del(&node->peer_list);
	}
}

static void
walk_remove_by_peer(struct whitelist_node **top,
				struct wg_peer *peer, struct mtx *lock)
{
#define REF(p) atomic_load_acq((p))
#define PUSH(p) ({                                                             \
			MPASS(len < 128);											\
			stack[len++] = p;											\
	})

	struct whitelist_node **stack[128], **nptr;
	struct whitelist_node *node, *prev;
	unsigned int len;

	if (__predict_false(!peer || !REF(*top)))
		return;

	for (prev = NULL, len = 0, PUSH(top); len > 0; prev = node) {
		nptr = stack[len - 1];
		node = *nptr;
		if (!node) {
			--len;
			continue;
		}
		if (!prev || REF(prev->bit[0]) == node ||
		    REF(prev->bit[1]) == node) {
			if (REF(node->bit[0]))
				PUSH(&node->bit[0]);
			else if (REF(node->bit[1]))
				PUSH(&node->bit[1]);
		} else if (REF(node->bit[0]) == prev) {
			if (REF(node->bit[1]))
				PUSH(&node->bit[1]);
		} else {
			if (node->peer == peer) {
				node->peer = NULL;
				list_del_init(&node->peer_list);
				if (!node->bit[0] || !node->bit[1]) {
					*nptr = *&node->bit[!REF(node->bit[0])];
					call_rcu(&node->rcu, node_free_rcu);
					node = *nptr;
				}
			}
			--len;
		}
	}

#undef REF
#undef PUSH
}

static unsigned int
fls128(uint64_t a, uint64_t b)
{
	return a ? fls64(a) + 64U : fls64(b);
}

static uint8_t
common_bits(const struct whitelist_node *node, const uint8_t *key,
		      uint8_t bits)
{
	if (bits == 32)
		return 32U - fls(*(const uint32_t *)node->bits ^ *(const uint32_t *)key);
	else if (bits == 128)
		return 128U - fls128(
			*(const uint64_t *)&node->bits[0] ^ *(const uint64_t *)&key[0],
			*(const uint64_t *)&node->bits[8] ^ *(const uint64_t *)&key[8]);
	return 0;
}

static bool
prefix_matches(const struct whitelist_node *node, const uint8_t *key,
			   uint8_t bits)
{
	/* This could be much faster if it actually just compared the common
	 * bits properly, by precomputing a mask bswap(~0 << (32 - cidr)), and
	 * the rest, but it turns out that common_bits is already super fast on
	 * modern processors, even taking into account the unfortunate bswap.
	 * So, we just inline it like this instead.
	 */
	return common_bits(node, key, bits) >= node->cidr;
}

static struct whitelist_node *
find_node(struct whitelist_node *trie, uint8_t bits,
					 const uint8_t *key)
{
	struct whitelist_node *node = trie, *found = NULL;

	while (node && prefix_matches(node, key, bits)) {
		if (atomic_load_acq(node->peer))
			found = node;
		if (node->cidr == bits)
			break;
		node = rcu_dereference_bh(CHOOSE_NODE(node, key));
	}
	return found;
}

/* Returns a strong reference to a peer */
static struct wg_peer *
lookup(struct whitelist_node __rcu *root, uint8_t bits,
			      const void *be_ip)
{
	/* Aligned so it can be passed to fls/fls64 */
	uint8_t ip[16] __aligned(__alignof(uint64_t));
	struct whitelist_node *node;
	struct wg_peer *peer = NULL;

	native_endian(ip, be_ip, bits);

	rcu_read_lock_bh();
retry:
	node = find_node(rcu_dereference_bh(root), bits, ip);
	if (node) {
		peer = wg_peer_get_maybe_zero(rcu_dereference_bh(node->peer));
		if (!peer)
			goto retry;
	}
	rcu_read_unlock_bh();
	return peer;
}

static bool
node_placement(struct whitelist_node __rcu *trie, const uint8_t *key,
			   uint8_t cidr, uint8_t bits, struct whitelist_node **rnode,
			   struct mutex *lock)
{
	struct whitelist_node *node = rcu_dereference_protected(trie,
						lockdep_is_held(lock));
	struct whitelist_node *parent = NULL;
	bool exact = false;

	while (node && node->cidr <= cidr && prefix_matches(node, key, bits)) {
		parent = node;
		if (parent->cidr == cidr) {
			exact = true;
			break;
		}
		node = rcu_dereference_protected(CHOOSE_NODE(parent, key),
						 lockdep_is_held(lock));
	}
	*rnode = parent;
	return exact;
}

static int
add(struct whitelist_node __rcu **trie, uint8_t bits, const uint8_t *key,
	       uint8_t cidr, struct wg_peer *peer, struct mutex *lock)
{
	struct whitelist_node *node, *parent, *down, *newnode;

	if (__predict_false(cidr > bits || !peer))
		return (EINVAL);

	if (!atomic_load_acq(trie)) {
		node = kzalloc(sizeof(*node), GFP_KERNEL);
		if (__predict_false(!node))
			return (ENOMEM);
		RCU_INIT_POINTER(node->peer, peer);
		list_add_tail(&node->peer_list, &peer->whitelist_list);
		copy_and_assign_cidr(node, key, cidr, bits);
		rcu_assign_pointer(*trie, node);
		return (0);
	}
	if (node_placement(*trie, key, cidr, bits, &node, lock)) {
		rcu_assign_pointer(node->peer, peer);
		list_move_tail(&node->peer_list, &peer->whitelist_list);
		return (0);
	}

	newnode = kzalloc(sizeof(*newnode), GFP_KERNEL);
	if (__predict_false(!newnode))
		return (ENOMEM);
	RCU_INIT_POINTER(newnode->peer, peer);
	list_add_tail(&newnode->peer_list, &peer->whitelist_list);
	copy_and_assign_cidr(newnode, key, cidr, bits);

	if (!node) {
		down = rcu_dereference_protected(*trie, lockdep_is_held(lock));
	} else {
		down = rcu_dereference_protected(CHOOSE_NODE(node, key),
						 lockdep_is_held(lock));
		if (!down) {
			rcu_assign_pointer(CHOOSE_NODE(node, key), newnode);
			return 0;
		}
	}
	cidr = min(cidr, common_bits(down, key, bits));
	parent = node;

	if (newnode->cidr == cidr) {
		rcu_assign_pointer(CHOOSE_NODE(newnode, down->bits), down);
		if (!parent)
			rcu_assign_pointer(*trie, newnode);
		else
			rcu_assign_pointer(CHOOSE_NODE(parent, newnode->bits),
					   newnode);
	} else {
		node = kzalloc(sizeof(*node), GFP_KERNEL);
		if (__predict_false(!node)) {
			kfree(newnode);
			return (ENOMEM);
		}
		INIT_LIST_HEAD(&node->peer_list);
		copy_and_assign_cidr(node, newnode->bits, cidr, bits);

		rcu_assign_pointer(CHOOSE_NODE(node, down->bits), down);
		rcu_assign_pointer(CHOOSE_NODE(node, newnode->bits), newnode);
		if (!parent)
			rcu_assign_pointer(*trie, node);
		else
			rcu_assign_pointer(CHOOSE_NODE(parent, node->bits),
					   node);
	}
	return 0;
}

void
wg_whitelist_init(struct whitelist *table)
{
	table->root4 = table->root6 = NULL;
	table->seq = 1;
}

void
wg_whitelist_free(struct whitelist *table, struct mutex *lock)
{
	struct whitelist_node __rcu *old4 = table->root4, *old6 = table->root6;

	++table->seq;
	RCU_INIT_POINTER(table->root4, NULL);
	RCU_INIT_POINTER(table->root6, NULL);
	if (atomic_load_acq(old4)) {
		struct whitelist_node *node = rcu_dereference_protected(old4,
							lockdep_is_held(lock));

		root_remove_peer_lists(node);
		call_rcu(&node->rcu, root_free_rcu);
	}
	if (atomic_load_acq(old6)) {
		struct whitelist_node *node = rcu_dereference_protected(old6,
							lockdep_is_held(lock));

		root_remove_peer_lists(node);
		call_rcu(&node->rcu, root_free_rcu);
	}
}

int
wg_whitelist_insert_v4(struct whitelist *table, const struct in_addr *ip,
			    uint8_t cidr, struct wg_peer *peer, struct mutex *lock)
{
	/* Aligned so it can be passed to fls */
	uint8_t key[4] __aligned(__alignof(uint32_t));

	++table->seq;
	native_endian(key, (const uint8_t *)ip, 32);
	return add(&table->root4, 32, key, cidr, peer, lock);
}

int
wg_whitelist_insert_v6(struct whitelist *table, const struct in6_addr *ip,
			    uint8_t cidr, struct wg_peer *peer, struct mutex *lock)
{
	/* Aligned so it can be passed to fls64 */
	uint8_t key[16] __aligned(__alignof(uint64_t));

	++table->seq;
	native_endian(key, (const uint8_t *)ip, 128);
	return add(&table->root6, 128, key, cidr, peer, lock);
}

void
wg_whitelist_remove_by_peer(struct whitelist *table,
				  struct wg_peer *peer, struct mutex *lock)
{
	++table->seq;
	walk_remove_by_peer(&table->root4, peer, lock);
	walk_remove_by_peer(&table->root6, peer, lock);
}

int
wg_whitelist_read_node(struct whitelist_node *node, uint8_t ip[16], uint8_t *cidr)
{
	const unsigned int cidr_bytes = DIV_ROUND_UP(node->cidr, 8U);
	native_endian(ip, node->bits, node->bitlen);
	memset(ip + cidr_bytes, 0, node->bitlen / 8U - cidr_bytes);
	if (node->cidr)
		ip[cidr_bytes - 1U] &= ~0U << (-node->cidr % 8U);

	*cidr = node->cidr;
	return node->bitlen == 32 ? AF_INET : AF_INET6;
}

/* Returns a strong reference to a peer */
struct wg_peer *
wg_whitelist_lookup_dst(struct whitelist *table,
					 struct mbuf *skb)
{
	if (skb->protocol == htons(ETH_P_IP))
		return lookup(table->root4, 32, &ip_hdr(skb)->daddr);
	else if (skb->protocol == htons(ETH_P_IPV6))
		return lookup(table->root6, 128, &ipv6_hdr(skb)->daddr);
	return NULL;
}

/* Returns a strong reference to a peer */
struct wg_peer *
wg_whitelist_lookup_src(struct whitelist *table,
					 struct mbuf *skb)
{
	if (skb->protocol == htons(ETH_P_IP))
		return lookup(table->root4, 32, &ip_hdr(skb)->saddr);
	else if (skb->protocol == htons(ETH_P_IPV6))
		return lookup(table->root6, 128, &ipv6_hdr(skb)->saddr);
	return NULL;
}

//#include "selftest/whitelist.c"
