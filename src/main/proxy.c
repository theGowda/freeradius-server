/*
 * proxy.c	Proxy stuff.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2000  The FreeRADIUS server project
 * Copyright 2000  Miquel van Smoorenburg <miquels@cistron.nl>
 * Copyright 2000  Chris Parker <cparker@starnetusa.com>
 */

static const char rcsid[] = "$Id$";

#include "autoconf.h"
#include "libradius.h"

#include <sys/socket.h>

#if HAVE_NETINET_IN_H
#	include <netinet/in.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>

#include "radiusd.h"


static uint32_t proxy_id = 1;

/*
 *	We received a response from a remote radius server.
 *	Find the original request, then return.
 *	Returns:   1 replication don't reply
 *	           0 proxy found
 *		  -1 error don't reply
 */
int proxy_receive(REQUEST *request)
{
	VALUE_PAIR *proxypair;
	VALUE_PAIR *replicatepair;
	VALUE_PAIR *realmpair;
	int replicating;
	REALM *realm;
	char *realmname;

	proxypair = pairfind(request->config_items, PW_PROXY_TO_REALM);
	replicatepair = pairfind(request->config_items, PW_REPLICATE_TO_REALM);
	if(proxypair) {
		realmpair = proxypair;
		replicating = 0;
	} else if(replicatepair) {
		realmpair = replicatepair;
		replicating = 1;
	} else {
		radlog(L_PROXY, "Proxy reply to packet with no Realm");
		return -1;
	}

	realmname = (char *) realmpair->strvalue;
	realm = realm_find(realmname);

	/*
	 *	Don't touch the reply VP's.  Assume that a module
	 *	takes care of that...
	 */

	return replicating?1:0;
}

/*
 *	Add a proxy-pair to the end of the request.
 */
static void proxy_addinfo(REQUEST *request)
{
	VALUE_PAIR *proxy_pair;

	proxy_pair = paircreate(PW_PROXY_STATE, PW_TYPE_STRING);
	if (proxy_pair == NULL) {
		radlog(L_ERR|L_CONS, "no memory");
		exit(1);
	}
	sprintf((char *)proxy_pair->strvalue, "%d", request->packet->id);
	proxy_pair->length = strlen((char *)proxy_pair->strvalue);

	pairadd(&request->proxy->vps, proxy_pair);
}

/*
 *	Relay the request to a remote server.
 *	Returns:  2 success (we replicate, caller replies normally)
 *		  1 success (we reply, caller returns without replying)
 *	          0 fail (caller falls through to normal processing)
 *		 -1 fail (we don't reply, caller returns without replying)
 */
