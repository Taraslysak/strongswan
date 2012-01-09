/*
 * Copyright (C) 2012 Martin Willi
 * Copyright (C) 2012 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "phase1.h"

#include <daemon.h>
#include <sa/ikev1/keymat_v1.h>
#include <encoding/payloads/ke_payload.h>
#include <encoding/payloads/nonce_payload.h>

typedef struct private_phase1_t private_phase1_t;

/**
 * Private data of an phase1_t object.
 */
struct private_phase1_t {

	/**
	 * Public phase1_t interface.
	 */
	phase1_t public;

	/**
	 * IKE_SA we negotiate
	 */
	ike_sa_t *ike_sa;

	/**
	 * Acting as initiator
	 */
	bool initiator;

	/**
	 * Extracted SA payload bytes
	 */
	chunk_t sa_payload;

	/**
	 * DH exchange
	 */
	diffie_hellman_t *dh;

	/**
	 * Keymat derivation (from SA)
	 */
	keymat_v1_t *keymat;

	/**
	 * Received public DH value from peer
	 */
	chunk_t dh_value;

	/**
	 * Initiators nonce
	 */
	chunk_t nonce_i;

	/**
	 * Responder nonce
	 */
	chunk_t nonce_r;
};

/**
 * Get the first authentcation config from peer config
 */
static auth_cfg_t *get_auth_cfg(peer_cfg_t *peer_cfg, bool local)
{
	enumerator_t *enumerator;
	auth_cfg_t *cfg = NULL;

	enumerator = peer_cfg->create_auth_cfg_enumerator(peer_cfg, local);
	enumerator->enumerate(enumerator, &cfg);
	enumerator->destroy(enumerator);
	return cfg;
}

/**
 * Lookup a shared secret for this IKE_SA
 */
static shared_key_t *lookup_shared_key(private_phase1_t *this,
									   peer_cfg_t *peer_cfg)
{
	host_t *me, *other;
	identification_t *my_id, *other_id;
	shared_key_t *shared_key = NULL;
	auth_cfg_t *my_auth, *other_auth;
	enumerator_t *enumerator;

	/* try to get a PSK for IP addresses */
	me = this->ike_sa->get_my_host(this->ike_sa);
	other = this->ike_sa->get_other_host(this->ike_sa);
	my_id = identification_create_from_sockaddr(me->get_sockaddr(me));
	other_id = identification_create_from_sockaddr(other->get_sockaddr(other));
	if (my_id && other_id)
	{
		shared_key = lib->credmgr->get_shared(lib->credmgr, SHARED_IKE,
											  my_id, other_id);
	}
	DESTROY_IF(my_id);
	DESTROY_IF(other_id);
	if (shared_key)
	{
		return shared_key;
	}

	if (peer_cfg)
	{	/* as initiator, use identities from configuraiton */
		my_auth = get_auth_cfg(peer_cfg, TRUE);
		other_auth = get_auth_cfg(peer_cfg, FALSE);
		if (my_auth && other_auth)
		{
			my_id = my_auth->get(my_auth, AUTH_RULE_IDENTITY);
			other_id = other_auth->get(other_auth, AUTH_RULE_IDENTITY);
			if (my_id && other_id)
			{
				shared_key = lib->credmgr->get_shared(lib->credmgr, SHARED_IKE,
													  my_id, other_id);
				if (!shared_key)
				{
					DBG1(DBG_IKE, "no shared key found for '%Y'[%H] - '%Y'[%H]",
						 my_id, me, other_id, other);
				}
			}
		}
		return shared_key;
	}
	/* as responder, we try to find a config by IP */
	enumerator = charon->backends->create_peer_cfg_enumerator(charon->backends,
												me, other, NULL, NULL, IKEV1);
	while (enumerator->enumerate(enumerator, &peer_cfg))
	{
		my_auth = get_auth_cfg(peer_cfg, TRUE);
		other_auth = get_auth_cfg(peer_cfg, FALSE);
		if (my_auth && other_auth)
		{
			my_id = my_auth->get(my_auth, AUTH_RULE_IDENTITY);
			other_id = other_auth->get(other_auth, AUTH_RULE_IDENTITY);
			if (my_id && other_id)
			{
				shared_key = lib->credmgr->get_shared(lib->credmgr, SHARED_IKE,
													  my_id, other_id);
				if (shared_key)
				{
					break;
				}
				else
				{
					DBG1(DBG_IKE, "no shared key found for '%Y'[%H] - '%Y'[%H]",
						 my_id, me, other_id, other);
				}
			}
		}
	}
	enumerator->destroy(enumerator);
	if (!peer_cfg)
	{
		DBG1(DBG_IKE, "no shared key found for %H - %H", me, other);
	}
	return shared_key;
}

