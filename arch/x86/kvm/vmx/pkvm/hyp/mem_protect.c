/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/bitfield.h>
#include <pkvm.h>
#include <gfp.h>
#include "pkvm_hyp.h"
#include "mem_protect.h"
#include "pgtable.h"
#include "ept.h"

struct check_walk_data {
	int 			nstate;
	enum pkvm_page_state	*desired;
};

enum pkvm_component_id {
	PKVM_ID_HOST,
	PKVM_ID_HYP,
	PKVM_ID_GUEST,
};

struct pkvm_mem_trans_desc {
	enum pkvm_component_id	id;
	union {
		struct {
			struct pkvm_pgtable	*pgt_override;
			u64	addr;
		} host;

		struct {
			u64	addr;
		} hyp;

		struct {
			struct pkvm_pgtable	*pgt;
			u64			addr;
			u64			phys;
		} guest;
	};
	u64			prot;
};

struct pkvm_mem_transition {
	u64				size;
	struct pkvm_mem_trans_desc	initiator;
	struct pkvm_mem_trans_desc	completer;
};

static void guest_pgstate_pgt_lock(struct pkvm_pgtable *pgt)
{
	pkvm_spin_lock(&pgstate_pgt_to_shadow_vm(pgt)->lock);
}

static void guest_pgstate_pgt_unlock(struct pkvm_pgtable *pgt)
{
	pkvm_spin_unlock(&pgstate_pgt_to_shadow_vm(pgt)->lock);
}

static u64 pkvm_init_invalid_leaf_owner(pkvm_id owner_id)
{
	/* the page owned by others also means NOPAGE in page state */
	return FIELD_PREP(PKVM_INVALID_PTE_OWNER_MASK, owner_id) |
		FIELD_PREP(PKVM_PAGE_STATE_PROT_MASK, PKVM_NOPAGE);
}

static int host_ept_set_owner_locked(struct pkvm_pgtable *pgt_override, phys_addr_t addr,
				     u64 size, pkvm_id owner_id)
{
	u64 annotation = pkvm_init_invalid_leaf_owner(owner_id);

	/*
	 * The memory [addr, addr + size) will be unmaped from host ept. At the
	 * same time, the annotation with a NOPAGE flag will be put in the
	 * invalid pte that has been unmaped. And these information record that
	 * the page has been used by some guest and it's id can be read from
	 * annotation. Also when later these page back to host, the annotation
	 * will help to check the right page transition.
	 */
	return pkvm_pgtable_annotate(pgt_override ? pgt_override : pkvm_hyp->host_vm.ept,
				     addr, size, annotation);
}

static int host_ept_create_idmap_locked(struct pkvm_pgtable *pgt_override, u64 addr,
					u64 size, int pgsz_mask, u64 prot)
{
	return pkvm_pgtable_map(pgt_override ? pgt_override : pkvm_hyp->host_vm.ept,
				addr, addr, size, pgsz_mask, prot, NULL);
}

static int
__check_page_state_walker(struct pkvm_pgtable *pgt, unsigned long vaddr,
			  unsigned long vaddr_end, int level, void *ptep,
			  unsigned long flags, struct pgt_flush_data *flush_data,
			  void *const arg)
{
	struct check_walk_data *data = arg;
	int i;

	for (i = 0; i < data->nstate; i++)
		if (pkvm_getstate(*(u64 *)ptep) == data->desired[i])
			return 0;

	return -EPERM;
}

static int check_page_state_range(struct pkvm_pgtable *pgt, u64 addr, u64 size,
				  enum pkvm_page_state *states, int nstate)
{
	struct check_walk_data data = {
		.nstate			= nstate,
		.desired		= states,
	};
	struct pkvm_pgtable_walker walker = {
		.cb		= __check_page_state_walker,
		.flags		= PKVM_PGTABLE_WALK_LEAF,
		.arg		= &data,
	};

	return pgtable_walk(pgt, addr, size, true, &walker);
}

static int __host_check_page_state_range(struct pkvm_pgtable *pgt_override, u64 addr,
					 u64 size, enum pkvm_page_state state)
{
	struct pkvm_pgtable *host_ept = pgt_override ? pgt_override : pkvm_hyp->host_vm.ept;

	return check_page_state_range(host_ept, addr, size, &state, 1);
}

