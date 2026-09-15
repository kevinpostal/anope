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
struct BridgeMessage final {
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
  /* The remote id of the message this one replies to, if any. */
  Anope::string reply_to;
  /* The remote thread it was posted in, and that thread's display name,
   * when it was not posted in the bridged channel itself. */
  Anope::string thread;
  Anope::string thread_name;

  bool edit = false;
  bool del = false;
};

/** A message from IRC which is ready to relay to a bridged network. */
struct BridgeOutbound final {
  /* The nickname of the IRC user who sent it. */
  Anope::string nick;
  /* The message, with IRC formatting already removed. */
  Anope::string text;
  /* Whether the message was a CTCP ACTION. */
  bool action = false;
  /* The IRC msgid of the line, and the msgid of the line it replies to
   * (already unescaped), when the IRCd supplied them. */
  Anope::string msgid;
  Anope::string reply_to;
};

/** A reaction added to, or removed from, a message on a bridged network. */
struct BridgeReaction final {
  Anope::string protocol;
  Anope::string space;
  /* The bridged channel: the parent when the message is in a thread. */
  Anope::string channel;
  /* Who reacted. The display name is only known, and only needed, when a
   * reaction is added: a removal from someone without a client is
   * nothing to relay. */
  Anope::string user_id;
  Anope::string display;
  /* The remote id of the message reacted to. */
  Anope::string remote_id;
  /* The emoji: a unicode sequence as-is, or ":name:" for a custom one. */
  Anope::string emoji;
  bool add = true;
};

/** One member of a bridged space, as the roster sees them.
 *
 * A bridge introduces an IRC pseudo client for every member of the space it
 * is pointed at, so that the IRC channel shows the same population as the
 * remote one instead of only the people who have happened to speak.
 */
struct BridgeMember final {
  /* The remote user, and the name they should appear under. */
  Anope::string user_id;
  Anope::string display;
  /* Whether the remote network reports them as away, and the reason to
   * put on the IRC AWAY. Only meaningful when presence is known. */
  bool away = false;
  Anope::string away_reason;
  /* Whether the protocol actually knows their presence. A protocol which
   * cannot see presence leaves this false and the client is never marked
   * away rather than being marked permanently present. */
  bool presence_known = false;
};

/** The part of the service which a protocol is allowed to use.
 *
 * A protocol never touches users, channels, or servers: it hands messages to
 * the service and the service decides what happens on IRC.
 */
class BridgeCore {
public:
  virtual ~BridgeCore() = default;

  /** The module which owns the service, for logging. */
  virtual Module *GetOwner() = 0;

  /** Every configured bridge. */
  virtual const std::vector<Bridge *> &GetBridges() const = 0;

  /** Finds a bridge by the remote channel it is pointed at. */
  virtual Bridge *FindRemote(const Anope::string &protocol,
                             const Anope::string &channel) const = 0;

  /** Finds a bridge by the IRC channel it relays into. */
  virtual Bridge *FindIrc(const Anope::string &irc_channel) const = 0;

  /** Relays a message from a bridged network into its IRC channel. */
  virtual void RelayToIrc(const BridgeMessage &msg) = 0;

  /** Relays a reaction from a bridged network into its IRC channel, as a
   * TAGMSG from the reacting member's pseudo client. */
  virtual void RelayReaction(const BridgeReaction &reaction) = 0;

  /** Relays a typing notification from a bridged network into its IRC
   * channel, from the member's pseudo client. A member without a client
   * is not introduced for one. */
  virtual void RelayTyping(const Anope::string &protocol,
                           const Anope::string &space,
                           const Anope::string &channel,
                           const Anope::string &user_id) = 0;

  /** Records that an IRC message was delivered to the remote network, so
   * that later replies and reactions can be mapped in both directions. */
  virtual void RememberLink(const BridgeServ::Relay::Links::Entry &entry) = 0;

  /** The IRC msgid to use when IRC needs to refer to a remote message. */
  virtual Anope::string IrcIdFor(const Anope::string &remote_id) const = 0;

  /** The remote id an IRC msgid refers to, or "" when it is not known. */
  virtual Anope::string RemoteIdFor(const Anope::string &irc_msgid) const = 0;

  /** The remote id of the bridged client which holds an IRC nickname, or
   * "" when the nickname is not one of `bridge`'s own clients.
   *
   * Scoped to one bridge deliberately: a nickname held by another space's
   * client names somebody who is not in this channel, and mentioning them
   * there would reach a stranger or nobody. Not const, because it resolves
   * through FindClient().
   */
  virtual Anope::string RemoteIdForNick(Bridge *bridge, const Anope::string &nick) = 0;

  /** Looks up what is known about a remote message for quoting it.
   * @param remote_id The remote message id.
   * @param author The display name of who sent it.
   * @param excerpt The start of its text.
   * @param thread The remote thread it lives in, or "" for the channel.
   * @return Whether the message is still remembered.
   */
  virtual bool QuotedMessage(const Anope::string &remote_id,
                             Anope::string &author, Anope::string &excerpt,
                             Anope::string &thread) const = 0;

