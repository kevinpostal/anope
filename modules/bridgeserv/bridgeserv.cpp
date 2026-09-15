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

/* BridgeServ: the network-agnostic half of the bridge. It owns the bridge
 * records, the commands, and everything which happens on IRC: the virtual
 * links which bridged networks appear behind, the pseudo clients which stand
 * in for their users, the nicknames those clients reserve, and the relaying
 * into channels. Everything which knows about a particular network lives
 * behind the BridgeProtocol interface; see discord.cpp.
 */

#include "bridgeserv.h"
#include "convert.h"
#include "modules/bridgeserv/render.h"
#include "xline.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Relay = BridgeServ::Relay;
namespace Text = BridgeServ::Text;

/* The prefix of the msgid stamped on every line relayed from a remote
 * network; see Relay::RemoteMsgId. Discord is the only protocol, so this is
 * one constant rather than a property of the protocol. */
static constexpr const char *REMOTE_ID_PREFIX = "dc";

class ModuleBridgeServ;

/* The protocols which are available to bridge to. */
static std::map<Anope::string, BridgeProtocol *, ci::less> bridge_protocols;

BridgeProtocol *FindBridgeProtocol(const Anope::string &name) {
  const auto it = bridge_protocols.find(name);
  if (it != bridge_protocols.end())
    return it->second;
  return nullptr;
}

BridgeProtocol *Bridge::GetProtocol() const {
  return FindBridgeProtocol(this->protocol);
}

/** An IRC pseudo client which stands in for a user of a bridged network.
 *
 * One client exists per remote user, per virtual link, per nickname suffix:
 * the same user seen in two channels of one space with the same suffix is one
 * IRC client joined to both channels.
 */
class BridgeClient final {
public:
  Anope::string key;
  Anope::string user_id;
  /* The display name the client currently carries; a change renames the
   * client in place. The old nickname stays reserved for as long as the
   * bridge exists. */
  Anope::string display;
  User *user = nullptr;
  std::set<Anope::string> chans;
  time_t last_active = Anope::CurTime;
  /* Whether the client is currently marked away on IRC, so that an
   * unchanged presence update does not put AWAY on the wire again. */
  bool away = false;
};

class BridgeType final : public Serialize::Type {
  ModuleBridgeServ *module;

public:
  explicit BridgeType(ModuleBridgeServ *creator);

  void Serialize(Serializable *obj, Serialize::Data &data) const override;
  Serializable *Unserialize(Serializable *obj,
                            Serialize::Data &data) const override;
};

/** Quits pseudo clients which have been idle for too long. */
class BridgeReapTimer final : public Timer {
  ModuleBridgeServ *module;

public:
  explicit BridgeReapTimer(ModuleBridgeServ *creator);

  bool Tick() override;
};

class CommandBSAdd final : public Command {
  ModuleBridgeServ *module;

public:
  explicit CommandBSAdd(ModuleBridgeServ *creator);