int proxy_send(REQUEST *request)
{
	VALUE_PAIR *proxypair;
	VALUE_PAIR *replicatepair;
	VALUE_PAIR *realmpair;
	VALUE_PAIR *namepair;
	VALUE_PAIR *strippednamepair;
	VALUE_PAIR *delaypair;
	VALUE_PAIR *vp, *vps;
	REALM *realm;
	char *realmname;
	int replicating;

	/* 
	 *	The timestamp is used below to figure the
	 *	next_try. The request needs to "hang around" until
	 *	either the other server sends a reply or the retry
	 *	count has been exceeded.  Until then, it should not
	 *	be eligible for the time-based cleanup.  --Pac. */

	/* Look for proxy/replicate signs */
	/* FIXME - What to do if multiple Proxy-To/Replicate-To attrs are
	 * set...  Log an error? Actually replicate to multiple places? That
	 * would be cool. For now though, I'll just take the first one and
	 * ignore the rest. */
	proxypair = pairfind(request->config_items, PW_PROXY_TO_REALM);
	replicatepair = pairfind(request->config_items, PW_REPLICATE_TO_REALM);
	if (proxypair) {
		realmpair = proxypair;
		replicating = 0;
	} else if (replicatepair) {
		realmpair = replicatepair;
		replicating = 1;
	} else {
		/*
		 *	Neither proxy or replicate attributes are set,
		 *	so we can exit from the proxy code.
		 */
		return 0;
	}

	/*
	 *	Length == 0 means it exists, but there's no realm.
	 *	Don't proxy it.
	 */
	if (realmpair->length == 0) {
		return 0;
	}

	realmname = (char *)realmpair->strvalue;

	/*
	 *	Look for the realm, letting realm_find take care
	 *	of the "NULL" realm.
	 *
	 *	If there is no such realm, then exit.
	 *	Maybe we should log an error?
	 */
	realm = realm_find(realmname);
	if (realm == NULL) {
		return 0;
	}

	/*
	 *	Remember that we sent the request to a Realm.
	 */
	pairadd(&request->packet->vps,
		pairmake("Realm", realm->realm, T_OP_EQ));
	

	/*
	 *	Maybe they're proxying it to a LOCAL realm, in which
	 *	case do nothing.
	 */
	if ((realm->ipaddr == htonl(INADDR_LOOPBACK)) &&
			(realm->auth_port == auth_port) &&
			(realm->acct_port == acct_port)) {
		return 0;
	}
	
	/*
	 *	Copy the request, then look up
	 *	name and plain-text password in the copy.
	 *
	 *	Note that the User-Name attribute is the *original*
	 *	as sent over by the client.  The Stripped-User-Name
	 *	attribute is the one hacked through the 'hints' file.
	 */
	vps = paircopy(request->packet->vps);
	namepair = pairfind(vps, PW_USER_NAME);
	strippednamepair = pairfind(vps, PW_STRIPPED_USER_NAME);

	/*
	 *	If there's a Stripped-User-Name attribute in the
	 *	request, then use THAT as the User-Name for the
	 *	proxied request, instead of the original name.
	 *
	 *	This is done by making a copy of the Stripped-User-Name
	 *	attribute, turning it into a User-Name attribute,
	 *	deleting the Stripped-User-Name and User-Name attributes
	 *	from the vps list, and making the new User-Name
	 *	the head of the vps list.
	 */
	if (strippednamepair) {
		vp = paircreate(PW_USER_NAME, PW_TYPE_STRING);
		if (!vp) {
			radlog(L_ERR|L_CONS, "no memory");
			exit(1);
		}
		memcpy(vp->strvalue, strippednamepair->strvalue,
				sizeof(vp->strvalue));
		vp->length = strippednamepair->length;
		pairdelete(&vps, PW_USER_NAME);
		pairdelete(&vps, PW_STRIPPED_USER_NAME);
		vp->next = vps;
		namepair = vp;
		vps = vp;
	}

	/*
	 *	Now build a new RADIUS_PACKET and send it.
	 *
	 *	FIXME: it could be that the id wraps around too fast if
	 *	we have a lot of requests, it might be better to keep
	 *	a seperate ID value per remote server.
	 *
	 *	OTOH the remote radius server should be smart enough to
	 *	compare _both_ ID and vector. Right ?
	 */
	if ((request->proxy = rad_alloc(TRUE)) == NULL) {
		radlog(L_ERR|L_CONS, "no memory");
		exit(1);
	}

	/*
	 *	Proxied requests get sent out the proxy FD ONLY.
	 */
	request->proxy->sockfd = proxyfd;

	request->proxy->code = request->packet->code;
	request->proxy->dst_ipaddr = realm->ipaddr;
	if (request->packet->code == PW_AUTHENTICATION_REQUEST)
		request->proxy->dst_port = realm->auth_port;
	else
		request->proxy->dst_port = realm->acct_port;
	assert(request->proxy->vps == NULL);
	request->proxy->vps = vps;

	/*
	 *	Add the request to the list of outstanding requests.
	 *	Note that request->proxy->id is a 16 bits value,
	 *	while rad_send sends only the 8 least significant
	 *	bits of that same value.
	 */
	request->proxy->id = (proxy_id++) & 0xff;
	proxy_id &= 0xffff;

	/*
	 *	Add PROXY_STATE attribute.
	 */
	proxy_addinfo(request);

	/*
	 *	Encrypt the Password with the proxy server's secret.
	 */
	if ((vp = pairfind(vps, PW_PASSWORD)) != NULL) {

		rad_pwencode((char *)vp->strvalue,
				&(vp->length),
				(char *)realm->secret,
				(char *)request->proxy->vector);
 
	/*
	 *	If there is no PW_CHAP_CHALLENGE attribute but there
	 *	is a PW_CHAP_PASSWORD we need to add it since we can't
	 *	use the request authenticator anymore - we changed it.
	 */
	} else if (pairfind(vps, PW_CHAP_PASSWORD) &&
		pairfind(vps, PW_CHAP_CHALLENGE) == NULL) {
		vp = paircreate(PW_CHAP_CHALLENGE, PW_TYPE_STRING);
		if (!vp) {
			radlog(L_ERR|L_CONS, "no memory");
			exit(1);
		}
		vp->length = AUTH_VECTOR_LEN;
		memcpy(vp->strvalue, request->packet->vector, AUTH_VECTOR_LEN);
		pairadd(&vps, vp);
	}

	/*
	 *	Send the request.
	 */
	rad_send(request->proxy, (char *)realm->secret);
	memcpy(request->proxysecret, realm->secret, sizeof(request->proxysecret));
	request->proxy_is_replicate = replicating;
	request->proxy_try_count = proxy_retry_count - 1;
	request->proxy_next_try = request->timestamp + proxy_retry_delay;
	delaypair = pairfind(vps, PW_ACCT_DELAY_TIME);
	request->proxy->timestamp = request->timestamp - (delaypair ? delaypair->lvalue : 0);

	/*
	 *	Do NOT free proxy->vps, the pairs are needed for the
	 *	retries! --Pac.
	 */
	return replicating?2:1;
}