  /** Introduces (or updates) the members of a bridged space and joins them
   * to every IRC channel bridged to it.
   *
   * Safe to call repeatedly and with partial rosters: a member who already
   * has a client keeps it, so a re-sync does not churn the IRC channel.
   */
  virtual void SyncRoster(const Anope::string &protocol,
                          const Anope::string &space,
                          const std::vector<BridgeMember> &members) = 0;

  /** Retires the client of a member who is no longer in the space. */
  virtual void RemoveMember(const Anope::string &protocol,
                            const Anope::string &space,
                            const Anope::string &user_id) = 0;

  /** Marks the client of a member away or back, from remote presence. */
  virtual void SetPresence(const Anope::string &protocol,
                           const Anope::string &space,
                           const Anope::string &user_id, bool away,
                           const Anope::string &reason) = 0;

  /** Delivers the result of an asynchronous listing to its requester.
   * @param requester The UID (or, on IRCds without UIDs, the nickname) of
   *                  the user who asked for the listing.
   * @param svc The service the listing was asked of.
   * @param channels Whether the listing is of channels rather than spaces.
   * @param failed Whether the listing could not be retrieved.
   * @param lines The listing.
   */
  virtual void DeliverListing(const Anope::string &requester,
                              const Anope::string &svc, bool channels,
                              bool failed,
                              const std::vector<Anope::string> &lines) = 0;

  /** Marks a bridge as needing to be written to the database. */
  virtual void SaveBridge(Bridge *bridge) = 0;
};

/** A network which IRC channels can be bridged to.
 *
 * One protocol exists per bridged network type; the Discord implementation
 * lives in discord.cpp and is the only one so far.
 */
class BridgeProtocol {
  Anope::string name;

protected:
  BridgeCore *core;

  BridgeProtocol(const Anope::string &n, BridgeCore *c) : name(n), core(c) {}

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

  /** Shows the remote channel of a bridge that someone on IRC is typing.
   * Networks with no such notion leave this alone. */
  virtual void Typing(Bridge *bridge) { (void)bridge; }

  /** Adds or removes a reaction on a remote message on behalf of IRC.
   * @param remote_id The remote message.
   * @param channel The remote channel or thread the message lives in.
   * @param emoji A unicode emoji as-is, or ":name:" for a custom one.
   */
  virtual void React(Bridge *bridge, const Anope::string &remote_id,
                     const Anope::string &channel, const Anope::string &emoji,
                     bool add) {
    (void)bridge;
    (void)remote_id;
    (void)channel;
    (void)emoji;
    (void)add;
  }

  /** Tells the remote channel of a bridge about something which happened
   * on the IRC side (a nick change, a topic, a kick). The text is literal
   * and comes from the service, not from a user. */
  virtual void Notice(Bridge *bridge, const Anope::string &text) {
    (void)bridge;
    (void)text;
  }

  /** Called when the set of bridges has changed in any way. */
  virtual void OnBridgesChanged() {}

  /** Re-sends the roster of every bridged space from whatever the
   * protocol has cached.
   *
   * The remote network and the IRC uplink come up independently, so the
   * roster which arrives with a remote connection can land before IRC is
   * ready for it. This is called once IRC is, and whenever the set of
   * bridges changes, so the population converges either way round.
   */
  virtual void RefreshRoster() {}

  /** Called when a bridge is about to stop using its remote channel, so
   * that any delivery endpoint set up for it can be torn down.
   */
  virtual void OnBridgeRemoved(Bridge *bridge) { (void)bridge; }

  /** Asks the network for the spaces the bridge account can see.
   * @param requester The identity to pass back to DeliverListing().
   * @param svc The service the listing was asked of.
   */
  virtual void ListSpaces(const Anope::string &requester,
                          const Anope::string &svc) = 0;

  /** Asks the network for the channels of one space. */
  virtual void ListChannels(const Anope::string &space,
                            const Anope::string &requester,
                            const Anope::string &svc) = 0;
};

/** A bridge between an IRC channel and a channel on a bridged network. */
class Bridge final : public Serializable {
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

  /* Runtime counters for the STATUS command; not written to the database
   * and reset when the module loads. */
  struct Stats final {
    /* Lines sent into the IRC channel, messages sent to the remote
     * network, and lines or reactions refused by the flood bucket. */
    unsigned long in_lines = 0;
    unsigned long out_messages = 0;
    unsigned long dropped = 0;
    unsigned long reactions_in = 0;
    unsigned long reactions_out = 0;
    unsigned long typing_in = 0;
    unsigned long events_out = 0;
    time_t last_in = 0;
    time_t last_out = 0;
  } stats;

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
  /* When the remote channel was last told that someone on IRC is typing;
   * the indicator there is for the bridge as a whole. */
  time_t last_typing_out = 0;

  Bridge() : Serializable("Bridge") {}

  /** The protocol of this bridge, or null if it is not loaded. */
  BridgeProtocol *GetProtocol() const;
};

/** Looks up a protocol by name. */
BridgeProtocol *FindBridgeProtocol(const Anope::string &name);

/** Creates the Discord protocol. Defined in discord.cpp. */
BridgeProtocol *CreateDiscordProtocol(BridgeCore *core);
