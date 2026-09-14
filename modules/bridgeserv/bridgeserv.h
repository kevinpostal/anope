// Anope IRC Services <https://www.anope.org/>
//
// Copyright (C) 2003-2026 Anope Contributors
//
// Anope is free software. You can use, modify, and/or distribute it under the
// terms of version 2 of the GNU General Public License. See docs/LICENSE.txt
// for the complete terms of this license and docs/AUTHORS.txt for a list of
// contributors.
//
// Based on the original code of Epona by Lara
// Based on the original code of Services by Andy Church
//
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include "module.h"
#include "modules/bridgeserv/relay.h"

#include <vector>

class Bridge;
class BridgeProtocol;

/** A message from a bridged network which is ready to relay to IRC.
 *
 * A protocol renders the message itself, because only the protocol knows how
 * its own formatting, mentions, and attachments look; the service only knows
 * how to get plain text onto a channel.
 */
struct BridgeMessage final
{
	/* The protocol which produced the message. */
	Anope::string protocol;
	/* The remote space (a Discord guild, for example) it was sent in. */
	Anope::string space;
	/* The remote channel it was sent in. */
	Anope::string channel;
	/* The remote user who sent it, and their display name. */
	Anope::string user_id;
	Anope::string display;
	/* The remote message id, for edit and delete tracking. */
	Anope::string msg_id;
	/* The message, rendered as IRC-ready text. */
	Anope::string text;

	bool edit = false;
	bool del = false;
};

/** A message from IRC which is ready to relay to a bridged network. */
struct BridgeOutbound final
{
	/* The nickname of the IRC user who sent it. */
	Anope::string nick;
	/* The message, with IRC formatting already removed. */
	Anope::string text;
	/* Whether the message was a CTCP ACTION. */
	bool action = false;
};

/** The part of the service which a protocol is allowed to use.
 *
 * A protocol never touches users, channels, or servers: it hands messages to
 * the service and the service decides what happens on IRC.
 */
class BridgeCore
{
public:
	virtual ~BridgeCore() = default;

	/** The module which owns the service, for logging. */
	virtual Module *GetOwner() = 0;

	/** Every configured bridge. */
	virtual const std::vector<Bridge *> &GetBridges() const = 0;

	/** Finds a bridge by the remote channel it is pointed at. */
	virtual Bridge *FindRemote(const Anope::string &protocol, const Anope::string &channel) const = 0;

	/** Finds a bridge by the IRC channel it relays into. */
	virtual Bridge *FindIrc(const Anope::string &irc_channel) const = 0;

	/** Relays a message from a bridged network into its IRC channel. */
	virtual void RelayToIrc(const BridgeMessage &msg) = 0;

	/** Delivers the result of an asynchronous listing to its requester.
	 * @param requester The UID (or, on IRCds without UIDs, the nickname) of
	 *                  the user who asked for the listing.
	 * @param svc The service the listing was asked of.
	 * @param channels Whether the listing is of channels rather than spaces.
	 * @param failed Whether the listing could not be retrieved.
	 * @param lines The listing.
	 */
	virtual void DeliverListing(const Anope::string &requester, const Anope::string &svc, bool channels, bool failed, const std::vector<Anope::string> &lines) = 0;

	/** Marks a bridge as needing to be written to the database. */
	virtual void SaveBridge(Bridge *bridge) = 0;
};

/** A network which IRC channels can be bridged to.
 *
 * One protocol exists per bridged network type; the Discord implementation
 * lives in discord.cpp and is the only one so far.
 */
class BridgeProtocol
{
	Anope::string name;

protected:
	BridgeCore *core;

	BridgeProtocol(const Anope::string &n, BridgeCore *c)
		: name(n)
		, core(c)
	{
	}

public:
	virtual ~BridgeProtocol() = default;

	/** The name of this protocol, as stored in a bridge record. */
	const Anope::string &GetName() const { return this->name; }

	/** The domain which this protocol's virtual links and clients live under.
	 *
	 * A bridge to a space is introduced as the virtual link
	 * "<space id>.<domain>" and its clients are given "<domain>" as their
	 * hostname.
	 */
	virtual const Anope::string &GetDomain() const = 0;

	/** Reads the configuration of this protocol. */
	virtual void Configure(Configuration::Block &block) = 0;

	/** Whether the network is currently reachable. */
	virtual bool IsConnected() const = 0;

	/** Whether a string is a well formed space or channel identifier. */
	virtual bool IsValidId(const Anope::string &id) const = 0;

	/** Relays a message from IRC to the remote channel of a bridge. */
	virtual void Relay(Bridge *bridge, const BridgeOutbound &out) = 0;

	/** Called when the set of bridges has changed in any way. */
	virtual void OnBridgesChanged() { }

	/** Called when a bridge is about to stop using its remote channel, so
	 * that any delivery endpoint set up for it can be torn down.
	 */
	virtual void OnBridgeRemoved(Bridge *bridge) { (void)bridge; }

	/** Asks the network for the spaces the bridge account can see.
	 * @param requester The identity to pass back to DeliverListing().
	 * @param svc The service the listing was asked of.
	 */
	virtual void ListSpaces(const Anope::string &requester, const Anope::string &svc) = 0;

	/** Asks the network for the channels of one space. */
	virtual void ListChannels(const Anope::string &space, const Anope::string &requester, const Anope::string &svc) = 0;
};

/** A bridge between an IRC channel and a channel on a bridged network. */
class Bridge final
	: public Serializable
{
public:
	/* The protocol which this bridge speaks, eg. "discord". */
	Anope::string protocol;
	/* The IRC channel which the remote channel is relayed into. */
	Anope::string irc_channel;
	/* The remote space and channel which are relayed into IRC. */
	Anope::string space;
	Anope::string foreign_channel;
	/* Appended to the nickname of every client of this bridge, so that the
	 * same remote user can be told apart per channel mapping. */
	Anope::string nick_suffix;
	/* The nicknames which this bridge has reserved on the network. They stay
	 * reserved until the bridge is deleted. */
	std::set<Anope::string> reserved;

	/* The protocol-specific endpoint which IRC messages are delivered to (a
	 * Discord webhook, for example) and whether one is being set up. */
	Anope::string endpoint_id;
	Anope::string endpoint_token;
	bool endpoint_pending = false;
	time_t endpoint_retry_at = 0;
	time_t endpoint_failed_at = 0;
	unsigned endpoint_failures = 0;

	/* Throttles relaying into the IRC channel. */
	BridgeServ::Relay::TokenBucket throttle;

	Bridge()
		: Serializable("Bridge")
	{
	}

	/** The protocol of this bridge, or null if it is not loaded. */
	BridgeProtocol *GetProtocol() const;
};

/** Looks up a protocol by name. */
BridgeProtocol *FindBridgeProtocol(const Anope::string &name);

/** Creates the Discord protocol. Defined in discord.cpp. */
BridgeProtocol *CreateDiscordProtocol(BridgeCore *core);