METHOD(phase1_t, create_hasher, bool,
	private_phase1_t *this, proposal_t *proposal)
{
	return this->keymat->create_hasher(this->keymat, proposal);
}

METHOD(phase1_t, create_dh, bool,
	private_phase1_t *this, diffie_hellman_group_t group)
{
	this->dh = this->keymat->keymat.create_dh(&this->keymat->keymat, group);
	return this->dh != NULL;
}

METHOD(phase1_t, derive_keys, bool,
	private_phase1_t *this, peer_cfg_t *peer_cfg, auth_method_t method,
	proposal_t *proposal)
{
	shared_key_t *shared_key = NULL;

	switch (method)
	{
		case AUTH_PSK:
		case AUTH_XAUTH_INIT_PSK:
		case AUTH_XAUTH_RESP_PSK:
			shared_key = lookup_shared_key(this, peer_cfg);
			if (!shared_key)
			{
				return FALSE;
			}
			break;
		default:
			break;
	}

	if (!this->keymat->derive_ike_keys(this->keymat, proposal,
						this->dh, this->dh_value, this->nonce_i, this->nonce_r,
						this->ike_sa->get_id(this->ike_sa), method, shared_key))
	{
		DESTROY_IF(shared_key);
		DBG1(DBG_IKE, "key derivation for %N failed", auth_method_names, method);
		return FALSE;
	}
	DESTROY_IF(shared_key);
	charon->bus->ike_keys(charon->bus, this->ike_sa, this->dh,
						  this->nonce_i, this->nonce_r, NULL);
	return TRUE;
}

/**
 * Check if a peer skipped authentication by using Hybrid authentication
 */
static bool skipped_auth(private_phase1_t *this,
						 auth_method_t method, bool local)
{
	bool initiator;

	initiator = local == this->initiator;
	if (initiator && method == AUTH_HYBRID_INIT_RSA)
	{
		return TRUE;
	}
	if (!initiator && method == AUTH_HYBRID_RESP_RSA)
	{
		return TRUE;
	}
	return FALSE;
}

/**
 * Check if remote authentication constraints fulfilled
 */
static bool check_constraints(private_phase1_t *this, auth_method_t method)
{
	identification_t *id;
	auth_cfg_t *auth, *cfg;
	peer_cfg_t *peer_cfg;

	auth = this->ike_sa->get_auth_cfg(this->ike_sa, FALSE);
	/* auth identity to comply */
	id = this->ike_sa->get_other_id(this->ike_sa);
	auth->add(auth, AUTH_RULE_IDENTITY, id->clone(id));
	if (skipped_auth(this, method, FALSE))
	{
		return TRUE;
	}
	peer_cfg = this->ike_sa->get_peer_cfg(this->ike_sa);
	cfg = get_auth_cfg(peer_cfg, FALSE);
	return cfg && auth->complies(auth, cfg, TRUE);
}

/**
 * Save authentication information after authentication succeeded
 */
static void save_auth_cfg(private_phase1_t *this,
						  auth_method_t method, bool local)
{
	auth_cfg_t *auth;

	if (skipped_auth(this, method, local))
	{
		return;
	}
	auth = auth_cfg_create();
	/* for local config, we _copy_ entires from the config, as it contains
	 * certificates we must send later. */
	auth->merge(auth, this->ike_sa->get_auth_cfg(this->ike_sa, local), local);
	this->ike_sa->add_auth_cfg(this->ike_sa, local, auth);
}

/**
 * Create an authenticator instance
 */
static authenticator_t* create_authenticator(private_phase1_t *this,
											 auth_method_t method, chunk_t id)
{
	authenticator_t *authenticator;

	authenticator = authenticator_create_v1(this->ike_sa, this->initiator,
						method, this->dh, this->dh_value, this->sa_payload, id);
	if (!authenticator)
	{
		DBG1(DBG_IKE, "negotiated authentication method %N not supported",
			 auth_method_names, method);
	}
	return authenticator;
}

METHOD(phase1_t, verify_auth, bool,
	private_phase1_t *this, auth_method_t method, message_t *message,
	chunk_t id_data)
{
	authenticator_t *authenticator;
	status_t status;

	authenticator = create_authenticator(this, method, id_data);
	if (authenticator)
	{
		status = authenticator->process(authenticator, message);
		authenticator->destroy(authenticator);
		if (status == SUCCESS && check_constraints(this, method))
		{
			save_auth_cfg(this, method, FALSE);
			return TRUE;
		}
	}
	return FALSE;
}