  void Execute(CommandSource &source,
               const std::vector<Anope::string> &params) override;
  bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSSet final : public Command {
  ModuleBridgeServ *module;

public:
  explicit CommandBSSet(ModuleBridgeServ *creator);

  void Execute(CommandSource &source,
               const std::vector<Anope::string> &params) override;
  bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSDel final : public Command {
  ModuleBridgeServ *module;

public:
  explicit CommandBSDel(ModuleBridgeServ *creator);

  void Execute(CommandSource &source,
               const std::vector<Anope::string> &params) override;
  bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSList final : public Command {
  ModuleBridgeServ *module;

public:
  explicit CommandBSList(ModuleBridgeServ *creator);

  void Execute(CommandSource &source,
               const std::vector<Anope::string> &params) override;
  bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSGuilds final : public Command {
  ModuleBridgeServ *module;

public:
  explicit CommandBSGuilds(ModuleBridgeServ *creator);

  void Execute(CommandSource &source,
               const std::vector<Anope::string> &params) override;
  bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSChannels final : public Command {
  ModuleBridgeServ *module;

public:
  explicit CommandBSChannels(ModuleBridgeServ *creator);

  void Execute(CommandSource &source,
               const std::vector<Anope::string> &params) override;
  bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class ModuleBridgeServ final : public Module, public BridgeCore {
  BridgeType *btype = nullptr;
  std::vector<BridgeProtocol *> protocols;

  Anope::string client_name;
  Anope::string default_protocol;
  time_t user_idle = 3600;
  size_t max_lines = 8;
  unsigned flood_lines = 6;
  time_t flood_secs = 4;
  bool relay_edits = true;
  bool relay_deletes = false;

  std::vector<Bridge *> bridges;

  /* The virtual links which bridged spaces appear behind, keyed by
   * "<protocol>/<space id>". */
  std::map<Anope::string, Server *> links;

  /* The counter which pseudo client UIDs are allocated from. The UIDs of
   * these clients belong to their link, not to services, so they cannot
   * come from IRCDProto::UID_Retrieve. */
  unsigned long uid_counter = 0;

  /* The pseudo clients of bridged networks, keyed by BridgeClient::key. */
  std::map<Anope::string, BridgeClient *> clients;

  /* What was last relayed for a message, keyed by
   * "<protocol>/<channel id>/<message id>". */
  Relay::History relayed;

  /* The IRC messages which were posted to a remote network, in both
   * directions. */
  Relay::Links msg_links;

  CommandBSAdd cmd_add;
  CommandBSSet cmd_set;
  CommandBSDel cmd_del;
  CommandBSList cmd_list;
  CommandBSGuilds cmd_guilds;
  CommandBSChannels cmd_channels;
  BridgeReapTimer reaper;

  /* ------------------------------------------------------------------ */
  /* Virtual links                                                      */
  /* ------------------------------------------------------------------ */

  static Anope::string LinkKey(const BridgeProtocol *protocol,
                               const Anope::string &space) {
    return protocol->GetName() + "/" + space;
  }

  /** Introduces, or finds, the virtual link a bridged space appears behind.
   *
   * A bridge is presented to the network as a link to
   * "<space id>.<protocol domain>" which services itself is carrying, so
   * that the clients of a space are visibly grouped and can be addressed
   * with the usual server oriented commands. The link is created with the
   * services-created flag, which tells Anope not to expect a burst from it.
   */
  Server *EnsureLink(BridgeProtocol *protocol, const Anope::string &space) {
    const Anope::string key = LinkKey(protocol, space);
    const auto it = this->links.find(key);
    if (it != this->links.end())
      return it->second;

    if (!IRCD || !Servers::GetUplink() || !Servers::GetUplink()->IsSynced())
      return nullptr;

    const Anope::string name = space + "." + protocol->GetDomain();
    if (!IRCD->IsHostValid(name) || name.find('.') == Anope::string::npos) {
      Log(this) << "BridgeServ: " << name
                << " is not a usable link name; check the "
                << protocol->GetName() << " domain.";
      return nullptr;
    }

    Server *existing = Server::Find(name, true);
    if (existing) {
      if (!existing->IsJuped()) {
        Log(this)
            << "BridgeServ: " << name
            << " is already a real server on the network; not bridging it.";
        return nullptr;
      }
      this->links[key] = existing;
      return existing;
    }

    const Anope::string desc = Anope::Format(
        "IRC bridge link (%s %s)", protocol->GetName().c_str(), space.c_str());
    auto *link = new Server(Me, name, desc, IRCD->SID_Retrieve(), 1, true);
    IRCD->SendServer(link);

    this->links[key] = link;
    Log(this) << "BridgeServ: introduced bridge link " << name;
    return link;
  }

  /** Allocates a UID for a pseudo client of a bridge link.
   *
   * The IRCd attributes a user to the server whose SID prefixes its UID, so
   * a client introduced with a UID from IRCDProto::UID_Retrieve, which is
   * always prefixed with the SID of services itself, would appear on
   * services instead of behind the link of its space.
   */
  Anope::string MakeUID(Server *link) {
    if (!IRCD || !IRCD->RequiresID)
      return "";

    const Anope::string &sid = link->GetSID();
    if (sid.empty())
      return IRCD->UID_Retrieve();

    /* Six letters, which is what every UID capable IRCd accepts, taken
     * from a counter and checked so that a reused counter after a rehash
     * cannot collide with a client which is still online. */
    for (unsigned tries = 0; tries < 1000; ++tries) {
      unsigned long n = this->uid_counter++;
      char suffix[7];
      for (int i = 5; i >= 0; --i) {
        suffix[i] = static_cast<char>('A' + (n % 26));
        n /= 26;
      }
      suffix[6] = '\0';

      const Anope::string uid = sid + suffix;
      if (!User::Find(uid))
        return uid;
    }

    return "";
  }

  /** Whether a user is one of our pseudo clients. */
  bool IsBridgeClient(const User *u) const {
    if (!u || !u->server)
      return false;

    for (const auto &[_, link] : this->links) {
      if (link == u->server)
        return true;
    }
    return false;
  }

  /* ------------------------------------------------------------------ */
  /* Reserved nicknames                                                 */
  /* ------------------------------------------------------------------ */

  /** Reserves a nickname on the network for as long as a bridge exists.
   *
   * The nickname of a bridged user is held with a network ban on the nick
   * (a Q-line on InspIRCd) so that nobody on IRC can take the identity of
   * somebody on the bridged network, even while that user is not currently
   * present. This is the same mechanism services use for their own nicks,
   * and it is sent to the IRCd rather than managed here so that it never
   * applies to our own clients.
   */
  void SendReservation(const Anope::string &nick, bool add) const {
    if (!IRCD || !IRCD->CanSQLine || !Me || !Me->IsSynced())
      return;

    if (add) {
      XLine x(nick, "Reserved for a bridged network user");
      IRCD->SendSQLine(nullptr, &x);
    } else {
      XLine x(nick);
      IRCD->SendSQLineDel(&x);
    }
  }

  void ReserveNick(Bridge *bridge, const Anope::string &nick) {
    if (bridge->reserved.insert(nick).second)
      this->SaveBridge(bridge);

    this->SendReservation(nick, true);
  }

  /** Whether a bridge other than the given one reserves a nickname. */
  bool HeldElsewhere(const Bridge *bridge, const Anope::string &nick) const {
    for (const auto *other : this->bridges) {
      if (other != bridge && other->reserved.count(nick))
        return true;
    }
    return false;
  }

  /** Releases the nicknames a bridge reserved, unless another bridge still
   * reserves them.
   * @return The number of nicknames actually released.
   */
  size_t ReleaseNicks(const Bridge *bridge) {
    size_t released = 0;
    for (const auto &nick : bridge->reserved) {
      if (this->HeldElsewhere(bridge, nick))
        continue;

      this->SendReservation(nick, false);
      ++released;
    }
    return released;
  }

  /* ------------------------------------------------------------------ */
  /* Pseudo clients                                                     */
  /* ------------------------------------------------------------------ */

  Anope::string MakeNick(const Anope::string &raw,
                         const Anope::string &suffix,
                         const User *ignore = nullptr) const {
    /* Leave room for the suffix and for the uniquifying counter. */
    const size_t maxlen = IRCD->MaxNick ? IRCD->MaxNick : 31;
    const size_t reserved = suffix.length() + 3;
    const size_t budget = maxlen > reserved ? maxlen - reserved : 1;

    const Anope::string base = Relay::SanitiseNick(raw.str(), budget);

    Anope::string nick = base + suffix;
    if (!IRCD->IsNickValid(nick))
      nick = "b_" + base + suffix;
    if (!IRCD->IsNickValid(nick))
      return "";

    const Anope::string candidate = nick;
    for (unsigned counter = 2;; ++counter) {
      const User *held = User::Find(nick, true);
      if ((!held || held == ignore) && IRCD->IsNickValid(nick))
        break;
      if (counter > 99)
        return "";

      nick = candidate + "_" + Anope::ToString(counter);
    }
    return nick;
  }

  Anope::string MakeIdent(const Anope::string &user_id) {
    /* The identity of a remote user is stable and unique, but not usable
     * as an ident, so a hash of it is used instead. */
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(user_id.c_str()),
           user_id.length(), digest);

    const Anope::string raw(reinterpret_cast<const char *>(digest),
                            SHA256_DIGEST_LENGTH);
    Anope::string hex = Anope::Hex(raw);

    /* networkinfo:userlen is 10 by default. */
    const size_t maxlen = IRCD->MaxUser ? IRCD->MaxUser : 10;
    if (hex.length() > maxlen)
      hex = hex.substr(0, maxlen);

    if (hex.empty() || !IRCD->IsIdentValid(hex)) {
      Log(this) << "BridgeServ: the derived ident for " << user_id
                << " is not valid on this IRCd; using \"bridge\".";
      return "bridge";
    }
    return hex;
  }

  BridgeClient *EnsureClient(Bridge *bridge, const Anope::string &user_id,
                             const Anope::string &display) {
    BridgeProtocol *protocol = bridge->GetProtocol();
    if (!protocol)
      return nullptr;

    const Anope::string key = LinkKey(protocol, bridge->space) + "/" +
                              bridge->nick_suffix + "/" + user_id;
    const auto it = this->clients.find(key);
    if (it != this->clients.end()) {
      BridgeClient *client = it->second;
      if (client->display == display)
        return client;

      /* The remote user renamed themselves. The client is renamed in place
       * rather than retired and re-introduced: quitting it made every
       * display-name change show up on IRC as a QUIT followed by a JOIN.
       * The old nickname stays reserved for as long as the bridge exists. */
      if (!client->user) {
        this->RemoveClient(client, "Display name changed",
                           Servers::GetUplink() &&
                               Servers::GetUplink()->IsSynced());
      } else {
        const Anope::string renamed =
            this->MakeNick(display, bridge->nick_suffix, client->user);
        if (renamed.empty()) {
          Log(this) << "BridgeServ: unable to allocate an IRC nick for the "
                    << "new display name of " << client->user->nick
                    << "; keeping the current nick.";
        } else if (!renamed.equals_ci(client->user->nick)) {
          this->ReserveNick(bridge, renamed);
          const Anope::string previous = client->user->nick;
          IRCD->SendNickChange(client->user, renamed);
          client->user->ChangeNick(renamed);
          Log(this) << "BridgeServ: renamed pseudo client " << previous
                    << " to " << renamed;
        }
        client->display = display;
        return client;
      }
    }

    Server *link = this->EnsureLink(protocol, bridge->space);
    if (!link)
      return nullptr;

    const Anope::string nick = this->MakeNick(display, bridge->nick_suffix);
    if (nick.empty()) {
      Log(this) << "BridgeServ: unable to allocate an IRC nick for "
                << protocol->GetName() << " user " << user_id
                << "; dropping message.";
      return nullptr;
    }

    /* The nickname is reserved before the client is introduced, exactly as
     * services reserve the nicks of their own clients. */
    this->ReserveNick(bridge, nick);

    const Anope::string realname =
        Anope::Format("%s (%s)", display.c_str(), protocol->GetName().c_str());
    User *user = User::OnIntroduce(
        nick, this->MakeIdent(user_id), protocol->GetDomain(), "", "", link,
        realname, Anope::CurTime, "", this->MakeUID(link), nullptr);
    if (!user) {
      /* The nick or UID collided with a real user; both sides have been
       * killed by the factory, so there is nobody to relay as. The
       * reservation is rolled back so the nick is not held for a
       * client which never existed. */
      Log(this) << "BridgeServ: collision introducing pseudo client " << nick
                << "; dropping message.";
      bridge->reserved.erase(nick);
      if (!this->HeldElsewhere(bridge, nick))
        this->SendReservation(nick, false);
      this->SaveBridge(bridge);
      return nullptr;
    }
    IRCD->SendClientIntroduction(user);

    auto *client = new BridgeClient();
    client->key = key;
    client->user_id = user_id;
    client->display = display;
    client->user = user;
    this->clients[key] = client;

    Log(this) << "BridgeServ: introduced pseudo client " << nick << "!"
              << user->GetIdent() << "@" << user->host << " on "
              << link->GetName();
    return client;
  }

  void EnsureJoin(BridgeClient *client, const Anope::string &channel_name) {
    if (!client->user)
      return;

    bool created = false;
    Channel *chan = Channel::FindOrCreate(channel_name, created);
    if (chan->FindUser(client->user))
      return;

    chan->JoinUser(client->user, nullptr);
    IRCD->SendJoin(client->user, chan, nullptr);
    client->chans.insert(chan->name);
  }

  /* ------------------------------------------------------------------ */
  /* Roster and presence                                                */
  /* ------------------------------------------------------------------ */

  /** Puts a pseudo client into, or back out of, the IRC away state.
   *
   * InspIRCd and every other IRCd Anope speaks take AWAY from the client
   * itself: "<ts> :<reason>" to go away, no parameters to come back.
   */
  void SetClientAway(BridgeClient *client, bool away,
                     const Anope::string &reason) {
    if (!client->user || client->away == away)
      return;

    client->away = away;
    if (!Servers::GetUplink() || !Servers::GetUplink()->IsSynced())
      return;

    if (away) {
      const Anope::string text = reason.empty() ? "Away" : reason;
      Uplink::Send(client->user, "AWAY", Anope::CurTime, text);
      client->user->SetAway(text, Anope::CurTime);
    } else {
      Uplink::Send(client->user, "AWAY");
      client->user->SetAway();
    }
  }

  /** The bridges of one space, in the order they were configured. */
  std::vector<Bridge *> BridgesOf(const Anope::string &protocol,
                                  const Anope::string &space) const {
    std::vector<Bridge *> found;
    for (auto *bridge : this->bridges) {
      if (bridge->protocol.equals_ci(protocol) &&
          bridge->space.equals_ci(space) && !bridge->irc_channel.empty())
        found.push_back(bridge);
    }
    return found;
  }

  /** The client a bridge holds for a remote user, or null. */
  BridgeClient *FindClient(Bridge *bridge, const Anope::string &user_id) {
    BridgeProtocol *protocol = bridge->GetProtocol();
    if (!protocol)
      return nullptr;

    const Anope::string key = LinkKey(protocol, bridge->space) + "/" +
                              bridge->nick_suffix + "/" + user_id;
    const auto it = this->clients.find(key);
    return it == this->clients.end() ? nullptr : it->second;
  }

  void SyncRoster(const Anope::string &protocol, const Anope::string &space,
                  const std::vector<BridgeMember> &members) override {
    if (!IRCD || members.empty())
      return;

    /* Introducing clients before the uplink has finished bursting would
     * put JOINs on a link which is not ready for them; the roster is
     * re-sent on every guild create, so dropping this one is safe. */
    if (!Servers::GetUplink() || !Servers::GetUplink()->IsSynced())
      return;

    size_t introduced = 0;
    for (auto *bridge : this->BridgesOf(protocol, space)) {
      for (const auto &member : members) {
        if (member.user_id.empty())
          continue;

        const bool existing = this->FindClient(bridge, member.user_id);
        BridgeClient *client =
            this->EnsureClient(bridge, member.user_id, member.display);
        if (!client || !client->user)
          continue;

        if (!existing)
          ++introduced;

        this->EnsureJoin(client, bridge->irc_channel);
        if (member.presence_known)
          this->SetClientAway(client, member.away, member.away_reason);
      }
    }

    if (introduced)
      Log(this) << "BridgeServ: joined " << introduced
                << " roster member(s) of " << protocol << " space " << space;
  }

  void RemoveMember(const Anope::string &protocol, const Anope::string &space,
                    const Anope::string &user_id) override {
    if (!IRCD || user_id.empty())
      return;

    const bool synced =
        Servers::GetUplink() && Servers::GetUplink()->IsSynced();
    for (auto *bridge : this->BridgesOf(protocol, space)) {
      BridgeClient *client = this->FindClient(bridge, user_id);
      if (client)
        this->RemoveClient(client, "Left the bridged space", synced);
    }
  }

  void SetPresence(const Anope::string &protocol, const Anope::string &space,
                   const Anope::string &user_id, bool away,
                   const Anope::string &reason) override {
    if (!IRCD || user_id.empty())
      return;

    for (auto *bridge : this->BridgesOf(protocol, space)) {
      BridgeClient *client = this->FindClient(bridge, user_id);
      if (client)
        this->SetClientAway(client, away, reason);
    }
  }

  void PartClient(BridgeClient *client, const Anope::string &channel_name,
                  const Anope::string &reason) {
    Channel *c = Channel::Find(channel_name);
    if (c && client->user && c->FindUser(client->user)) {
      IRCD->SendPart(client->user, c, reason);
      c->DeleteUser(client->user);
    }
    client->chans.erase(channel_name);
  }

  /* ------------------------------------------------------------------ */
  /* Relaying into IRC                                                  */
  /* ------------------------------------------------------------------ */

  void RelayDelete(Bridge *bridge, const std::string &key) {
    if (!this->relay_deletes)
      return;

    const auto *previous = this->relayed.Find(key);
    if (!previous)
      return;

    const Anope::string preview = previous->preview;
    this->relayed.Forget(key);

    auto *bi = this->GetClient();
    if (bi)
      IRCD->SendNotice(
          bi, bridge->irc_channel,
          Anope::Format(Language::Translate(_("message deleted (%s)")),
                        preview.c_str()));
  }

  /* ------------------------------------------------------------------ */
  /* Teardown                                                           */
  /* ------------------------------------------------------------------ */

  void RemoveClient(BridgeClient *client, const Anope::string &reason,
                    bool synced) {
    if (client->user) {
      if (synced)
        IRCD->SendQuit(client->user, reason);
      client->user->Quit(reason);
      /* OnUserQuit erases and deletes the record. */
      return;
    }
    this->clients.erase(client->key);
    delete client;
  }

  void RemoveAllClients(const Anope::string &reason) {
    const bool synced =
        IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced();

    /* User::Quit calls OnUserQuit synchronously, which erases the record
     * being quit, so the records to quit are snapshotted first. */
    std::vector<BridgeClient *> snapshot;
    snapshot.reserve(this->clients.size());
    for (const auto &[_, client] : this->clients)
      snapshot.push_back(client);

    for (auto *client : snapshot)
      this->RemoveClient(client, reason, synced);

    for (const auto &[_, client] : this->clients)
      delete client;
    this->clients.clear();

    /* Server::Delete fires OnServerQuit synchronously, which would
     * otherwise mutate the map while it is being walked. */
    std::vector<Server *> stale;
    stale.reserve(this->links.size());
    for (const auto &[_, link] : this->links)
      stale.push_back(link);
    this->links.clear();

    for (auto *link : stale) {
      if (synced)
        IRCD->SendSquit(link, reason);
      link->Delete(reason);
    }
  }

public:
  /* ------------------------------------------------------------------ */
  /* BridgeCore                                                         */
  /* ------------------------------------------------------------------ */

  Module *GetOwner() override { return this; }

  const std::vector<Bridge *> &GetBridges() const override {
    return this->bridges;
  }

  Bridge *FindRemote(const Anope::string &protocol,
                     const Anope::string &channel) const override {
    for (auto *bridge : this->bridges) {
      if (bridge->protocol.equals_ci(protocol) &&
          bridge->foreign_channel.equals_ci(channel))
        return bridge;
    }
    return nullptr;
  }

  Bridge *FindIrc(const Anope::string &irc_channel) const override {
    for (auto *bridge : this->bridges) {
      if (bridge->irc_channel.equals_ci(irc_channel))
        return bridge;
    }
    return nullptr;
  }

  void SaveBridge(Bridge *bridge) override { bridge->QueueUpdate(); }

  void RememberLink(const Relay::Links::Entry &entry) override {
    this->msg_links.Remember(entry);
  }

  Anope::string IrcIdFor(const Anope::string &remote_id) const override {
    if (const auto *link = this->msg_links.ByRemote(remote_id.str()))
      return link->irc_msgid;
    return Relay::RemoteMsgId(REMOTE_ID_PREFIX, remote_id.str());
  }

  Anope::string RemoteIdFor(const Anope::string &irc_msgid) const override {
    std::string remote_id;
    if (Relay::ParseRemoteMsgId(irc_msgid.str(), REMOTE_ID_PREFIX, remote_id))
      return remote_id;
    if (const auto *link = this->msg_links.ByIrc(irc_msgid.str()))
      return link->remote_id;
    return "";
  }

  bool QuotedMessage(const Anope::string &remote_id, Anope::string &author,
                     Anope::string &excerpt,
                     Anope::string &thread) const override {
    if (const auto *link = this->msg_links.ByRemote(remote_id.str())) {
      author = link->author;
      excerpt = link->excerpt;
      thread = link->remote_thread;
      return true;
    }

    /* A message which came from the remote network is keyed by the
     * channel it was relayed from, which the id alone does not say; there
     * is one bridge per remote channel, so try each. */
    for (const auto *bridge : this->bridges) {
      const std::string key = bridge->protocol.str() + "/" +
                              bridge->foreign_channel.str() + "/" +
                              remote_id.str();
      if (const auto *entry = this->relayed.Find(key)) {
        author = entry->author;
        excerpt = entry->preview;
        thread = entry->remote_thread;
        return true;
      }
    }
    return false;
  }

  void RelayToIrc(const BridgeMessage &msg) override {
    if (!IRCD)
      return;

    Bridge *bridge = this->FindRemote(msg.protocol, msg.channel);
    if (!bridge || bridge->irc_channel.empty())
      return;

    /* Only relay traffic from the space the bridge was pointed at. */
    if (!msg.space.empty() && msg.space != "0" &&
        !bridge->space.equals_ci(msg.space))
      return;

    const std::string key =
        msg.protocol.str() + "/" + msg.channel.str() + "/" + msg.msg_id.str();
    if (msg.del) {
      this->RelayDelete(bridge, key);
      return;
    }

    if (msg.text.empty())
      return;

    Relay::History::Entry record;
    record.hash = std::hash<std::string>{}(msg.text.str());
    record.preview = Text::TruncateCodePoints(msg.text.str(), 120);
    record.author = msg.display.str();
    record.remote_thread = msg.thread.str();

    if (msg.edit) {
      if (!this->relay_edits)
        return;

      const auto *previous = this->relayed.Find(key);
      if (!previous || previous->hash == record.hash)
        return; // never relayed, or nothing visible changed.
    }

    BridgeClient *client = this->EnsureClient(bridge, msg.user_id, msg.display);
    if (!client || !client->user)
      return;

    client->last_active = Anope::CurTime;
    this->EnsureJoin(client, bridge->irc_channel);

    User *u = client->user;
    const size_t source_len = std::max(
        u->GetUID().length(), u->nick.length() + u->GetIdent().length() +
                                  u->GetDisplayedHost().length() + 2);
    const size_t budget =
        Relay::PayloadBudget(source_len, bridge->irc_channel.length());

    /* Visible markers for clients without the reply tag: what the message
     * answers and the thread it came from, then the edit marker, so a
     * line reads "(edit) (reply to X) [thread] text". */
    std::string prefix;
    if (!msg.reply_to.empty()) {
      Anope::string author, excerpt, thread;
      if (this->QuotedMessage(msg.reply_to, author, excerpt, thread) &&
          !author.empty())
        prefix += "(reply to " + author.str() + ") ";
      else
        prefix += "(reply) ";
    }
    if (!msg.thread_name.empty())
      prefix += "[" + msg.thread_name.str() + "] ";
    if (msg.edit)
      prefix.insert(0, "(edit) ");

    std::string text = msg.text.str();
    if (!prefix.empty()) {
      const size_t at = text.find_first_not_of('\n');
      if (at != std::string::npos)
        text.insert(at, prefix);
    }
    const auto wire = Relay::SplitForWire(text, budget, this->max_lines);

    /* The message's identity goes on the first wire line only, which is
     * the multiline fallback rule: the IRCd allocates ids for the rest.
     * An edit is a new IRC line and must not reuse the id of the
     * original. No time= is stamped because InspIRCd's server-time is a
     * CapTag which replaces any incoming value with its own delivery
     * time; msgid is different, its module reuses a remote server's
     * value so that the id is the same on every side. */
    Anope::map<Anope::string> first_tags;
    if (!msg.edit)
      first_tags["msgid"] = Relay::RemoteMsgId(REMOTE_ID_PREFIX, msg.msg_id.str());
    if (!msg.reply_to.empty())
      first_tags["+draft/reply"] =
          Relay::EscapeTagValue(this->IrcIdFor(msg.reply_to).str());

    bool sent = false;
    for (const auto &line : wire.lines) {
      if (!Relay::Take(bridge->throttle, this->flood_lines, this->flood_secs,
                       Anope::CurTime)) {
        ++bridge->throttle.dropped;
        continue;
      }

      /* The drop notice comes from the service, not the pseudo client:
       * it is about the bridge, not about the user who happened to
       * speak next. */
      if (bridge->throttle.dropped) {
        if (auto *bi = this->GetClient()) {
          IRCD->SendNotice(
              bi, bridge->irc_channel,
              Anope::Format(Language::Translate(
                                _("%u bridge lines dropped (rate limit)")),
                            bridge->throttle.dropped));
          bridge->throttle.dropped = 0;
        }
      }
      IRCD->SendPrivmsg(u, bridge->irc_channel, line,
                        sent ? Anope::map<Anope::string>{} : first_tags);
      sent = true;
    }

    /* Only a message which reached the channel is remembered, so that an
     * edit or delete of a message nobody saw stays silent. */
    if (!sent)
      return;

    this->relayed.Remember(key, record);
    if (wire.dropped)
      IRCD->SendNotice(
          u, bridge->irc_channel,
          Anope::Format(
              Language::Translate(_("... [message truncated, %zu more lines]")),
              wire.dropped));
  }

  void DeliverListing(const Anope::string &requester, const Anope::string &svc,
                      bool channels, bool failed,
                      const std::vector<Anope::string> &lines) override {
    /* The requester is a UID where the IRCd has them, which User::Find
     * resolves first; a nick change while the listing was in flight
     * therefore still finds the right user. */
    User *u = User::Find(requester);
    auto *bi = BotInfo::Find(svc, true);
    if (!u || !bi)
      return; // the requester or the service went away.

    if (failed) {
      u->SendMessage(bi, channels ? _("Failed to retrieve the channel list "
                                      "from the bridged network.")
                                  : _("Failed to retrieve the space list from "
                                      "the bridged network."));
      return;
    }

    if (lines.empty()) {
      u->SendMessage(
          bi, channels ? _("No text channels were found in that space.")
                       : _("The bridge account is not present in any spaces."));
      return;
    }

    const size_t shown = std::min<size_t>(lines.size(), 100);
    for (size_t idx = 0; idx < shown; ++idx)
      u->SendMessage(bi, lines[idx]);

    if (lines.size() > shown)
      u->SendMessage(bi,
                     Anope::Format(Language::Translate(u->Account(),
                                                       _("(... and %zu more)")),
                                   lines.size() - shown));
  }

  /* ------------------------------------------------------------------ */
  /* Service API used by the commands                                   */
  /* ------------------------------------------------------------------ */

  BotInfo *GetClient() const { return BotInfo::Find(this->client_name, true); }

  /** The protocol which new bridges are created with. */
  BridgeProtocol *DefaultProtocol() const {
    return FindBridgeProtocol(this->default_protocol);
  }

  const std::map<Anope::string, BridgeProtocol *, ci::less> &
  GetProtocols() const {
    return bridge_protocols;
  }

  /** Whether a nickname suffix produces nicknames this network accepts. */
  bool IsSuffixValid(const Anope::string &suffix) const {
    return suffix.empty() || (IRCD && IRCD->IsNickValid("bridge" + suffix));
  }

  void JoinChannel(Bridge *bridge) {
    auto *bi = this->GetClient();
    if (bi && !bridge->irc_channel.empty())
      bi->Join(bridge->irc_channel);
  }

  void AddBridge(Bridge *bridge) {
    this->bridges.push_back(bridge);
    this->BridgesChanged();

    if (IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced())
      this->JoinChannel(bridge);
  }

  /** Removes a bridge.
   * @return The number of reserved nicknames which were released.
   */
  size_t DropBridge(Bridge *bridge) {
    const auto it =
        std::find(this->bridges.begin(), this->bridges.end(), bridge);
    if (it != this->bridges.end())
      this->bridges.erase(it);

    if (auto *protocol = bridge->GetProtocol())
      protocol->OnBridgeRemoved(bridge);

    const size_t released = this->ReleaseNicks(bridge);
    delete bridge;

    this->BridgesChanged();
    this->PruneChannels();
    return released;
  }

  /** Tells every protocol that the set of bridges has changed. */
  void BridgesChanged() {
    for (auto *protocol : this->protocols)
      protocol->OnBridgesChanged();
  }

  /** Parts pseudo clients from channels which are no longer bridged, and
   * from channels whose bridge they no longer belong to.
   */
  void PruneChannels() {
    for (auto &[_, client] : this->clients) {
      std::vector<Anope::string> stale;
      for (const auto &chan : client->chans) {
        if (!this->FindIrc(chan))
          stale.push_back(chan);
      }

      for (const auto &chan : stale)
        this->PartClient(client, chan, "Bridge removed");
    }
  }

  /** Takes the pseudo clients of a bridge out of its channel, and off the
   * network if that was their only channel. They are named and grouped by
   * the space and suffix of the bridge, so after a change to either they
   * have to come back under the new key.
   */
  void RetireClients(const Bridge *bridge, const Anope::string &reason) {
    const bool synced =
        IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced();

    /* RemoveClient erases the record through OnUserQuit. */
    std::vector<BridgeClient *> snapshot;
    for (const auto &[_, client] : this->clients) {
      if (client->chans.count(bridge->irc_channel))
        snapshot.push_back(client);
    }

    for (auto *client : snapshot) {
      this->PartClient(client, bridge->irc_channel, reason);
      if (client->chans.empty())
        this->RemoveClient(client, reason, synced);
    }
  }

  size_t CountBridgedUsers(const Anope::string &channel) const {
    size_t count = 0;
    for (const auto &[_, client] : this->clients) {
      if (client->chans.count(channel))
        ++count;
    }
    return count;
  }

  /** Quits pseudo clients which have not spoken for useridle seconds. The
   * nicknames they used stay reserved.
   */
  void ReapIdleUsers() {
    if (!this->user_idle)
      return;

    const bool synced =
        IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced();
    for (auto it = this->clients.begin(); it != this->clients.end();) {
      BridgeClient *client = it->second;
      if (!client->user ||
          Anope::CurTime - client->last_active <= this->user_idle) {
        ++it;
        continue;
      }

      /* The record is erased by OnUserQuit when the deferred quit is
       * processed, so it must not be erased here. */
      ++it;
      if (synced)
        IRCD->SendQuit(client->user, "Idle");
      client->user->Quit("Idle");
    }
  }

  /* ------------------------------------------------------------------ */
  /* Module                                                             */
  /* ------------------------------------------------------------------ */

  ModuleBridgeServ(const Anope::string &modname, const Anope::string &creator)
      : Module(modname, creator, VENDOR), cmd_add(this), cmd_set(this),
        cmd_del(this), cmd_list(this), cmd_guilds(this), cmd_channels(this),
        reaper(this) {
    this->SetAuthor("Anope");
    this->SetVersion("1.1");

    this->btype = new BridgeType(this);

    auto *discord = CreateDiscordProtocol(this);
    this->protocols.push_back(discord);
    bridge_protocols[discord->GetName()] = discord;

    Log(this) << "BridgeServ: Loaded with the " << discord->GetName()
              << " bridge protocol.";
  }

  ~ModuleBridgeServ() override {
    /* Protocols go first: they stop their network threads, after which
     * nothing else can arrive. */
    for (auto *protocol : this->protocols) {
      bridge_protocols.erase(protocol->GetName());
      delete protocol;
    }
    this->protocols.clear();

    this->RemoveAllClients("Bridge unloading");

    for (auto *bridge : this->bridges)
      delete bridge;
    this->bridges.clear();

    delete this->btype;
    this->btype = nullptr;
  }

  /** Reads a numeric setting, clamping it to its documented range. */
  template <typename T>
  T GetClamped(Configuration::Block &block, const Anope::string &key,
               const Anope::string &def, T lo, T hi) {
    const T given = block.Get<T>(key, def);
    const T used = std::clamp(given, lo, hi);
    if (used != given)
      Log(this) << "BridgeServ: " << key << " " << given
                << " is out of range; using " << used << ".";
    return used;
  }

  void OnReload(Configuration::Conf &conf) override {
    auto &block = conf.GetModule(this);

    const Anope::string old_client = this->client_name;
    this->client_name = block.Get<const Anope::string>("client", "BridgeServ");
    if (!this->GetClient())
      Log(this) << "BridgeServ: no service client named " << this->client_name
                << " exists.";

    this->default_protocol =
        block.Get<const Anope::string>("protocol", "discord");
    this->max_lines = this->GetClamped<size_t>(block, "maxlines", "8", 1, 64);
    this->flood_lines =
        this->GetClamped<unsigned>(block, "floodlines", "6", 0, 100);
    this->flood_secs =
        this->GetClamped<time_t>(block, "floodsecs", "4s", 1, 3600);
    this->relay_edits = block.Get<bool>("relayedits", "yes");
    this->relay_deletes = block.Get<bool>("relaydeletes", "no");

    /* Zero disables reaping; anything else is at least a minute so the
     * reaper can not quit a client which just spoke. */
    const time_t idle = block.Get<time_t>("useridle", "1h");
    this->user_idle = idle <= 0 ? 0 : std::max<time_t>(idle, 60);
    if (this->user_idle != idle)
      Log(this) << "BridgeServ: useridle " << idle << " is out of range; using "
                << this->user_idle << ".";

    bool domain_changed = false;
    for (auto *protocol : this->protocols) {
      const Anope::string old_domain = protocol->GetDomain();
      protocol->Configure(block);

      if (!old_domain.empty() && old_domain != protocol->GetDomain())
        domain_changed = true;
    }

    /* The links and the clients behind them are named after the domain, so
     * a domain change has to take them down and let them come back. */
    if (domain_changed)
      this->RemoveAllClients("Bridge domain changed");

    /* A new service client has to be in the bridged channels; the old
     * one belongs to whichever module configured it and is left alone. */
    const bool synced =
        IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced();
    if (synced && !old_client.empty() &&
        !old_client.equals_ci(this->client_name)) {
      for (auto *bridge : this->bridges)
        this->JoinChannel(bridge);
    }

    this->BridgesChanged();
  }

  void OnUplinkSync(Server *s) override {
    /* Joining from OnServerConnect would write the join into the uplink
     * socket during link negotiation, which InspIRCd rejects; the end of
     * the burst is the first point at which a join is legal. */
    for (auto *bridge : this->bridges) {
      this->JoinChannel(bridge);

      /* The IRCd may have restarted, so the reservations are sent
       * again; they are only held for as long as the bridge exists. */
      for (const auto &nick : bridge->reserved)
        this->SendReservation(nick, true);
    }

    this->BridgesChanged();

    /* The bridged networks may already have handed over their rosters
     * while the uplink was still bursting, when nothing could be joined
     * yet; this is the first moment the population can be put on IRC. */
    for (auto *protocol : this->protocols)
      protocol->RefreshRoster();
  }

  void OnServerQuit(Server *server) override {
    /* The server quits its own users as it goes away; the records only
     * need to be forgotten, and before the users are, so that nothing
     * looks them up through a link which no longer exists. */
    for (auto it = this->clients.begin(); it != this->clients.end();) {
      BridgeClient *client = it->second;
      if (!client->user || client->user->server != server) {
        ++it;
        continue;
      }

      delete client;
      it = this->clients.erase(it);
    }

    for (auto it = this->links.begin(); it != this->links.end();) {
      if (it->second == server)
        it = this->links.erase(it);
      else
        ++it;
    }
  }

  void OnUserQuit(User *u, const Anope::string &msg) override {
    if (!u)
      return;

    for (auto it = this->clients.begin(); it != this->clients.end(); ++it) {
      if (it->second->user != u)
        continue;

      delete it->second;
      this->clients.erase(it);
      break;
    }
  }

  void OnPrivmsg(User *u, Channel *c, Anope::string &msg,
                 const Anope::map<Anope::string> &tags) override {
    if (!u || !c || msg.empty())
      return;

    /* Never relay services clients or our own pseudo clients; that would
     * loop messages back into the channel they came from. */
    if (u == this->GetClient() || u->server == Me || this->IsBridgeClient(u))
      return;

    Bridge *bridge = this->FindIrc(c->name);
    if (!bridge)
      return;

    BridgeProtocol *protocol = bridge->GetProtocol();
    if (!protocol || !protocol->IsConnected())
      return;

    Anope::string ctcp_name;
    Anope::string ctcp_body;
    BridgeOutbound out;
    out.nick = u->nick;
    if (const auto it = tags.find("msgid"); it != tags.end())
      out.msgid = it->second;
    if (const auto it = tags.find("+draft/reply"); it != tags.end())
      out.reply_to = Relay::UnescapeTagValue(it->second.str());

    Anope::string payload = msg;
    if (Anope::ParseCTCP(msg, ctcp_name, ctcp_body)) {
      /* An ACTION has a sensible rendering on other networks; no other
       * CTCP does, so those are dropped. */
      if (!ctcp_name.equals_ci("ACTION") || ctcp_body.empty())
        return;

      out.action = true;
      payload = ctcp_body;
    }

    out.text = Anope::RemoveFormatting(payload);
    if (out.text.empty())
      return;

    protocol->Relay(bridge, out);
  }
};

BridgeType::BridgeType(ModuleBridgeServ *creator)
    /* The type is deliberately unowned: an owned type is written to a separate
     * per-module database which Anope only reads when the type is created
     * after the databases have been loaded, so owned rows would never come
     * back after a restart. */
    : Serialize::Type("Bridge"), module(creator) {}

void BridgeType::Serialize(Serializable *obj, Serialize::Data &data) const {
  const auto *bridge = static_cast<const Bridge *>(obj);

  data.Store("protocol", bridge->protocol);
  data.Store("irc-channel", bridge->irc_channel);
  data.Store("space", bridge->space);
  data.Store("foreign-channel", bridge->foreign_channel);
  data.Store("nick-suffix", bridge->nick_suffix);
  data.Store("endpoint-id", bridge->endpoint_id);
  data.Store("endpoint-token", bridge->endpoint_token);

  Anope::string reserved;
  for (const auto &nick : bridge->reserved)
    reserved += (reserved.empty() ? "" : " ") + nick;
  data.Store("reserved", reserved);
}

Serializable *BridgeType::Unserialize(Serializable *obj,
                                      Serialize::Data &data) const {
  if (!this->module)
    return nullptr;

  /* Rows written before the fields were named for any protocol used the
   * Discord names; they are read once and written back under the new
   * names on the next save. */
  const auto load = [&data](const char *key, const char *legacy) {
    Anope::string value = data.Load(key);
    if (value.empty())
      value = data.Load(legacy);
    return value;
  };

  const Anope::string irc_channel = data.Load("irc-channel");
  const Anope::string space = load("space", "guild");
  const Anope::string foreign_channel = data.Load("foreign-channel");

  /* Rows which can not make a working bridge are skipped; the database
   * loader treats a null return as "ignore this record". */
  if (irc_channel.empty() || space.empty() || foreign_channel.empty())
    return nullptr;

  Bridge *bridge;
  if (obj)
    bridge = anope_dynamic_static_cast<Bridge *>(obj);
  else {
    if (this->module->FindIrc(irc_channel))
      return nullptr; // a duplicate of a bridge which is already loaded.

    bridge = new Bridge();
  }

  /* Records from before bridges could name their protocol are Discord. */
  bridge->protocol = data.Load("protocol");
  if (bridge->protocol.empty())
    bridge->protocol = "discord";
  if (!FindBridgeProtocol(bridge->protocol))
    Log(this->module)
        << "BridgeServ: the bridge for " << irc_channel
        << " uses the unknown protocol " << bridge->protocol
        << "; it will not relay until that protocol is available.";

  bridge->irc_channel = irc_channel;
  bridge->space = space;
  bridge->foreign_channel = foreign_channel;
  bridge->nick_suffix = data.Load("nick-suffix");
  bridge->endpoint_id = load("endpoint-id", "webhook-id");
  bridge->endpoint_token = load("endpoint-token", "webhook-token");

  bridge->reserved.clear();
  size_t invalid = 0;
  spacesepstream reserved(data.Load("reserved"));
  for (Anope::string nick; reserved.GetToken(nick);) {
    if (nick.empty() || !IRCD || !IRCD->IsNickValid(nick))
      ++invalid;
    else
      bridge->reserved.insert(nick);
  }
  if (invalid)
    Log(this->module) << "BridgeServ: dropped " << invalid
                      << " invalid reserved nickname(s) from the record for "
                      << irc_channel;

  if (obj)
    this->module->BridgesChanged();
  else
    this->module->AddBridge(bridge);

  return bridge;
}

BridgeReapTimer::BridgeReapTimer(ModuleBridgeServ *creator)
    : Timer(creator, 60), module(creator) {}

bool BridgeReapTimer::Tick() {
  this->module->ReapIdleUsers();
  return true;
}

/* -------------------------------------------------------------------------- */
/* Commands */
/* -------------------------------------------------------------------------- */

/** Checks the granular permission of a command, with an oper fallback. */
static bool CheckAccess(CommandSource &source,
                        const Anope::string &permission) {
  if (source.HasPriv(permission) || source.IsServicesOper())
    return true;

  source.Reply(_("Access denied."));
  return false;
}

/** The identity an asynchronous listing is delivered back to: the UID where
 * the IRCd has them, so that a nick change in the meantime does not lose the
 * reply or deliver it to whoever took the nick.
 */
static Anope::string Requester(CommandSource &source) {
  const User *u = source.GetUser();
  if (u && !u->GetUID().empty())
    return u->GetUID();
  return source.GetNick();
}

/** Validates the space, channel, and suffix of a bridge. */
static BridgeProtocol *CheckBridgeArgs(CommandSource &source,
                                       ModuleBridgeServ *module,
                                       const Anope::string &space,
                                       const Anope::string &channel,
                                       const Anope::string &suffix) {
  BridgeProtocol *protocol = module->DefaultProtocol();
  if (!protocol) {
    source.Reply(_("No bridge protocol is available."));
    return nullptr;
  }

  if (!protocol->IsValidId(space)) {
    source.Reply(_("Invalid %s space ID."), protocol->GetName().c_str());
    return nullptr;
  }

  if (!protocol->IsValidId(channel)) {
    source.Reply(_("Invalid %s channel ID."), protocol->GetName().c_str());
    return nullptr;
  }

  if (!module->IsSuffixValid(suffix)) {
    source.Reply(
        _("\002%s\002 can not be used as a nickname suffix on this network. "
          "Characters outside the usual nickname alphabet have to be allowed "
          "with "
          "\002networkinfo:nick_chars\002 first."),
        suffix.c_str());
    return nullptr;
  }
  return protocol;
}

CommandBSAdd::CommandBSAdd(ModuleBridgeServ *creator)
    : Command(creator, "bridgeserv/add", 3, 4), module(creator) {
  this->SetDesc(_("Bridge an IRC channel to a channel on a bridged network"));
  this->SetSyntax(_("\037#channel\037 \037space-id\037 \037channel-id\037 "
                    "[\037nick-suffix\037]"));
}

void CommandBSAdd::Execute(CommandSource &source,
                           const std::vector<Anope::string> &params) {
  if (!CheckAccess(source, "bridgeserv/add"))
    return;

  const auto &irc_channel = params[0];
  const auto &space = params[1];
  const auto &foreign_channel = params[2];
  const Anope::string suffix = params.size() > 3 ? params[3] : "";

  if (!IRCD || !IRCD->IsChannelValid(irc_channel)) {
    source.Reply(_("Invalid IRC channel name."));
    return;
  }

  auto *protocol =
      CheckBridgeArgs(source, this->module, space, foreign_channel, suffix);
  if (!protocol)
    return;

  if (this->module->FindIrc(irc_channel)) {
    source.Reply(_("\002%s\002 is already bridged."), irc_channel.c_str());
    return;
  }

  if (const auto *other =
          this->module->FindRemote(protocol->GetName(), foreign_channel)) {
    source.Reply(_("That channel is already bridged to \002%s\002."),
                 other->irc_channel.c_str());
    return;
  }

  auto *bridge = new Bridge();
  bridge->protocol = protocol->GetName();
  bridge->irc_channel = irc_channel;
  bridge->space = space;
  bridge->foreign_channel = foreign_channel;
  bridge->nick_suffix = suffix;

  this->module->AddBridge(bridge);
  bridge->QueueUpdate();

  Log(LOG_ADMIN, source, this)
      << "to bridge " << irc_channel << " to " << protocol->GetName()
      << " space " << space << " channel " << foreign_channel
      << (suffix.empty() ? "" : " with nick suffix " + suffix);
  source.Reply(_("Added bridge %s <-> %s space %s channel %s."),
               irc_channel.c_str(), protocol->GetName().c_str(), space.c_str(),
               foreign_channel.c_str());
}

bool CommandBSAdd::OnHelp(CommandSource &source,
                          const Anope::string &subcommand) {
  this->SendSyntax(source);
  source.Reply(" ");
  source.Reply(
      _("Creates a bridge between an IRC channel and a channel on a "
        "bridged network. The space and channel are given by their "
        "numeric ids, as shown by the \002GUILDS\002 and \002CHANNELS\002 "
        "commands. The bridge service joins the IRC channel and starts "
        "relaying messages in both directions immediately."));
  source.Reply(" ");
  source.Reply(
      _("If a nickname suffix is given then it is appended to the nick of "
        "every user relayed into this channel, so that the same user can "
        "be told apart per channel mapping, for example "
        "\002iampaigeat/NC\002. Suffix characters outside the usual "
        "nickname alphabet have to be allowed with "
        "\002networkinfo:nick_chars\002 first."));
  return true;
}

CommandBSSet::CommandBSSet(ModuleBridgeServ *creator)
    : Command(creator, "bridgeserv/set", 3, 4), module(creator) {
  this->SetDesc(_("Re-point an existing bridge"));
  this->SetSyntax(_("\037#channel\037 \037space-id\037 \037channel-id\037 "
                    "[\037nick-suffix\037]"));
}

void CommandBSSet::Execute(CommandSource &source,
                           const std::vector<Anope::string> &params) {
  if (!CheckAccess(source, "bridgeserv/set"))
    return;

  const auto &irc_channel = params[0];
  const auto &space = params[1];
  const auto &foreign_channel = params[2];
  const Anope::string suffix = params.size() > 3 ? params[3] : "";

  Bridge *bridge = this->module->FindIrc(irc_channel);
  if (!bridge) {
    source.Reply(_("No such bridge."));
    return;
  }

  auto *protocol =
      CheckBridgeArgs(source, this->module, space, foreign_channel, suffix);
  if (!protocol)
    return;

  const auto *other =
      this->module->FindRemote(protocol->GetName(), foreign_channel);
  if (other && other != bridge) {
    source.Reply(_("That channel is already bridged to \002%s\002."),
                 other->irc_channel.c_str());
    return;
  }

  /* The old endpoint belongs to the old channel; it is of no use here. */
  if (!bridge->foreign_channel.equals_ci(foreign_channel)) {
    if (auto *old = bridge->GetProtocol())
      old->OnBridgeRemoved(bridge);
  }

  bridge->protocol = protocol->GetName();
  bridge->space = space;
  bridge->foreign_channel = foreign_channel;
  bridge->nick_suffix = suffix;
  bridge->QueueUpdate();

  /* The clients of this bridge were named and grouped by the old space and
   * suffix, so they are taken down and come back under the new ones. */
  this->module->RetireClients(bridge, "Bridge repointed");
  this->module->BridgesChanged();

  Log(LOG_ADMIN, source, this)
      << "to point " << irc_channel << " at " << protocol->GetName()
      << " space " << space << " channel " << foreign_channel
      << (suffix.empty() ? "" : " with nick suffix " + suffix);
  source.Reply(_("Updated bridge %s <-> %s space %s channel %s."),
               irc_channel.c_str(), protocol->GetName().c_str(), space.c_str(),
               foreign_channel.c_str());
}

bool CommandBSSet::OnHelp(CommandSource &source,
                          const Anope::string &subcommand) {
  this->SendSyntax(source);
  source.Reply(" ");
  source.Reply(
      _("Re-points an existing bridge at a different space and channel, "
        "and sets or clears its nickname suffix. Any delivery endpoint of "
        "the previous channel is deleted and a new one is created. "
        "Nicknames reserved under the previous settings stay reserved "
        "until the bridge is deleted."));
  return true;
}

CommandBSDel::CommandBSDel(ModuleBridgeServ *creator)
    : Command(creator, "bridgeserv/del", 1, 1), module(creator) {
  this->SetDesc(_("Remove a bridge"));
  this->SetSyntax(_("\037#channel\037"));
}

void CommandBSDel::Execute(CommandSource &source,
                           const std::vector<Anope::string> &params) {
  if (!CheckAccess(source, "bridgeserv/del"))
    return;

  Bridge *bridge = this->module->FindIrc(params[0]);
  if (!bridge) {
    source.Reply(_("No such bridge."));
    return;
  }

  const Anope::string irc_channel = bridge->irc_channel;
  const size_t reserved = bridge->reserved.size();
  const size_t released = this->module->DropBridge(bridge);

  Log(LOG_ADMIN, source, this) << "to remove the bridge for " << irc_channel;
  source.Reply(_("Bridge removed."));
  if (reserved > released)
    source.Reply(_("%zu reserved nickname(s) released, %zu still held by other "
                   "bridges."),
                 released, reserved - released);
  else if (released)
    source.Reply(_("%zu reserved nickname(s) released."), released);
}

bool CommandBSDel::OnHelp(CommandSource &source,
                          const Anope::string &subcommand) {
  this->SendSyntax(source);
  source.Reply(" ");
  source.Reply(
      _("Removes an existing bridge, deletes its delivery endpoint, and "
        "releases the nicknames it had reserved on the network. Pseudo "
        "clients are removed from the channel but remain on IRC for any "
        "other bridge they are active in."));
  return true;
}

CommandBSList::CommandBSList(ModuleBridgeServ *creator)
    : Command(creator, "bridgeserv/list", 0, 0), module(creator) {
  this->SetDesc(_("List the configured bridges"));
}

void CommandBSList::Execute(CommandSource &source,
                            const std::vector<Anope::string> &params) {
  if (!CheckAccess(source, "bridgeserv/list"))
    return;

  for (const auto &[name, protocol] : this->module->GetProtocols()) {
    source.Reply(protocol->IsConnected()
                     ? _("%s: online (clients appear on <space-id>.%s)")
                     : _("%s: offline (clients appear on <space-id>.%s)"),
                 name.c_str(), protocol->GetDomain().c_str());
  }

  const auto &bridges = this->module->GetBridges();
  if (bridges.empty()) {
    source.Reply(_("No bridges are configured."));
    return;
  }

  ListFormatter list(source.GetAccount());
  list.AddColumn(_("Channel"))
      .AddColumn(_("Network"))
      .AddColumn(_("Space"))
      .AddColumn(_("Remote channel"))
      .AddColumn(_("Suffix"))
      .AddColumn(_("Endpoint"))
      .AddColumn(_("Users"))
      .AddColumn(_("Nicks"));

  for (const auto *bridge : bridges) {
    ListFormatter::ListEntry entry;
    entry["Channel"] = bridge->irc_channel;
    entry["Network"] = bridge->protocol;
    entry["Space"] = bridge->space;
    entry["Remote channel"] = bridge->foreign_channel;
    entry["Suffix"] = bridge->nick_suffix.empty() ? "-" : bridge->nick_suffix;
    entry["Endpoint"] = bridge->endpoint_id.empty() ? _("no") : _("yes");
    entry["Users"] =
        Anope::ToString(this->module->CountBridgedUsers(bridge->irc_channel));
    entry["Nicks"] = Anope::ToString(bridge->reserved.size());
    list.AddEntry(entry);
  }

  list.SendTo(source);
}

bool CommandBSList::OnHelp(CommandSource &source,
                           const Anope::string &subcommand) {
  this->SendSyntax(source);
  source.Reply(" ");
  source.Reply(
      _("Lists the configured bridges: the network and channel each one is "
        "pointed at, its nickname suffix, whether it has a delivery "
        "endpoint for per-user identity, how many bridged users are "
        "currently in the IRC channel, and how many nicknames it has "
        "reserved on the network."));
  return true;
}

CommandBSGuilds::CommandBSGuilds(ModuleBridgeServ *creator)
    : Command(creator, "bridgeserv/guilds", 0, 0), module(creator) {
  this->SetDesc(_("List the spaces the bridge account is in"));
}

void CommandBSGuilds::Execute(CommandSource &source,
                              const std::vector<Anope::string> &params) {
  if (!CheckAccess(source, "bridgeserv/guilds"))
    return;

  auto *protocol = this->module->DefaultProtocol();
  if (!protocol) {
    source.Reply(_("No bridge protocol is available."));
    return;
  }

  if (!protocol->IsConnected()) {
    source.Reply(_("Not connected to %s."), protocol->GetName().c_str());
    return;
  }

  protocol->ListSpaces(Requester(source),
                       source.service ? source.service->nick : "");
}

bool CommandBSGuilds::OnHelp(CommandSource &source,
                             const Anope::string &subcommand) {
  this->SendSyntax(source);
  source.Reply(" ");
  source.Reply(
      _("Lists the spaces (Discord guilds) which the bridge account has "
        "been invited to, as \037name\037 (\037id\037). The ids are what "
        "the \002ADD\002 and \002SET\002 commands expect."));
  return true;
}

CommandBSChannels::CommandBSChannels(ModuleBridgeServ *creator)
    : Command(creator, "bridgeserv/channels", 1, 1), module(creator) {
  this->SetDesc(_("List the channels of a space"));
  this->SetSyntax(_("\037space-id\037"));
}

void CommandBSChannels::Execute(CommandSource &source,
                                const std::vector<Anope::string> &params) {
  if (!CheckAccess(source, "bridgeserv/channels"))
    return;

  auto *protocol = this->module->DefaultProtocol();
  if (!protocol) {
    source.Reply(_("No bridge protocol is available."));
    return;
  }

  if (!protocol->IsValidId(params[0])) {
    source.Reply(_("Invalid %s space ID."), protocol->GetName().c_str());
    return;
  }

  if (!protocol->IsConnected()) {
    source.Reply(_("Not connected to %s."), protocol->GetName().c_str());
    return;
  }

  protocol->ListChannels(params[0], Requester(source),
                         source.service ? source.service->nick : "");
}

bool CommandBSChannels::OnHelp(CommandSource &source,
                               const Anope::string &subcommand) {
  this->SendSyntax(source);
  source.Reply(" ");
  source.Reply(
      _("Lists the text channels of a space as \037name\037 (\037id\037). "
        "The space is given by its id, as shown by the \002GUILDS\002 "
        "command."));
  return true;
}

MODULE_INIT(ModuleBridgeServ)