static int __guest_check_page_state_range(struct pkvm_pgtable *pgt,
					  u64 addr, u64 size,
					  enum pkvm_page_state state)
{
	return check_page_state_range(pgt, addr, size, &state, 1);
}

static pkvm_id pkvm_guest_id(struct pkvm_pgtable *pgt)
{
	/* Using the shadow_vm_handle as guest_id. */
	return pgstate_pgt_to_shadow_vm(pgt)->shadow_vm_handle;
}

static pkvm_id __pkvm_owner_id(const struct pkvm_mem_trans_desc *desc)
{
	switch (desc->id) {
	case PKVM_ID_HYP:
		return pkvm_hyp_id;
	case PKVM_ID_GUEST:
		return pkvm_guest_id(desc->guest.pgt);
	default:
		WARN_ON(1);
		return -1;
	}
}

static pkvm_id initiator_owner_id(const struct pkvm_mem_transition *tx)
{
	return __pkvm_owner_id(&tx->initiator);
}

static pkvm_id completer_owner_id(const struct pkvm_mem_transition *tx)
{
	return __pkvm_owner_id(&tx->completer);
}

static int host_request_donation(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.host.addr;
	u64 size = tx->size;

	return __host_check_page_state_range(tx->initiator.host.pgt_override,
					     addr, size, PKVM_PAGE_OWNED);
}

static int guest_request_donation(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.guest.addr;
	u64 size = tx->size;
	enum pkvm_page_state states[] = { PKVM_PAGE_OWNED,
					  PKVM_PAGE_SHARED_OWNED,
					};

	/*
	 * When destroying vm, there may be multiple page state in the guest
	 * pgstate pgt. In this case, both page state is ok to be reclaimed
	 * back by host.
	 */
	return check_page_state_range(tx->initiator.guest.pgt,
				      addr, size, states, ARRAY_SIZE(states));
}

static int host_ack_donation(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.host.addr;
	u64 size = tx->size;
	enum pkvm_page_state states[] = { PKVM_NOPAGE,
					  PKVM_PAGE_SHARED_BORROWED,
					};
	struct pkvm_pgtable *host_ept = tx->completer.host.pgt_override ?
					tx->completer.host.pgt_override :
					pkvm_hyp->host_vm.ept;

	/* Same as guest_request_donation. */
	return check_page_state_range(host_ept, addr, size, states, ARRAY_SIZE(states));
}

static int guest_ack_donation(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.guest.addr;
	u64 size = tx->size;

	return __guest_check_page_state_range(tx->completer.guest.pgt, addr,
					      size, PKVM_NOPAGE);
}

