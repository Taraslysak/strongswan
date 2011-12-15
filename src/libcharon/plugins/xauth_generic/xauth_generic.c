/*
 * Copyright (C) 2011 Tobias Brunner
 * Hochschule fuer Technik Rapperswil
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

#include "xauth_generic.h"

#include <daemon.h>
#include <library.h>

typedef struct private_xauth_generic_t private_xauth_generic_t;

/**
 * Private data of an xauth_generic_t object.
 */
struct private_xauth_generic_t {

	/**
	 * Public interface.
	 */
	xauth_generic_t public;

	/**
	 * ID of the server
	 */
	identification_t *server;

	/**
	 * ID of the peer
	 */
	identification_t *peer;

};

METHOD(xauth_method_t, initiate_peer, status_t,
	private_xauth_generic_t *this, cp_payload_t **out)
{
	/* peer never initiates */
	return FAILED;
}

METHOD(xauth_method_t, process_peer, status_t,
	private_xauth_generic_t *this, cp_payload_t *in, cp_payload_t **out)
{
	shared_key_t *shared;
	cp_payload_t *cp;
	chunk_t user, pass;

	shared = lib->credmgr->get_shared(lib->credmgr, SHARED_EAP, this->peer,
									  this->server);
	if (!shared)
	{
		DBG1(DBG_IKE, "no XAuth secret found for '%Y' - '%Y'", this->peer,
			 this->server);
		return FAILED;
	}

	user = this->peer->get_encoding(this->peer);
	pass = shared->get_key(shared);

	cp = cp_payload_create_type(CONFIGURATION_V1, CFG_REPLY);
	cp->add_attribute(cp, configuration_attribute_create_chunk(
				CONFIGURATION_ATTRIBUTE_V1, XAUTH_USER_NAME, user));
	cp->add_attribute(cp, configuration_attribute_create_chunk(
				CONFIGURATION_ATTRIBUTE_V1, XAUTH_USER_PASSWORD, pass));
	shared->destroy(shared);
	*out = cp;
	return NEED_MORE;
}

METHOD(xauth_method_t, initiate_server, status_t,
	private_xauth_generic_t *this, cp_payload_t **out)
{
	cp_payload_t *cp;

	cp = cp_payload_create_type(CONFIGURATION_V1, CFG_REQUEST);
	cp->add_attribute(cp, configuration_attribute_create_chunk(
				CONFIGURATION_ATTRIBUTE_V1, XAUTH_USER_NAME, chunk_empty));
	cp->add_attribute(cp, configuration_attribute_create_chunk(
				CONFIGURATION_ATTRIBUTE_V1, XAUTH_USER_PASSWORD, chunk_empty));
	*out = cp;
	return NEED_MORE;
}

METHOD(xauth_method_t, process_server, status_t,
	private_xauth_generic_t *this, cp_payload_t *in, cp_payload_t **out)
{
	configuration_attribute_t *attr;
	enumerator_t *enumerator;
	shared_key_t *shared;
	identification_t *id;
	chunk_t user = chunk_empty, pass = chunk_empty;
	status_t status = SUCCESS;

	enumerator = in->create_attribute_enumerator(in);
	while (enumerator->enumerate(enumerator, &attr))
	{
		switch (attr->get_type(attr))
		{
			case XAUTH_USER_NAME:
				user = attr->get_chunk(attr);
				break;
			case XAUTH_USER_PASSWORD:
				pass = attr->get_chunk(attr);
				break;
			default:
				break;
		}
	}
	enumerator->destroy(enumerator);

	if (!user.ptr || !pass.ptr)
	{
		DBG1(DBG_IKE, "peer did not respond to our XAuth request");
		return FAILED;
	}
	if (user.len)
	{
		id = identification_create_from_data(user);
		if (!id)
		{
			DBG1(DBG_IKE, "failed to parse provided XAuth username");
			return FAILED;
		}
		this->peer->destroy(this->peer);
		this->peer = id;
	}

	shared = lib->credmgr->get_shared(lib->credmgr, SHARED_EAP,
									  this->server, this->peer);
	if (!shared)
	{
		DBG1(DBG_IKE, "no XAuth secret found for '%Y' - '%Y'",
			 this->server, this->peer);
		status = FAILED;
	}
	else if (!chunk_equals(shared->get_key(shared), pass))
	{
		DBG1(DBG_IKE, "failed to authenticate '%Y' with XAuth", this->peer);
		status = FAILED;
	}
	DESTROY_IF(shared);
	return status;
}

METHOD(xauth_method_t, get_identity, identification_t*,
	private_xauth_generic_t *this)
{
	return this->peer;
}

METHOD(xauth_method_t, destroy, void,
	private_xauth_generic_t *this)
{
	this->server->destroy(this->server);
	this->peer->destroy(this->peer);
	free(this);
}

/*
 * Described in header.
 */
xauth_generic_t *xauth_generic_create_peer(identification_t *server,
										   identification_t *peer)
{
	private_xauth_generic_t *this;

	INIT(this,
		.public =  {
			.xauth_method = {
				.initiate = _initiate_peer,
				.process = _process_peer,
				.get_identity = _get_identity,
				.destroy = _destroy,
			},
		},
		.server = server->clone(server),
		.peer = peer->clone(peer),
	);

	return &this->public;
}

/*
 * Described in header.
 */
xauth_generic_t *xauth_generic_create_server(identification_t *server,
											 identification_t *peer)
{
	private_xauth_generic_t *this;

	INIT(this,
		.public = {
			.xauth_method = {
				.initiate = _initiate_server,
				.process = _process_server,
				.get_identity = _get_identity,
				.destroy = _destroy,
			},
		},
		.server = server->clone(server),
		.peer = peer->clone(peer),
	);

	return &this->public;
}
