#ifndef _WG_NOISE_H
#define _WG_NOISE_H

#include "messages.h"
#include "peerlookup.h"

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/kref.h>

union noise_counter {
	struct {
		uint64_t counter;
		unsigned long backtrack[COUNTER_BITS_TOTAL / BITS_PER_LONG];
		spinlock_t lock;
	} receive;
	atomic64_t counter;
};

struct noise_symmetric_key {
	uint8_t key[NOISE_SYMMETRIC_KEY_LEN];
	union noise_counter counter;
	uint64_t birthdate;
	bool is_valid;
};

struct noise_keypair {
	struct index_hashtable_entry entry;
	struct noise_symmetric_key sending;
	struct noise_symmetric_key receiving;
	__le32 remote_index;
	bool i_am_the_initiator;
	uint64_t internal_id;

	struct kref refcount;
	struct rcu_head rcu;
};

struct noise_keypairs {
	struct noise_keypair __rcu *current_keypair;
	struct noise_keypair __rcu *previous_keypair;
	struct noise_keypair __rcu *next_keypair;
	spinlock_t keypair_update_lock;
};

struct noise_static_identity {
	uint8_t static_public[NOISE_PUBLIC_KEY_LEN];
	uint8_t static_private[NOISE_PUBLIC_KEY_LEN];
	struct rw_semaphore lock;
	bool has_identity;
};

enum noise_handshake_state {
	HANDSHAKE_ZEROED,
	HANDSHAKE_CREATED_INITIATION,
	HANDSHAKE_CONSUMED_INITIATION,
	HANDSHAKE_CREATED_RESPONSE,
	HANDSHAKE_CONSUMED_RESPONSE
};

struct noise_handshake {
	struct index_hashtable_entry entry;

	enum noise_handshake_state state;
	uint64_t last_initiation_consumption;

	struct noise_static_identity *static_identity;

	uint8_t ephemeral_private[NOISE_PUBLIC_KEY_LEN];
	uint8_t remote_static[NOISE_PUBLIC_KEY_LEN];
	uint8_t remote_ephemeral[NOISE_PUBLIC_KEY_LEN];
	uint8_t precomputed_static_static[NOISE_PUBLIC_KEY_LEN];

	uint8_t preshared_key[NOISE_SYMMETRIC_KEY_LEN];

	uint8_t hash[NOISE_HASH_LEN];
	uint8_t chaining_key[NOISE_HASH_LEN];

	uint8_t latest_timestamp[NOISE_TIMESTAMP_LEN];
	__le32 remote_index;

	/* Protects all members except the immutable (after noise_handshake_
	 * init): remote_static, precomputed_static_static, static_identity.
	 */
	struct rw_semaphore lock;
};

struct wg_device;

void wg_noise_init(void);
bool wg_noise_handshake_init(struct noise_handshake *handshake,
			   struct noise_static_identity *static_identity,
			   const uint8_t peer_public_key[NOISE_PUBLIC_KEY_LEN],
			   const uint8_t peer_preshared_key[NOISE_SYMMETRIC_KEY_LEN],
			   struct wg_peer *peer);
void wg_noise_handshake_clear(struct noise_handshake *handshake);
static inline void wg_noise_reset_last_sent_handshake(atomic64_t *handshake_ns)
{
	atomic64_set(handshake_ns, ktime_get_coarse_boottime_ns() -
				       (uint64_t)(REKEY_TIMEOUT + 1) * NSEC_PER_SEC);
}

void wg_noise_keypair_put(struct noise_keypair *keypair, bool unreference_now);
struct noise_keypair *wg_noise_keypair_get(struct noise_keypair *keypair);
void wg_noise_keypairs_clear(struct noise_keypairs *keypairs);
bool wg_noise_received_with_keypair(struct noise_keypairs *keypairs,
				    struct noise_keypair *received_keypair);
void wg_noise_expire_current_peer_keypairs(struct wg_peer *peer);

void wg_noise_set_static_identity_private_key(
	struct noise_static_identity *static_identity,
	const uint8_t private_key[NOISE_PUBLIC_KEY_LEN]);
bool wg_noise_precompute_static_static(struct wg_peer *peer);

bool
wg_noise_handshake_create_initiation(struct message_handshake_initiation *dst,
				     struct noise_handshake *handshake);
struct wg_peer *
wg_noise_handshake_consume_initiation(struct message_handshake_initiation *src,
				      struct wg_device *wg);

bool wg_noise_handshake_create_response(struct message_handshake_response *dst,
					struct noise_handshake *handshake);
struct wg_peer *
wg_noise_handshake_consume_response(struct message_handshake_response *src,
				    struct wg_device *wg);

bool wg_noise_handshake_begin_session(struct noise_handshake *handshake,
				      struct noise_keypairs *keypairs);

#endif /* _WG_NOISE_H */