static int check_donation(const struct pkvm_mem_transition *tx)
{
	int ret;

	switch (tx->initiator.id) {
	case PKVM_ID_HOST:
		ret = host_request_donation(tx);
		break;
	case PKVM_ID_HYP:
		ret = 0;
		break;
	case PKVM_ID_GUEST:
		ret = guest_request_donation(tx);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	switch (tx->completer.id) {
	case PKVM_ID_HOST:
		ret = host_ack_donation(tx);
		break;
	case PKVM_ID_HYP:
		ret = 0;
		break;
	case PKVM_ID_GUEST:
		ret = guest_ack_donation(tx);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int host_initiate_donation(const struct pkvm_mem_transition *tx)
{
	pkvm_id owner_id = completer_owner_id(tx);
	u64 addr = tx->initiator.host.addr;
	u64 size = tx->size;

	return host_ept_set_owner_locked(tx->initiator.host.pgt_override, addr, size, owner_id);
}

static int guest_initiate_donation(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.guest.addr;
	u64 phys = tx->initiator.guest.phys;
	u64 size = tx->size;

	return pkvm_pgtable_unmap_safe(tx->initiator.guest.pgt, addr, phys, size, NULL);
}

static int host_complete_donation(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.host.addr;
	u64 size = tx->size;
	u64 prot = pkvm_mkstate(tx->completer.prot, PKVM_PAGE_OWNED);

	return host_ept_create_idmap_locked(tx->completer.host.pgt_override, addr, size, 0, prot);
}

static int guest_complete_donation(const struct pkvm_mem_transition *tx)
{
	struct pkvm_pgtable *pgt = tx->completer.guest.pgt;
	u64 addr = tx->completer.guest.addr;
	u64 size = tx->size;
	u64 phys = tx->completer.guest.phys;
	u64 prot = tx->completer.prot;

	prot = pkvm_mkstate(prot, PKVM_PAGE_OWNED);
	return pkvm_pgtable_map(pgt, addr, phys, size, 0, prot, NULL);
}

static int __do_donate(const struct pkvm_mem_transition *tx)
{
	int ret;

	switch (tx->initiator.id) {
	case PKVM_ID_HOST:
		ret = host_initiate_donation(tx);
		break;
	case PKVM_ID_HYP:
		ret = 0;
		break;
	case PKVM_ID_GUEST:
		ret = guest_initiate_donation(tx);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	switch (tx->completer.id) {
	case PKVM_ID_HOST:
		ret = host_complete_donation(tx);
		break;
	case PKVM_ID_HYP:
		ret = 0;
		break;
	case PKVM_ID_GUEST:
		ret = guest_complete_donation(tx);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/*
 * do_donate - the page owner transfer ownership to another component.
 *
 * Initiator: OWNED	=> NO_PAGE
 * Completer: NO_APGE	=> OWNED
 *
 * The special component is pkvm_hyp, due to pkvm_hyp can access all of the
 * memory, so there need to do nothing if the page owner transfer to hyp or
 * hyp transfer to other entity.
 */
static int do_donate(const struct pkvm_mem_transition *donation)
{
	int ret;

	ret = check_donation(donation);
	if (ret)
		return ret;

	return WARN_ON(__do_donate(donation));
}

int __pkvm_host_donate_hyp(u64 hpa, u64 size)
{
	int ret;
	u64 hyp_addr = (u64)__pkvm_va(hpa);
	struct pkvm_mem_transition donation = {
		.size		= size,
		.initiator	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
		},
		.completer	= {
			.id	= PKVM_ID_HYP,
			.hyp	= {
				.addr = hyp_addr,
			},
		},
	};

	host_ept_lock();

	ret = do_donate(&donation);

	host_ept_unlock();

	return ret;
}

int __pkvm_hyp_donate_host(u64 hpa, u64 size)
{
	int ret;
	u64 hyp_addr = (u64)__pkvm_va(hpa);
	struct pkvm_mem_transition donation = {
		.size		= size,
		.initiator	= {
			.id	= PKVM_ID_HYP,
			.hyp	= {
				.addr	= hyp_addr,
			},
		},
		.completer	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
			.prot	= HOST_EPT_DEF_MEM_PROT,
		},
	};

	host_ept_lock();

	ret = do_donate(&donation);

	host_ept_unlock();

	return ret;
}

int __pkvm_host_donate_guest(u64 hpa, struct pkvm_pgtable *guest_pgt,
			     u64 gpa, u64 size, u64 prot)
{
	int ret;
	struct pkvm_mem_transition donation = {
		.size		= size,
		.initiator	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
		},
		.completer	= {
			.id	= PKVM_ID_GUEST,
			.guest	= {
				.pgt	= guest_pgt,
				.addr	= gpa,
				.phys	= hpa,
			},
			.prot	= prot,
		},
	};

	host_ept_lock();

	ret = do_donate(&donation);

	host_ept_unlock();

	return ret;
}

/*
 * Fastpath interface will use the host EPT instance without doing tlbflushing
 * to have a better performance. It is usually used in the scenario that caller
 * needs to change a bunch of pages' state without having the TLB flushing
 * overhead in the each iteration, but caller still needs to do TLB flushing
 * after completing all the iterations.
 */
int __pkvm_host_donate_guest_fastpath(u64 hpa, struct pkvm_pgtable *guest_pgt,
				      u64 gpa, u64 size, u64 prot)
{
	int ret;
	struct pkvm_mem_transition donation = {
		.size		= size,
		.initiator	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.pgt_override	= pkvm_hyp->host_vm.ept_notlbflush,
				.addr		= hpa,
			},
		},
		.completer	= {
			.id	= PKVM_ID_GUEST,
			.guest	= {
				.pgt		= guest_pgt,
				.addr		= gpa,
				.phys		= hpa,
			},
			.prot	= prot,
		},
	};

	host_ept_lock();

	ret = do_donate(&donation);

	host_ept_unlock();

	return ret;
}

int __pkvm_host_undonate_guest(u64 hpa, struct pkvm_pgtable *guest_pgt,
			       u64 gpa, u64 size)
{
	int ret;
	struct pkvm_mem_transition donation = {
		.size		= size,
		.initiator	= {
			.id	= PKVM_ID_GUEST,
			.guest	= {
				.addr	= gpa,
				.phys	= hpa,
				.pgt	= guest_pgt,
			},
		},
		.completer	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
			.prot	= HOST_EPT_DEF_MEM_PROT,
		},
	};

	host_ept_lock();

	ret = do_donate(&donation);

	host_ept_unlock();

	return ret;
}