METHOD(phase1_t, build_auth, bool,
	private_phase1_t *this, auth_method_t method, message_t *message,
	chunk_t id_data)
{
	authenticator_t *authenticator;
	status_t status;

	authenticator = create_authenticator(this, method, id_data);
	if (authenticator)
	{
		status = authenticator->build(authenticator, message);
		authenticator->destroy(authenticator);
		if (status == SUCCESS)
		{
			save_auth_cfg(this, method, TRUE);
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * Get the two auth classes from local or remote config
 */
static void get_auth_class(peer_cfg_t *peer_cfg, bool local,
						   auth_class_t *c1, auth_class_t *c2)
{
	enumerator_t *enumerator;
	auth_cfg_t *auth;

	*c1 = *c2 = AUTH_CLASS_ANY;

	enumerator = peer_cfg->create_auth_cfg_enumerator(peer_cfg, local);
	while (enumerator->enumerate(enumerator, &auth))
	{
		if (*c1 == AUTH_CLASS_ANY)
		{
			*c1 = (uintptr_t)auth->get(auth, AUTH_RULE_AUTH_CLASS);
		}
		else
		{
			*c2 = (uintptr_t)auth->get(auth, AUTH_RULE_AUTH_CLASS);
			break;
		}
	}
	enumerator->destroy(enumerator);
}

METHOD(phase1_t, get_auth_method, auth_method_t,
	private_phase1_t *this, peer_cfg_t *peer_cfg)
{
	auth_class_t i1, i2, r1, r2;

	get_auth_class(peer_cfg, this->initiator, &i1, &i2);
	get_auth_class(peer_cfg, !this->initiator, &r1, &r2);

	if (i1 == AUTH_CLASS_PUBKEY && r1 == AUTH_CLASS_PUBKEY)
	{
		if (i2 == AUTH_CLASS_ANY && r2 == AUTH_CLASS_ANY)
		{
			/* TODO-IKEv1: ECDSA? */
			return AUTH_RSA;
		}
		if (i2 == AUTH_CLASS_XAUTH)
		{
			return AUTH_XAUTH_INIT_RSA;
		}
		if (r2 == AUTH_CLASS_XAUTH)
		{
			return AUTH_XAUTH_RESP_RSA;
		}
	}
	if (i1 == AUTH_CLASS_PSK && r1 == AUTH_CLASS_PSK)
	{
		if (i2 == AUTH_CLASS_ANY && r2 == AUTH_CLASS_ANY)
		{
			return AUTH_PSK;
		}
		if (i2 == AUTH_CLASS_XAUTH)
		{
			return AUTH_XAUTH_INIT_PSK;
		}
		if (r2 == AUTH_CLASS_XAUTH)
		{
			return AUTH_XAUTH_RESP_PSK;
		}
	}
	if (i1 == AUTH_CLASS_XAUTH && r1 == AUTH_CLASS_PUBKEY &&
		i2 == AUTH_CLASS_ANY && r2 == AUTH_CLASS_ANY)
	{
		return AUTH_HYBRID_INIT_RSA;
	}
	return AUTH_NONE;
}

METHOD(phase1_t, select_config, peer_cfg_t*,
	private_phase1_t *this, auth_method_t method, bool aggressive,
	identification_t *id)
{
	enumerator_t *enumerator;
	peer_cfg_t *current, *found = NULL;
	host_t *me, *other;

	me = this->ike_sa->get_my_host(this->ike_sa);
	other = this->ike_sa->get_other_host(this->ike_sa);
	DBG1(DBG_CFG, "looking for %N peer configs matching %H...%H[%Y]",
		 auth_method_names, method, me, other, id);
	enumerator = charon->backends->create_peer_cfg_enumerator(charon->backends,
													me, other, NULL, id, IKEV1);
	while (enumerator->enumerate(enumerator, &current))
	{
		if (get_auth_method(this, current) == method &&
			current->use_aggressive(current) == aggressive)
		{
			found = current->get_ref(current);
			break;
		}
	}
	enumerator->destroy(enumerator);

	if (found)
	{
		DBG2(DBG_CFG, "selected peer config \"%s\"", found->get_name(found));
	}
	return found;
}

METHOD(phase1_t, get_id, identification_t*,
	private_phase1_t *this, peer_cfg_t *peer_cfg, bool local)
{
	auth_cfg_t *auth;

	auth = get_auth_cfg(peer_cfg, local);
	if (auth)
	{
		return auth->get(auth, AUTH_RULE_IDENTITY);
	}
	return NULL;
}

METHOD(phase1_t, save_sa_payload, bool,
	private_phase1_t *this, message_t *message)
{
	enumerator_t *enumerator;
	payload_t *payload, *sa = NULL;
	chunk_t data;
	size_t offset = IKE_HEADER_LENGTH;

	enumerator = message->create_payload_enumerator(message);
	while (enumerator->enumerate(enumerator, &payload))
	{
		if (payload->get_type(payload) == SECURITY_ASSOCIATION_V1)
		{
			sa = payload;
			break;
		}
		else
		{
			offset += payload->get_length(payload);
		}
	}
	enumerator->destroy(enumerator);

	data = message->get_packet_data(message);
	if (sa && data.len >= offset + sa->get_length(sa))
	{
		/* Get SA payload without 4 byte fixed header */
		data = chunk_skip(data, offset);
		data.len = sa->get_length(sa);
		data = chunk_skip(data, 4);
		this->sa_payload = chunk_clone(data);
		return TRUE;
	}
	DBG1(DBG_IKE, "unable to extract SA payload encoding");
	return FALSE;
}

METHOD(phase1_t, add_nonce_ke, bool,
	private_phase1_t *this, message_t *message)
{
	nonce_payload_t *nonce_payload;
	ke_payload_t *ke_payload;
	chunk_t nonce;
	rng_t *rng;

	ke_payload = ke_payload_create_from_diffie_hellman(KEY_EXCHANGE_V1, this->dh);
	message->add_payload(message, &ke_payload->payload_interface);

	rng = lib->crypto->create_rng(lib->crypto, RNG_WEAK);
	if (!rng)
	{
		DBG1(DBG_IKE, "no RNG found to create nonce");
		return FALSE;
	}
	rng->allocate_bytes(rng, NONCE_SIZE, &nonce);
	rng->destroy(rng);

	nonce_payload = nonce_payload_create(NONCE_V1);
	nonce_payload->set_nonce(nonce_payload, nonce);
	message->add_payload(message, &nonce_payload->payload_interface);

	if (this->initiator)
	{
		this->nonce_i = nonce;
	}
	else
	{
		this->nonce_r = nonce;
	}
	return TRUE;
}

METHOD(phase1_t, get_nonce_ke, bool,
	private_phase1_t *this, message_t *message)
{
	nonce_payload_t *nonce_payload;
	ke_payload_t *ke_payload;

	ke_payload = (ke_payload_t*)message->get_payload(message, KEY_EXCHANGE_V1);
	if (!ke_payload)
	{
		DBG1(DBG_IKE, "KE payload missing in message");
		return FALSE;
	}
	this->dh_value = chunk_clone(ke_payload->get_key_exchange_data(ke_payload));
	this->dh->set_other_public_value(this->dh, this->dh_value);

	nonce_payload = (nonce_payload_t*)message->get_payload(message, NONCE_V1);
	if (!nonce_payload)
	{
		DBG1(DBG_IKE, "NONCE payload missing in message");
		return FALSE;
	}

	if (this->initiator)
	{
		this->nonce_r = nonce_payload->get_nonce(nonce_payload);
	}
	else
	{
		this->nonce_i = nonce_payload->get_nonce(nonce_payload);
	}
	return TRUE;
}

METHOD(phase1_t, destroy, void,
	private_phase1_t *this)
{
	chunk_free(&this->sa_payload);
	DESTROY_IF(this->dh);
	free(this->dh_value.ptr);
	free(this->nonce_i.ptr);
	free(this->nonce_r.ptr);
	free(this);
}

/**
 * See header
 */
phase1_t *phase1_create(ike_sa_t *ike_sa, bool initiator)
{
	private_phase1_t *this;

	INIT(this,
		.public = {
			.create_hasher = _create_hasher,
			.create_dh = _create_dh,
			.derive_keys = _derive_keys,
			.get_auth_method = _get_auth_method,
			.get_id = _get_id,
			.select_config = _select_config,
			.verify_auth = _verify_auth,
			.build_auth = _build_auth,
			.save_sa_payload = _save_sa_payload,
			.add_nonce_ke = _add_nonce_ke,
			.get_nonce_ke = _get_nonce_ke,
			.destroy = _destroy,
		},
		.ike_sa = ike_sa,
		.initiator = initiator,
		.keymat = (keymat_v1_t*)ike_sa->get_keymat(ike_sa),
	);

	return &this->public;
}
