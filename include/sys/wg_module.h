#ifndef MODULE_H_
#define MODULE_H_

#include <sys/mbuf.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>

#include <net/iflib.h>

#include <sys/whitelist.h>
#include <sys/noise.h>

MALLOC_DECLARE(M_WG);
#define zfree(addr, type)						\
	do {										\
		explicit_bzero(addr, sizeof(*addr));	\
		free(addr, type);						\
	} while (0)

struct crypt_queue {
	//struct ptr_ring ring;
	union {
		struct {
			//struct multicore_worker __percpu *worker;
			int last_cpu;
		};
		//struct work_struct work;
	};
};

/*
 * Workaround FreeBSD's shitty typed atomic
 * accessors
 */
#define __ATOMIC_LOAD_SIZE						\
	({									\
	switch (size) {							\
	case 1: *(uint8_t *)res = *(volatile uint8_t *)p; break;		\
	case 2: *(uint16_t *)res = *(volatile uint16_t *)p; break;		\
	case 4: *(uint32_t *)res = *(volatile uint32_t *)p; break;		\
	case 8: *(uint64_t *)res = *(volatile uint64_t *)p; break;		\
	}								\
})

static inline void
__atomic_load_acq_size(volatile void *p, void *res, int size)
{
	__ATOMIC_LOAD_SIZE;
}

#define atomic_load_acq(x)						\
	({											\
	union { __typeof(x) __val; char __c[1]; } __u;			\
	__atomic_load_acq_size(&(x), __u.__c, sizeof(x));		\
	__u.__val;												\
})



struct wg_hashtable {
	struct mtx			 h_mtx;
	SIPHASH_KEY			 h_secret;
	LIST_HEAD(, wg_peer)		*h_peers;
	u_long				 h_peers_mask;
	size_t				 h_num_peers;
	LIST_HEAD(, noise_keypair)	*h_keys;
	u_long				 h_keys_mask;
	size_t				 h_num_keys;
};

struct wg_softc {
	if_softc_ctx_t shared;
	if_ctx_t wg_ctx;
	struct ifnet *wg_ifp;

	struct wg_socket *wg_socket;
	struct wg_hashtable wg_table;
	struct noise_local sc_local;
	unsigned int wg_npeers, wg_gen;
	CK_LIST_HEAD(, wg_peer) wg_peer_list;
	CK_LIST_HEAD(, noise_keypair) wg_keypair_list;
	struct whitelist wg_whitelist;
	struct mbufq wg_handshake_queue;
	struct gtask wg_handshake_task;
	struct wg_cookie_checker wg_cookie_checker;

	

#if 0
	struct crypt_queue encrypt_queue, decrypt_queue;
	struct net *creating_net;
	struct workqueue_struct *handshake_receive_wq, *handshake_send_wq;
	struct workqueue_struct *packet_crypt_wq;
	struct sk_buff_head incoming_handshakes;
	int incoming_handshake_cpu;
	struct multicore_worker __percpu *incoming_handshakes_worker;
	struct cookie_checker cookie_checker;
	struct allowedips peer_allowedips;
	u32 fwmark;
	u16 incoming_port;
	bool have_creating_net_ref;
#endif
};

int wg_ctx_init(void);
void wg_ctx_uninit(void);


#endif