static int host_request_share(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.host.addr;
	u64 size = tx->size;

	return __host_check_page_state_range(tx->initiator.host.pgt_override,
					     addr, size, PKVM_PAGE_OWNED);
}

static int guest_request_share(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.guest.addr;
	u64 size = tx->size;

	return __guest_check_page_state_range(tx->initiator.guest.pgt,
					      addr, size, PKVM_PAGE_OWNED);
}

static int host_ack_share(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.host.addr;
	u64 size = tx->size;

	return __host_check_page_state_range(tx->completer.host.pgt_override,
					     addr, size, PKVM_NOPAGE);
}

static int guest_ack_share(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.guest.addr;
	u64 size = tx->size;

	return __guest_check_page_state_range(tx->completer.guest.pgt, addr,
					      size, PKVM_NOPAGE);
}

static int check_share(const struct pkvm_mem_transition *tx)
{
	int ret;

	switch (tx->initiator.id) {
	case PKVM_ID_HOST:
		ret = host_request_share(tx);
		break;
	case PKVM_ID_GUEST:
		ret = guest_request_share(tx);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	switch (tx->completer.id) {
	case PKVM_ID_HOST:
		ret = host_ack_share(tx);
		break;
	case PKVM_ID_GUEST:
		ret = guest_ack_share(tx);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int host_initiate_share(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.host.addr;
	u64 size = tx->size;
	u64 prot = pkvm_mkstate(tx->initiator.prot, PKVM_PAGE_SHARED_OWNED);

	return host_ept_create_idmap_locked(tx->initiator.host.pgt_override, addr, size, 0, prot);
}

static int guest_initiate_share(const struct pkvm_mem_transition *tx)
{
	struct pkvm_pgtable *pgt = tx->initiator.guest.pgt;
	u64 addr = tx->initiator.guest.addr;
	u64 phys = tx->initiator.guest.phys;
	u64 size = tx->size;
	u64 prot = pkvm_mkstate(tx->initiator.prot, PKVM_PAGE_SHARED_OWNED);

	return pkvm_pgtable_map(pgt, addr, phys, size, 0, prot, NULL);
}

static int host_complete_share(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.host.addr;
	u64 size = tx->size;
	u64 prot = pkvm_mkstate(tx->completer.prot, PKVM_PAGE_SHARED_BORROWED);

	return host_ept_create_idmap_locked(tx->completer.host.pgt_override, addr, size, 0, prot);
}

static int guest_complete_share(const struct pkvm_mem_transition *tx)
{
	struct pkvm_pgtable *pgt = tx->completer.guest.pgt;
	u64 addr = tx->completer.guest.addr;
	u64 size = tx->size;
	u64 phys = tx->completer.guest.phys;
	u64 prot = tx->completer.prot;

	prot = pkvm_mkstate(prot, PKVM_PAGE_SHARED_BORROWED);
	return pkvm_pgtable_map(pgt, addr, phys, size, 0, prot, NULL);
}

static int __do_share(const struct pkvm_mem_transition *tx)
{
	int ret;

	switch (tx->initiator.id) {
	case PKVM_ID_HOST:
		ret = host_initiate_share(tx);
		break;
	case PKVM_ID_GUEST:
		ret = guest_initiate_share(tx);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	switch (tx->completer.id) {
	case PKVM_ID_HOST:
		ret = host_complete_share(tx);
		break;
	case PKVM_ID_GUEST:
		ret = guest_complete_share(tx);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/*
 * do_share() - The page owner grants access to another component with a given
 * set of permissions.
 *
 * Initiator: OWNED	=> SHARED_OWNED
 * Completer: NOPAGE	=> SHARED_BORROWED
 */
static int do_share(const struct pkvm_mem_transition *share)
{
	int ret;

	ret = check_share(share);
	if (ret)
		return ret;

	return WARN_ON(__do_share(share));
}

int __pkvm_host_share_guest(u64 hpa, struct pkvm_pgtable *guest_pgt,
			    u64 gpa, u64 size, u64 prot)
{
	int ret;
	struct pkvm_mem_transition share = {
		.size		= size,
		.initiator	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
			.prot	= HOST_EPT_DEF_MEM_PROT,
		},
		.completer	= {
			.id	= PKVM_ID_GUEST,
			.guest	= {
				.pgt	= guest_pgt,
				.addr	= gpa,
				.phys	= hpa,
			},
			.prot	= prot,
		},
	};

	host_ept_lock();

	ret = do_share(&share);

	host_ept_unlock();

	return ret;
}

static int __pkvm_guest_share_host_page(struct pkvm_pgtable *guest_pgt,
					u64 gpa, u64 hpa, u64 guest_prot)
{
	struct pkvm_mem_transition share = {
		.size		= PAGE_SIZE,
		.initiator	= {
			.id	= PKVM_ID_GUEST,
			.guest	= {
				.pgt	= guest_pgt,
				.addr	= gpa,
				.phys	= hpa,
			},
			.prot	= guest_prot,
		},
		.completer	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
			.prot	= HOST_EPT_DEF_MEM_PROT,
		},
	};

	return do_share(&share);
}

int __pkvm_guest_share_host(struct pkvm_pgtable *guest_pgt,
			    u64 gpa, u64 size)
{
	unsigned long hpa;
	u64 prot;
	int ret = 0;

	if (!PAGE_ALIGNED(size))
		return -EINVAL;

	host_ept_lock();
	guest_pgstate_pgt_lock(guest_pgt);

	while (size) {
		pkvm_pgtable_lookup(guest_pgt, gpa, &hpa, &prot, NULL);
		if (hpa == INVALID_ADDR) {
			ret = -EINVAL;
			break;
		}

		ret = __pkvm_guest_share_host_page(guest_pgt, gpa, hpa, prot);
		if (ret)
			break;

		size -= PAGE_SIZE;
		gpa += PAGE_SIZE;
	}

	guest_pgstate_pgt_unlock(guest_pgt);
	host_ept_unlock();

	return ret;
}

static int host_request_unshare(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.host.addr;
	u64 size = tx->size;

	return __host_check_page_state_range(tx->initiator.host.pgt_override, addr,
					     size, PKVM_PAGE_SHARED_OWNED);
}

static int guest_request_unshare(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.guest.addr;
	u64 size = tx->size;

	return __guest_check_page_state_range(tx->initiator.guest.pgt,
					      addr, size, PKVM_PAGE_SHARED_OWNED);
}

static int host_ack_unshare(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.host.addr;
	u64 size = tx->size;

	return __host_check_page_state_range(tx->completer.host.pgt_override, addr,
					     size, PKVM_PAGE_SHARED_BORROWED);
}

static int guest_ack_unshare(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.guest.addr;
	u64 size = tx->size;

	return __guest_check_page_state_range(tx->completer.guest.pgt, addr,
					      size, PKVM_PAGE_SHARED_BORROWED);
}

int check_unshare(const struct pkvm_mem_transition *tx)
{
	int ret;

	switch (tx->initiator.id) {
	case PKVM_ID_HOST:
		ret = host_request_unshare(tx);
		break;
	case PKVM_ID_GUEST:
		ret = guest_request_unshare(tx);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	switch (tx->completer.id) {
	case PKVM_ID_HOST:
		ret = host_ack_unshare(tx);
		break;
	case PKVM_ID_GUEST:
		ret = guest_ack_unshare(tx);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int host_initiate_unshare(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.host.addr;
	u64 size = tx->size;
	u64 prot = pkvm_mkstate(tx->initiator.prot, PKVM_PAGE_OWNED);

	return host_ept_create_idmap_locked(tx->initiator.host.pgt_override, addr, size, 0, prot);
}

static int guest_initiate_unshare(const struct pkvm_mem_transition *tx)
{
	struct pkvm_pgtable *pgt = tx->initiator.guest.pgt;
	u64 addr = tx->initiator.guest.addr;
	u64 phys = tx->initiator.guest.phys;
	u64 size = tx->size;
	u64 prot = pkvm_mkstate(tx->initiator.prot, PKVM_PAGE_OWNED);

	return pkvm_pgtable_map(pgt, addr, phys, size, 0, prot, NULL);
}

static int host_complete_unshare(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->completer.host.addr;
	u64 size = tx->size;
	u64 owner_id = initiator_owner_id(tx);

	return host_ept_set_owner_locked(tx->completer.host.pgt_override, addr, size, owner_id);
}

static int guest_complete_unshare(const struct pkvm_mem_transition *tx)
{
	struct pkvm_pgtable *pgt = tx->completer.guest.pgt;
	u64 addr = tx->completer.guest.addr;
	u64 phys = tx->completer.guest.phys;
	u64 size = tx->size;

	return pkvm_pgtable_unmap_safe(pgt, addr, phys, size, NULL);
}

static int __do_unshare(struct pkvm_mem_transition *tx)
{
	int ret;

	switch (tx->initiator.id) {
	case PKVM_ID_HOST:
		ret = host_initiate_unshare(tx);
		break;
	case PKVM_ID_GUEST:
		ret = guest_initiate_unshare(tx);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	switch (tx->completer.id) {
	case PKVM_ID_HOST:
		ret = host_complete_unshare(tx);
		break;
	case PKVM_ID_GUEST:
		ret = guest_complete_unshare(tx);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/*
 * do_unshare() - The page owner take back the page access for another
 * component.
 *
 * Initiator: SHARED_OWNED	=> OWNED
 * Completer: SHARED_BORROWED	=> NOPAGE
 */
int do_unshare(struct pkvm_mem_transition *share)
{
	int ret;

	ret = check_unshare(share);
	if (ret)
		return ret;

	return WARN_ON(__do_unshare(share));
}

int __pkvm_host_unshare_guest(u64 hpa, struct pkvm_pgtable *guest_pgt,
			      u64 gpa, u64 size)
{
	int ret;
	struct pkvm_mem_transition share = {
		.size = size,
		.initiator	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
			.prot	= HOST_EPT_DEF_MEM_PROT,
		},
		.completer	= {
			.id	= PKVM_ID_GUEST,
			.guest	= {
				.pgt	= guest_pgt,
				.addr	= gpa,
				.phys	= hpa,
			},
		},
	};

	host_ept_lock();

	ret = do_unshare(&share);

	host_ept_unlock();

	return ret;
}

static int __pkvm_guest_unshare_host_page(struct pkvm_pgtable *guest_pgt,
					  u64 gpa, u64 hpa, u64 guest_prot)
{
	struct pkvm_mem_transition share = {
		.size = PAGE_SIZE,
		.initiator	= {
			.id	= PKVM_ID_GUEST,
			.guest	= {
				.pgt	= guest_pgt,
				.addr	= gpa,
				.phys	= hpa,
			},
			.prot	= guest_prot,
		},
		.completer	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
		},
	};

	return do_unshare(&share);
}

int __pkvm_guest_unshare_host(struct pkvm_pgtable *guest_pgt,
			      u64 gpa, u64 size)
{
	unsigned long hpa;
	u64 prot;
	int ret = 0;

	host_ept_lock();
	guest_pgstate_pgt_lock(guest_pgt);

	while (size) {
		pkvm_pgtable_lookup(guest_pgt, gpa, &hpa, &prot, NULL);
		if (hpa == INVALID_ADDR) {
			ret = -EINVAL;
			break;
		}

		ret = __pkvm_guest_unshare_host_page(guest_pgt, gpa, hpa, prot);
		if (ret)
			break;

		size -= PAGE_SIZE;
		gpa += PAGE_SIZE;
	}

	guest_pgstate_pgt_unlock(guest_pgt);
	host_ept_unlock();

	return ret;
}
