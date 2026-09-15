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

/* The Discord implementation of a bridge protocol. Everything which knows
 * about Discord or about the DPP library lives in this file; the service
 * itself knows only about the BridgeProtocol interface.
 */

#include "bridgeserv.h"
#include "convert.h"
#include "modules/bridgeserv/mailbox.h"
#include "modules/bridgeserv/render.h"

#include <dpp/dpp.h>
#include <dpp/intents.h>
#include <dpp/webhook.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Relay = BridgeServ::Relay;
namespace Text = BridgeServ::Text;

class DiscordProtocol;

/* Work handed from DPP's thread pool back to the main thread. */
using Mailbox = BridgeServ::Async::Mailbox<DiscordProtocol>;

/** Checks whether a string is usable as a Discord snowflake. */
static bool ValidSnowflake(const Anope::string &value) {
  if (value.empty() || value.length() > 20)
    return false;

  for (const auto chr : value) {
    if (chr < '0' || chr > '9')
      return false;
  }
  return Anope::TryConvert<uint64_t>(value).has_value();
}

/** Removes the control characters from a configured string. */
static std::string StripControl(const std::string &value) {
  std::string out;
  out.reserve(value.length());
  for (const auto chr : value) {
    if (static_cast<unsigned char>(chr) >= 0x20)
      out.push_back(chr);
  }
  return out;
}

/** What the Discord thread needs to know to discard traffic without waking
 * the main thread.
 *
 * DPP dispatches gateway events and REST completions on its own thread pool
 * and cluster::shutdown() does not cancel work which is already in flight, so
 * a callback can fire at any point during (and after) module unload.
 * Callbacks therefore never capture the protocol; they capture shared_ptrs
 * to this filter and to the mailbox, and hand work back to the main thread
 * with Mailbox::Post(). The protocol detaches the mailbox in its destructor,
 * after which a late callback is a no-op.
 */
class DiscordFilter final {
  std::mutex mutex;

  /* Discord channel ids which are bridged. */
  std::unordered_set<std::string> bridged_channels;

  /* Discord guild ids which have at least one bridged channel, so that
   * the Discord thread can ignore roster and presence traffic from the
   * other guilds the bot account happens to be in. */
  std::unordered_set<std::string> bridged_spaces;

  /* Webhook ids which this module created or adopted, so that the Discord
   * thread can discard the messages that it sent itself. */
  std::unordered_set<std::string> own_webhook_ids;

  /* The Discord user id of the bot account, so that the Discord thread can
   * discard the bot-account fallback messages that it sent itself. */
  std::string own_user_id;

  /* The presence Discord last reported per user id. */
  std::unordered_map<std::string, dpp::presence_status> presences;

  /* The custom emoji of each guild, name -> id, snapshotted from the
   * gateway so that the main thread can resolve ":name:" without reading
   * DPP's cache from the wrong thread. */
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      custom_emoji;

public:
  bool IsBridged(const std::string &channel_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->bridged_channels.count(channel_id) > 0;
  }

  bool IsBridgedSpace(const std::string &guild_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->bridged_spaces.count(guild_id) > 0;
  }

  void SetBridgedSpaces(std::unordered_set<std::string> guild_ids) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->bridged_spaces = std::move(guild_ids);
  }

  void SetBridged(std::unordered_set<std::string> channel_ids) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->bridged_channels = std::move(channel_ids);
  }

  bool IsOwnWebhook(const std::string &webhook_id) {
    if (webhook_id.empty() || webhook_id == "0")
      return false;

    std::lock_guard<std::mutex> lock(this->mutex);
    return this->own_webhook_ids.count(webhook_id) > 0;
  }

  void SetOwnWebhooks(std::unordered_set<std::string> webhook_ids) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->own_webhook_ids = std::move(webhook_ids);
  }

  void AddOwnWebhook(const std::string &webhook_id) {
    if (webhook_id.empty() || webhook_id == "0")
      return;

    std::lock_guard<std::mutex> lock(this->mutex);
    this->own_webhook_ids.insert(webhook_id);
  }

  void DelOwnWebhook(const std::string &webhook_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->own_webhook_ids.erase(webhook_id);
  }

  /** Determines whether a message was sent by the bot account itself. */
  bool IsSelf(const std::string &user_id) {
    if (user_id.empty() || user_id == "0")
      return false;

    std::lock_guard<std::mutex> lock(this->mutex);
    return this->own_user_id == user_id;
  }

  void SetSelf(const std::string &user_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->own_user_id = user_id;
  }

  /** Records the presence Discord last reported for a user. */
  void SetPresence(const std::string &user_id, dpp::presence_status status) {
    if (user_id.empty() || user_id == "0")
      return;

    std::lock_guard<std::mutex> lock(this->mutex);
    this->presences[user_id] = status;
  }

  /** The last presence reported for a user, if any was.
   *
   * Discord only sends presences with a guild create and then as updates,
   * and DPP caches neither, so they are kept here: a roster which is
   * rebuilt later would otherwise show everybody as present.
   */
  bool GetPresence(const std::string &user_id,
                   dpp::presence_status &status) {
    std::lock_guard<std::mutex> lock(this->mutex);
    const auto it = this->presences.find(user_id);
    if (it == this->presences.end())
      return false;

    status = it->second;
    return true;
  }

  /** Replaces the custom emoji known for a guild. */
  void SetCustomEmoji(const std::string &guild_id,
                      std::unordered_map<std::string, std::string> by_name) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->custom_emoji[guild_id] = std::move(by_name);
  }

  /** The id of a guild's custom emoji, or "" if it has none of that name. */
  std::string CustomEmoji(const std::string &guild_id,
                          const std::string &name) {
    std::lock_guard<std::mutex> lock(this->mutex);
    const auto guild = this->custom_emoji.find(guild_id);
    if (guild == this->custom_emoji.end())
      return "";
    const auto it = guild->second.find(name);
    return it == guild->second.end() ? "" : it->second;
  }
};

/** A thread running the shared DPP socket engine. */
class DiscordThread final : public Thread {
  dpp::cluster *cluster;
  std::shared_ptr<Mailbox> mailbox;

public:
  DiscordThread(dpp::cluster *c, std::shared_ptr<Mailbox> m)
      : cluster(c), mailbox(std::move(m)) {}

  void Run() override;
};

class DiscordProtocol final : public BridgeProtocol, public Pipe {
  dpp::cluster *cluster = nullptr;
  DiscordThread *thread = nullptr;
  std::shared_ptr<Mailbox> mailbox;
  std::shared_ptr<DiscordFilter> filter;

  Anope::string token;
  Anope::string domain;
  Anope::string webhook_name;
  Anope::string webhook_suffix;
  /* Whether an IRC "@nick" which names one of this bridge's own pseudo
   * clients is relayed as a real Discord mention. */
  bool relay_mentions = false;

  /* Whether the privileged GUILD_PRESENCES intent is requested, which
   * decides whether a bridged member can be shown away on IRC. Requesting
   * an intent which the application is not approved for makes Discord
   * close the gateway with 4014, so this is opt-in rather than inferred. */
  bool use_presence = false;
  bool connected = false;

  /* ------------------------------------------------------------------ */
  /* Rendering (called on the Discord thread; touches no Anope state)   */
  /* ------------------------------------------------------------------ */

  /** Renders a Discord message as IRC-ready text. */
  static Anope::string RenderMessage(const dpp::message &msg) {
    /* Mentioned users come with their guild membership attached, which is
     * the only place the per-guild nickname is available. */
    std::map<std::string, std::string> mentioned;
    for (const auto &[user, member] : msg.mentions) {
      std::string name = member.get_nickname();
      if (name.empty())
        name = user.global_name;
      if (name.empty())
        name = user.username;
      if (!name.empty())
        mentioned.emplace(user.id.str(), name);
    }

    /* Resolved names are literal text: they are escaped so that a display
     * name or role name containing Markdown is not turned into IRC
     * formatting when the message is converted below. */
    auto resolve = [&mentioned](char kind, const std::string &id,
                                const std::string &name) -> std::string {
      switch (kind) {
      case '@': {
        const auto it = mentioned.find(id);
        if (it != mentioned.end())
          return "@" + Text::EscapeMarkdown(it->second);

        if (const auto *user = dpp::find_user(dpp::snowflake(id)))
          return "@" + Text::EscapeMarkdown(user->global_name.empty()
                                                ? user->username
                                                : user->global_name);

        return "@unknown-user";
      }

      case '&': {
        if (const auto *role = dpp::find_role(dpp::snowflake(id)))
          return "@" + Text::EscapeMarkdown(role->name);
        return "@" + id;
      }

      case '#': {
        if (const auto *channel = dpp::find_channel(dpp::snowflake(id)))
          return "#" + Text::EscapeMarkdown(channel->name);
        return "#" + id;
      }

      case 'e':
        return ":" + Text::EscapeMarkdown(name) + ":";

      case 't':
        return Text::FormatUnixTime(id);
      }
      return "";
    };

    const auto render = [&resolve](const std::string &markdown) {
      return Text::MarkdownToIrc(Text::ExpandTokens(markdown, resolve));
    };

    std::string text = render(msg.content);

    if (!msg.attachments.empty()) {
      const size_t shown = std::min<size_t>(msg.attachments.size(), 4);
      for (size_t idx = 0; idx < shown; ++idx) {
        const auto &attachment = msg.attachments[idx];
        /* Media is labelled by what it is; anything else by its name. */
        const std::string &type = attachment.content_type;
        if (type.compare(0, 6, "image/") == 0)
          text += " [image: " + attachment.url + "]";
        else if (type.compare(0, 6, "video/") == 0)
          text += " [video: " + attachment.url + "]";
        else if (type.compare(0, 6, "audio/") == 0)
          text += " [audio: " + attachment.url + "]";
        else
          text += " [file: " + attachment.filename + " " + attachment.url + "]";
      }
      if (msg.attachments.size() > shown)
        text +=
            " [+" + std::to_string(msg.attachments.size() - shown) + " more]";
    }

    /* A sticker is an image the CDN serves by id; a Lottie sticker is a
     * vector animation with no image form, so only its name is shown. */
    for (const auto &sticker : msg.stickers) {
      text += " [sticker: " + sticker.name;
      switch (sticker.format_type) {
      case dpp::sf_png:
      case dpp::sf_apng:
        text += " https://media.discordapp.net/stickers/" + sticker.id.str() + ".png";
        break;
      case dpp::sf_gif:
        text += " https://media.discordapp.net/stickers/" + sticker.id.str() + ".gif";
        break;
      default:
        break;
      }
      text += "]";
    }

    /* A rich embed is bot output which has no other representation, so
     * it is always rendered. Every other kind (link, image, video, gifv,
     * article) is Discord's automatic preview of a URL which is already
     * in the content, so those are only shown when there is nothing
     * else. */
    for (const auto &embed : msg.embeds) {
      if (embed.type != "rich" && !text.empty())
        continue;

      std::string summary = embed.title;
      if (!embed.description.empty())
        summary += (summary.empty() ? "" : " \xe2\x80\x94 ") +
                   Text::TruncateCodePoints(embed.description, 300);

      const size_t shown = std::min<size_t>(embed.fields.size(), 3);
      for (size_t idx = 0; idx < shown; ++idx)
        summary += " [" + embed.fields[idx].name + ": " +
                   Text::TruncateCodePoints(embed.fields[idx].value, 100) + "]";

      if (summary.empty())
        continue;
      text += (text.empty() ? "" : "\n") + std::string("[embed] ") + render(summary);
    }

    /* A forwarded message carries its content in a snapshot. */
    if (text.empty() && msg.has_snapshot()) {
      const auto &forwarded = msg.message_snapshots.messages;
      const size_t shown = std::min<size_t>(forwarded.size(), 3);
      for (size_t idx = 0; idx < shown; ++idx) {
        const std::string content = render(forwarded[idx].content);
        if (content.empty())
          continue;
        text +=
            (text.empty() ? "" : "\n") + std::string("(forwarded) ") + content;
      }
    }

    /* Carriage returns and NULs can not be sent to IRC; newlines are
     * handled by the line splitter when the message is relayed. */
    std::string clean;
    clean.reserve(text.length());
    for (const auto chr : text) {
      if (chr != '\r' && chr != '\0')
        clean += chr;
    }
    return clean;
  }

  /** The text of a reaction emoji as IRC sees it: a unicode emoji as-is,
   * a guild's custom emoji as ":name:", the same shape RenderMessage
   * gives one in a message. */
  static std::string EmojiText(const dpp::emoji &emoji) {
    return emoji.id ? ":" + emoji.name + ":" : emoji.name;
  }

  /** Records the custom emoji of a guild, by name, from DPP's cache. */
  static void SnapshotEmoji(DiscordFilter &filter, dpp::snowflake guild_id,
                            const std::vector<dpp::snowflake> &ids) {
    std::unordered_map<std::string, std::string> by_name;
    for (const auto id : ids) {
      if (const auto *emoji = dpp::find_emoji(id))
        by_name[emoji->name] = id.str();
    }
    filter.SetCustomEmoji(guild_id.str(), std::move(by_name));
  }

  /** Resolves the bridged channel a Discord channel maps to: itself, or
   * its parent when it is a thread of a bridged channel.
   * @return The bridged channel id, or an empty string if there is none.
   */
  static std::string BridgedChannel(DiscordFilter &filter,
                                    dpp::snowflake channel_id) {
    const std::string id = channel_id.str();
    if (filter.IsBridged(id))
      return id;

    if (const auto *channel = dpp::find_channel(channel_id)) {
      const std::string parent = channel->parent_id.str();
      if (channel->parent_id && filter.IsBridged(parent))
        return parent;
    }
    return "";
  }

  /** Handles an incoming Discord message on the Discord thread. */
  static void HandleMessage(const std::shared_ptr<DiscordFilter> &filter,
                            const std::shared_ptr<Mailbox> &mailbox,
                            const dpp::message &msg, bool edit) {
    const std::string channel_id = BridgedChannel(*filter, msg.channel_id);
    if (channel_id.empty())
      return;

    /* Messages which this module sent itself must not come back: either
     * through one of its webhooks, or through the bot account when the
     * webhook fallback is in use. Other bots are relayed as normal. */
    if (filter->IsOwnWebhook(msg.webhook_id.str()) ||
        filter->IsSelf(msg.author.id.str()))
      return;

    BridgeMessage relay;
    relay.text = RenderMessage(msg);
    if (relay.text.empty())
      return;

    relay.protocol = "discord";
    relay.channel = channel_id;
    relay.space = msg.guild_id.str();
    relay.user_id = msg.author.id.str();
    relay.msg_id = msg.id.str();
    relay.edit = edit;

    /* A forward references the original message too, but is not a reply
     * to it. */
    if (msg.message_reference.message_id &&
        msg.message_reference.type == dpp::mrt_default && !msg.has_snapshot())
      relay.reply_to = msg.message_reference.message_id.str();

    /* A message in a thread of the bridged channel is relayed into the
     * same IRC channel, labelled with the thread it came from. */
    if (msg.channel_id.str() != channel_id) {
      relay.thread = msg.channel_id.str();
      if (const auto *thread = dpp::find_channel(msg.channel_id))
        relay.thread_name = thread->name;
    }

    std::string display = msg.member.get_nickname();
    if (display.empty())
      display = msg.author.global_name;
    if (display.empty())
      display = msg.author.username;
    if (display.empty())
      display = "discord";
    relay.display = display;

    mailbox->Post([relay](DiscordProtocol *protocol) {
      protocol->core->RelayToIrc(relay);
    });
  }

  /* ------------------------------------------------------------------ */
  /* Roster and presence (Discord thread)                               */
  /* ------------------------------------------------------------------ */

  /** The name a guild member should appear under on IRC.
   *
   * The per-guild nickname wins, then the account display name, then the
   * login name — the same order a Discord client shows them in.
   */
  static std::string MemberDisplay(const dpp::guild_member &member) {
    std::string display = member.get_nickname();
    if (display.empty()) {
      /* The user object is cached by the same gateway events which carry
       * the membership, so this is a cache read and not a REST call. */
      if (const auto *user = member.get_user()) {
        display = user->global_name;
        if (display.empty())
          display = user->username;
      }
    }
    if (display.empty())
      display = "discord";
    return display;
  }

  /** Anything but "online" is away on IRC: idle, do-not-disturb and
   * offline all mean the person is not there to read the channel.
   */
  static bool PresenceAway(dpp::presence_status status) {
    return status != dpp::ps_online;
  }

  static Anope::string PresenceReason(dpp::presence_status status) {
    switch (status) {
    case dpp::ps_idle:
      return "Idle on Discord";
    case dpp::ps_dnd:
      return "Do not disturb on Discord";
    case dpp::ps_offline:
      return "Offline on Discord";
    default:
      return "Away on Discord";
    }
  }

  /** Hands a set of guild members to the service as a roster.
   * @param presences Presences which came with this event, empty for the
   *                  events which carry none. Any presence already known
   *                  for a member is applied whether or not it is here.
   */
  static void HandleRoster(const std::shared_ptr<DiscordFilter> &filter,
                           const std::shared_ptr<Mailbox> &mailbox,
                           dpp::snowflake guild_id,
                           const dpp::guild_member_map &members,
                           const dpp::presence_map &presences) {
    const std::string space = guild_id.str();
    if (space.empty() || !filter->IsBridgedSpace(space))
      return;

    std::vector<BridgeMember> roster;
    roster.reserve(members.size());
    for (const auto &[user_id, member] : members) {
      /* The bridge's own account must not be given a pseudo client: it
       * stands for IRC on Discord, not the other way round. */
      const std::string id = user_id.str();
      if (id.empty() || filter->IsSelf(id))
        continue;

      BridgeMember entry;
      entry.user_id = id;
      entry.display = MemberDisplay(member);

      /* Presences arrive once, with the guild create; the member list
       * arrives in chunks afterwards, so the two are joined through the
       * filter's cache rather than only within one event. */
      const auto presence = presences.find(user_id);
      if (presence != presences.end())
        filter->SetPresence(id, presence->second.status());

      dpp::presence_status status = dpp::ps_online;
      if (filter->GetPresence(id, status)) {
        entry.presence_known = true;
        entry.away = PresenceAway(status);
        entry.away_reason = PresenceReason(status);
      }
      roster.push_back(std::move(entry));
    }

    if (roster.empty())
      return;

    const Anope::string key = space;
    mailbox->Post([key, roster](DiscordProtocol *protocol) {
      protocol->core->SyncRoster(protocol->GetName(), key, roster);
    });
  }

  /* ------------------------------------------------------------------ */
  /* Connection lifecycle                                               */
  /* ------------------------------------------------------------------ */

  void StartCluster() {
    if (this->cluster)
      return;

    if (this->token.empty()) {
      Log(this->core->GetOwner())
          << "BridgeServ: no Discord token is configured; the Discord link is "
             "disabled.";
      return;
    }

    /* A completion which was in flight when the previous cluster went
     * away will never arrive, so nothing may still be waiting on one. */
    for (auto *bridge : this->core->GetBridges()) {
      if (bridge->protocol.equals_ci(this->GetName()))
        bridge->endpoint_pending = false;
    }

    /* GUILD_MEMBERS is what makes the roster visible at all: without it
     * Discord sends no member list and the IRC channel can only ever show
     * the people who have spoken. GUILD_PRESENCES is only added when the
     * operator has enabled it, because an intent the application is not
     * approved for is answered with gateway close 4014. Reactions and
     * typing are not privileged and are always requested. */
    uint32_t intents = dpp::i_guilds | dpp::i_guild_messages |
                       dpp::i_message_content | dpp::i_guild_members |
                       dpp::i_guild_message_reactions |
                       dpp::i_guild_message_typing;
    if (this->use_presence)
      intents |= dpp::i_guild_presences;

    this->cluster = new dpp::cluster(this->token.str(), intents);

    auto mailbox = this->mailbox;
    auto filter = this->filter;

    this->cluster->on_log([mailbox](const dpp::log_t &event) {
      if (event.severity < dpp::ll_warning)
        return;

      const std::string message = event.message;
      mailbox->Post([message](DiscordProtocol *protocol) {
        Log(protocol->core->GetOwner()) << "DPP: " << message;

        /* 4014 is Discord refusing a privileged intent. It is fatal to
         * the gateway, so say which switch fixes it rather than leaving
         * a bare DPP error in the log. */
        if (message.find("4014") == std::string::npos)
          return;

        Log(protocol->core->GetOwner())
            << "BridgeServ: Discord refused a privileged gateway intent. "
            << "Enable SERVER MEMBERS INTENT"
            << (protocol->use_presence ? " and PRESENCE INTENT" : "")
            << " for this application at "
            << "https://discord.com/developers, or set <bridgeserv:"
            << "usepresence> to no; the bridge cannot connect until then.";
      });
    });

    this->cluster->on_ready([mailbox, filter](const dpp::ready_t &event) {
      const auto guilds = event.guild_count;

      /* The bot account is only known once the gateway says hello. */
      if (event.owner)
        filter->SetSelf(event.owner->me.id.str());

      mailbox->Post(
          [guilds](DiscordProtocol *protocol) { protocol->OnReady(guilds); });
    });

    this->cluster->on_message_create(
        [mailbox, filter](const dpp::message_create_t &event) {
          HandleMessage(filter, mailbox, event.msg, false);
        });

    this->cluster->on_message_update(
        [mailbox, filter](const dpp::message_update_t &event) {
          HandleMessage(filter, mailbox, event.msg, true);
        });

    this->cluster->on_message_delete([mailbox, filter](
                                         const dpp::message_delete_t &event) {
      const std::string channel_id = BridgedChannel(*filter, event.channel_id);
      if (channel_id.empty())
        return;

      BridgeMessage relay;
      relay.protocol = "discord";
      relay.channel = channel_id;
      relay.space = event.guild_id.str();
      relay.msg_id = event.id.str();
      relay.del = true;

      mailbox->Post([relay](DiscordProtocol *protocol) {
        protocol->core->RelayToIrc(relay);
      });
    });

    /* Reactions are relayed from the reacting member's pseudo client.
     * Clearing every reaction, or every reaction of one emoji, carries no
     * per-user information to attribute an unreact to, so those two
     * events are left alone. */
    this->cluster->on_message_reaction_add(
        [mailbox, filter](const dpp::message_reaction_add_t &event) {
          const std::string channel_id =
              BridgedChannel(*filter, event.channel_id);
          const std::string user_id = event.reacting_user.id.str();
          if (channel_id.empty() || user_id.empty() || filter->IsSelf(user_id))
            return;

          BridgeReaction reaction;
          reaction.protocol = "discord";
          reaction.space = event.reacting_guild.id.str();
          reaction.channel = channel_id;
          reaction.user_id = user_id;
          reaction.remote_id = event.message_id.str();
          reaction.emoji = EmojiText(event.reacting_emoji);
          reaction.add = true;

          std::string display = MemberDisplay(event.reacting_member);
          if (display.empty())
            display = event.reacting_user.global_name;
          if (display.empty())
            display = event.reacting_user.username;
          reaction.display = display;

          mailbox->Post([reaction](DiscordProtocol *protocol) {
            protocol->core->RelayReaction(reaction);
          });
        });

    this->cluster->on_message_reaction_remove(
        [mailbox, filter](const dpp::message_reaction_remove_t &event) {
          const std::string channel_id =
              BridgedChannel(*filter, event.channel_id);
          const std::string user_id = event.reacting_user_id.str();
          if (channel_id.empty() || user_id.empty() || filter->IsSelf(user_id))
            return;

          BridgeReaction reaction;
          reaction.protocol = "discord";
          reaction.space = event.reacting_guild.id.str();
          reaction.channel = channel_id;
          reaction.user_id = user_id;
          reaction.remote_id = event.message_id.str();
          reaction.emoji = EmojiText(event.reacting_emoji);
          reaction.add = false;

          mailbox->Post([reaction](DiscordProtocol *protocol) {
            protocol->core->RelayReaction(reaction);
          });
        });

    this->cluster->on_typing_start(
        [mailbox, filter](const dpp::typing_start_t &event) {
          const std::string channel_id =
              BridgedChannel(*filter, event.typing_channel.id);
          const std::string user_id = event.user_id.str();
          if (channel_id.empty() || user_id.empty() || filter->IsSelf(user_id))
            return;

          const Anope::string space = event.typing_guild.id.str();
          const Anope::string channel = channel_id;
          const Anope::string who = user_id;
          mailbox->Post([space, channel, who](DiscordProtocol *protocol) {
            protocol->core->RelayTyping(protocol->GetName(), space, channel,
                                        who);
          });
        });

    /* A guild create carries the members Discord sends up front; DPP then
     * asks for the rest of the roster, which arrives as member chunks. It
     * also carries the guild's custom emoji, which are kept for
     * resolving ":name:" reactions from IRC. */
    this->cluster->on_guild_create(
        [mailbox, filter](const dpp::guild_create_t &event) {
          SnapshotEmoji(*filter, event.created.id, event.created.emojis);
          HandleRoster(filter, mailbox, event.created.id, event.created.members,
                       event.presences);
        });

    this->cluster->on_guild_emojis_update(
        [filter](const dpp::guild_emojis_update_t &event) {
          SnapshotEmoji(*filter, event.updating_guild.id, event.emojis);
        });

    this->cluster->on_guild_members_chunk(
        [mailbox, filter](const dpp::guild_members_chunk_t &event) {
          HandleRoster(filter, mailbox, event.adding.id, event.members, {});
        });

    this->cluster->on_guild_member_add(
        [mailbox, filter](const dpp::guild_member_add_t &event) {
          dpp::guild_member_map one;
          one[event.added.user_id] = event.added;
          HandleRoster(filter, mailbox, event.adding_guild.id, one, {});
        });

    /* A nickname change arrives here; the roster path renames the pseudo
     * client in place rather than reintroducing it. */
    this->cluster->on_guild_member_update(
        [mailbox, filter](const dpp::guild_member_update_t &event) {
          dpp::guild_member_map one;
          one[event.updated.user_id] = event.updated;
          HandleRoster(filter, mailbox, event.updating_guild.id, one, {});
        });

    this->cluster->on_guild_member_remove(
        [mailbox, filter](const dpp::guild_member_remove_t &event) {
          const std::string space = event.guild_id.str();
          const std::string user_id = event.removed.id.str();
          if (space.empty() || user_id.empty() ||
              !filter->IsBridgedSpace(space))
            return;

          const Anope::string key = space;
          const Anope::string who = user_id;
          mailbox->Post([key, who](DiscordProtocol *protocol) {
            protocol->core->RemoveMember(protocol->GetName(), key, who);
          });
        });

    this->cluster->on_presence_update(
        [mailbox, filter](const dpp::presence_update_t &event) {
          const std::string space = event.rich_presence.guild_id.str();
          const std::string user_id = event.rich_presence.user_id.str();
          if (space.empty() || user_id.empty() ||
              !filter->IsBridgedSpace(space) || filter->IsSelf(user_id))
            return;

          const auto status = event.rich_presence.status();
          filter->SetPresence(user_id, status);

          const Anope::string key = space;
          const Anope::string who = user_id;
          const bool away = PresenceAway(status);
          const Anope::string reason = PresenceReason(status);
          mailbox->Post([key, who, away, reason](DiscordProtocol *protocol) {
            protocol->core->SetPresence(protocol->GetName(), key, who, away,
                                        reason);
          });
        });

    this->thread = new DiscordThread(this->cluster, this->mailbox);
    this->thread->Start();
  }

  void StopCluster() {
    this->connected = false;

    if (this->cluster)
      this->cluster->shutdown();

    if (this->thread) {
      this->thread->Join();
      delete this->thread;
      this->thread = nullptr;
    }

    delete this->cluster;
    this->cluster = nullptr;
  }

  /* ------------------------------------------------------------------ */
  /* Webhooks                                                           */
  /* ------------------------------------------------------------------ */

  void CreateWebhook(const Anope::string &key, const Anope::string &channel) {
    Bridge *bridge = this->core->FindIrc(key);
    if (!bridge)
      return; // the bridge was removed while the listing was in flight.

    /* The bridge was repointed while the listing was in flight; the
     * pending guard is released so that the new channel is set up. */
    if (!bridge->foreign_channel.equals_ci(channel)) {
      bridge->endpoint_pending = false;
      this->EnsureWebhook(bridge);
      return;
    }

    if (!this->cluster || !this->connected) {
      /* The Discord link dropped between listing and creating; release
       * the guard so that a later message retries rather than leaving
       * the bridge pending forever. */
      this->OnWebhookFailed(key, "the Discord link went away");
      return;
    }

    dpp::webhook hook;
    hook.channel_id = dpp::snowflake(bridge->foreign_channel.c_str());
    hook.name = this->webhook_name.str();

    auto mailbox = this->mailbox;
    try {
      this->cluster->create_webhook(
          hook,
          [mailbox, key, channel](const dpp::confirmation_callback_t &cb) {
            if (cb.is_error()) {
              const std::string error = cb.get_error().human_readable;
              mailbox->Post([key, error](DiscordProtocol *protocol) {
                protocol->OnWebhookFailed(key, error);
              });
              return;
            }

            std::string id;
            std::string tok;
            try {
              const auto created = cb.get<dpp::webhook>();
              id = created.id.str();
              tok = created.token;
            } catch (const dpp::exception &) {
              mailbox->Post([key](DiscordProtocol *protocol) {
                protocol->OnWebhookFailed(key, "malformed webhook response");
              });
              return;
            }

            mailbox->Post([key, channel, id, tok](DiscordProtocol *protocol) {
              protocol->OnWebhookReady(key, channel, id, tok, true);
            });
          });
    } catch (const dpp::exception &err) {
      this->OnWebhookFailed(key, err.what());
    }
  }

  void OnWebhookReady(const Anope::string &key, const Anope::string &channel,
                      const std::string &id, const std::string &tok,
                      bool created) {
    Bridge *bridge = this->core->FindIrc(key);
    if (!bridge)
      return;

    /* The bridge may have been repointed while the lookup was in flight;
     * adopting the old channel's webhook would send IRC traffic to the
     * wrong Discord channel. */
    if (!bridge->foreign_channel.equals_ci(channel)) {
      bridge->endpoint_pending = false;
      Log(this->core->GetOwner())
          << "BridgeServ: discarding a webhook for " << key
          << " which no longer points at " << channel;
      this->EnsureWebhook(bridge);
      return;
    }

    bridge->endpoint_pending = false;
    bridge->endpoint_retry_at = 0;
    bridge->endpoint_id = id;
    bridge->endpoint_token = tok;
    this->core->SaveBridge(bridge);

    this->filter->AddOwnWebhook(id);
    Log(this->core->GetOwner())
        << "BridgeServ: " << (created ? "created" : "adopted")
        << " Discord webhook " << id << " for " << key;
  }

  void OnWebhookFailed(const Anope::string &key, const std::string &error) {
    Bridge *bridge = this->core->FindIrc(key);
    if (!bridge)
      return;

    bridge->endpoint_pending = false;
    bridge->endpoint_retry_at = Anope::CurTime + 60;
    Log(this->core->GetOwner())
        << "BridgeServ: webhook setup for " << key << " failed: " << error;
  }

  void OnWebhookSendFailed(const Anope::string &key, uint16_t status,
                           const std::string &error) {
    Bridge *bridge = this->core->FindIrc(key);
    if (!bridge)
      return;

    /* The webhook was deleted on the Discord side or its token was
     * revoked; drop it and set a new one up. */
    if (status == 401 || status == 403 || status == 404) {
      /* An isolated failure is retried at once so that a deleted
       * webhook heals on the next message, but a webhook which keeps
       * failing is backed off like any other setup failure. */
      if (Anope::CurTime - bridge->endpoint_failed_at > 300)
        bridge->endpoint_failures = 0;
      bridge->endpoint_failed_at = Anope::CurTime;
      ++bridge->endpoint_failures;

      this->filter->DelOwnWebhook(bridge->endpoint_id.str());
      bridge->endpoint_id.clear();
      bridge->endpoint_token.clear();
      bridge->endpoint_pending = false;
      bridge->endpoint_retry_at =
          bridge->endpoint_failures > 1 ? Anope::CurTime + 60 : 0;
      this->core->SaveBridge(bridge);

      Log(this->core->GetOwner())
          << "BridgeServ: the webhook for " << key << " is no longer usable ("
          << status << "); a new one will be created.";
      this->EnsureWebhook(bridge);
      return;
    }
    Log(this->core->GetOwner()) << "BridgeServ: relaying to the webhook for "
                                << key << " failed: " << error;
  }

  void EnsureWebhook(Bridge *bridge) {
    if (!this->cluster || !this->connected)
      return;
    if (!bridge->endpoint_id.empty() || bridge->endpoint_pending)
      return;
    if (Anope::CurTime < bridge->endpoint_retry_at)
      return;

    bridge->endpoint_pending = true;

    auto mailbox = this->mailbox;
    const Anope::string key = bridge->irc_channel;
    const Anope::string channel = bridge->foreign_channel;
    const std::string wanted = this->webhook_name.str();

    /* Reuse the webhook from a previous run rather than creating a new one
     * on every load, which would litter the channel with dead webhooks. */
    try {
      this->cluster->get_channel_webhooks(
          dpp::snowflake(channel.c_str()),
          [mailbox, key, channel,
           wanted](const dpp::confirmation_callback_t &cb) {
            if (cb.is_error()) {
              const std::string error = cb.get_error().human_readable;
              mailbox->Post([key, error](DiscordProtocol *protocol) {
                protocol->OnWebhookFailed(key, error);
              });
              return;
            }

            std::string id;
            std::string tok;
            try {
              for (const auto &[hook_id, hook] : cb.get<dpp::webhook_map>()) {
                if (hook.name != wanted || hook.token.empty())
                  continue;

                id = hook_id.str();
                tok = hook.token;
                break;
              }
            } catch (const dpp::exception &) {
              mailbox->Post([key](DiscordProtocol *protocol) {
                protocol->OnWebhookFailed(key,
                                          "malformed webhook list response");
              });
              return;
            }

            if (id.empty()) {
              mailbox->Post([key, channel](DiscordProtocol *protocol) {
                protocol->CreateWebhook(key, channel);
              });
              return;
            }
            mailbox->Post([key, channel, id, tok](DiscordProtocol *protocol) {
              protocol->OnWebhookReady(key, channel, id, tok, false);
            });
          });
    } catch (const dpp::exception &err) {
      this->OnWebhookFailed(key, err.what());
    }
  }

  void OnReady(uint32_t guilds) {
    this->connected = true;
    Log(this->core->GetOwner())
        << "BridgeServ: connected to Discord (" << guilds << " guild(s)).";

    /* Ready fires again after a reconnect; webhook setup is idempotent,
     * and a fresh connection is not made to wait out a backoff that was
     * earned on the old one. */
    for (auto *bridge : this->core->GetBridges()) {
      if (!bridge->protocol.equals_ci(this->GetName()))
        continue;

      bridge->endpoint_retry_at = 0;
      this->EnsureWebhook(bridge);
    }
  }

  void OnStopped(const std::string &error) {
    this->connected = false;
    Log(this->core->GetOwner())
        << "BridgeServ: the Discord connection stopped: " << error;
  }

  friend class DiscordThread;

public:
  explicit DiscordProtocol(BridgeCore *c)
      : BridgeProtocol("discord", c),
        mailbox(std::make_shared<Mailbox>(this, [this] { this->Notify(); })),
        filter(std::make_shared<DiscordFilter>()) {}

  ~DiscordProtocol() override {
    /* Detach first: any Discord callback which is already in flight must
     * not be able to reach this object once it starts being destroyed.
     * StopCluster then joins DPP's threads, so a wake which was copied
     * out of the mailbox before the detach still runs against a live
     * object. */
    this->mailbox->Detach();
    this->StopCluster();
  }

  const Anope::string &GetDomain() const override { return this->domain; }

  bool IsConnected() const override { return this->connected; }

  bool IsValidId(const Anope::string &id) const override {
    return ValidSnowflake(id);
  }

  void Configure(Configuration::Block &block) override {
    Anope::string domain_conf =
        block.Get<const Anope::string>("domain", "discord.bridge");
    if (IRCD && !IRCD->IsHostValid(domain_conf)) {
      Log(this->core->GetOwner())
          << "BridgeServ: domain " << domain_conf
          << " is not a valid hostname; using discord.bridge";
      domain_conf = "discord.bridge";
    }
    this->domain = domain_conf;

    /* Discord rejects control characters in webhook names and limits
     * them to 80 characters. */
    Anope::string name = Text::TruncateCodePoints(
        StripControl(
            block.Get<const Anope::string>("bridgename", "IRC Bridge").str()),
        80);
    if (name.empty()) {
      Log(this->core->GetOwner())
          << "BridgeServ: bridgename is empty; using \"IRC Bridge\".";
      name = "IRC Bridge";
    }
    const bool name_changed =
        !this->webhook_name.empty() && this->webhook_name != name;
    this->webhook_name = name;
    this->webhook_suffix = StripControl(
        block.Get<const Anope::string>("webhooksuffix", " (IRC)").str());
    /* Whether an IRC "@nick" which names one of this bridge's own pseudo
     * clients is relayed as a real Discord mention. Off by default: the
     * upstream contract is that a relayed line pings nobody. */
    this->relay_mentions = block.Get<bool>("relaymentions", "no");

    /* The intent set is fixed when the gateway session is opened, so a
     * change of usepresence needs a fresh connection to take effect. */
    const bool want_presence = block.Get<bool>("usepresence", "no");
    const bool presence_changed = want_presence != this->use_presence;

    const Anope::string new_token = block.Get<const Anope::string>("token");
    if (new_token != this->token || presence_changed || !this->cluster) {
      this->StopCluster();
      this->token = new_token;
      this->use_presence = want_presence;
      this->StartCluster();
    }

    /* Existing webhooks carry the old name; they are replaced so that
     * the name is what the operator configured everywhere. */
    if (name_changed && this->cluster && this->connected) {
      for (auto *bridge : this->core->GetBridges()) {
        if (!bridge->protocol.equals_ci(this->GetName()) ||
            bridge->endpoint_id.empty())
          continue;

        this->OnBridgeRemoved(bridge);
        this->core->SaveBridge(bridge);
        this->EnsureWebhook(bridge);
      }
    }
  }

  void OnBridgesChanged() override {
    std::unordered_set<std::string> channels;
    std::unordered_set<std::string> spaces;
    std::unordered_set<std::string> webhooks;
    for (const auto *bridge : this->core->GetBridges()) {
      if (!bridge->protocol.equals_ci(this->GetName()))
        continue;

      channels.insert(bridge->foreign_channel.str());
      if (!bridge->space.empty())
        spaces.insert(bridge->space.str());
      if (!bridge->endpoint_id.empty())
        webhooks.insert(bridge->endpoint_id.str());
    }

    this->filter->SetBridged(std::move(channels));
    this->filter->SetBridgedSpaces(std::move(spaces));
    this->filter->SetOwnWebhooks(std::move(webhooks));

    if (!this->connected)
      return;

    for (auto *bridge : this->core->GetBridges()) {
      if (bridge->protocol.equals_ci(this->GetName()))
        this->EnsureWebhook(bridge);
    }

    /* A bridge which was just pointed at a guild has no membership on
     * IRC yet, and no further guild create is coming for a guild which
     * was already connected. */
    this->RefreshRoster();
  }

  void RefreshRoster() override {
    if (!this->cluster || !this->connected)
      return;

    std::unordered_set<std::string> spaces;
    for (const auto *bridge : this->core->GetBridges()) {
      if (bridge->protocol.equals_ci(this->GetName()) &&
          !bridge->space.empty())
        spaces.insert(bridge->space.str());
    }

    for (const auto &space : spaces) {
      /* DPP keeps the membership of every guild it has seen, so the
       * roster is rebuilt from the cache without asking Discord again. */
      const dpp::guild *guild = dpp::find_guild(dpp::snowflake(space));
      if (!guild)
        continue;

      std::vector<BridgeMember> roster;
      roster.reserve(guild->members.size());
      for (const auto &[user_id, member] : guild->members) {
        const std::string id = user_id.str();
        if (id.empty() || this->filter->IsSelf(id))
          continue;

        BridgeMember entry;
        entry.user_id = id;
        entry.display = MemberDisplay(member);

        dpp::presence_status status = dpp::ps_online;
        if (this->filter->GetPresence(id, status)) {
          entry.presence_known = true;
          entry.away = PresenceAway(status);
          entry.away_reason = PresenceReason(status);
        }
        roster.push_back(std::move(entry));
      }

      if (!roster.empty())
        this->core->SyncRoster(this->GetName(), space, roster);
    }
  }

  void OnBridgeRemoved(Bridge *bridge) override {
    if (bridge->endpoint_id.empty())
      return;

    const std::string id = bridge->endpoint_id.str();
    this->filter->DelOwnWebhook(id);

    if (this->cluster && this->connected) {
      auto mailbox = this->mailbox;
      const Anope::string key = bridge->irc_channel;
      try {
        this->cluster->delete_webhook(
            dpp::snowflake(bridge->endpoint_id.c_str()),
            [mailbox, key](const dpp::confirmation_callback_t &cb) {
              if (!cb.is_error())
                return;

              const std::string error = cb.get_error().human_readable;
              mailbox->Post([key, error](DiscordProtocol *protocol) {
                Log(protocol->core->GetOwner())
                    << "BridgeServ: unable to delete the webhook for " << key
                    << ": " << error;
              });
            });
      } catch (const dpp::exception &err) {
        Log(this->core->GetOwner())
            << "BridgeServ: unable to delete the webhook for " << key << ": "
            << err.what();
      }
    }

    bridge->endpoint_id.clear();
    bridge->endpoint_token.clear();
    bridge->endpoint_pending = false;
    bridge->endpoint_retry_at = 0;
  }

  /** Renders the quote line which a reply from IRC carries.
   *
   * A webhook cannot set message_reference, so a reply is rendered as a
   * Discord block quote of what it answers, with a jump link, followed
   * by the text. The quote is elided when the msgid being replied to is
   * not one of ours: the IRC client already showed the context.
   *
   * @param thread Receives the thread the quoted message lives in, so
   *               that the reply can be posted into the same thread.
   */
  std::string RenderQuote(Bridge *bridge, const Anope::string &reply_to,
                          dpp::snowflake &thread) {
    const Anope::string remote_id = this->core->RemoteIdFor(reply_to);
    if (remote_id.empty())
      return "";

    Anope::string author, excerpt, in_thread;
    const bool known =
        this->core->QuotedMessage(remote_id, author, excerpt, in_thread);
    if (known && !in_thread.empty())
      thread = dpp::snowflake(in_thread.c_str());

    const std::string where =
        thread ? in_thread.str() : bridge->foreign_channel.str();
    /* U+2197 NORTH EAST ARROW, the conventional "jump to" glyph. */
    const std::string link = "[\xe2\x86\x97](https://discord.com/channels/" +
                             bridge->space.str() + "/" + where + "/" +
                             remote_id.str() + ")";
    if (!known)
      return "> " + link + "\n";

    /* The excerpt is IRC-rendered text: strip its formatting, and keep
     * it on the one quote line. */
    std::string plain = Anope::RemoveFormatting(excerpt).str();
    std::replace(plain.begin(), plain.end(), '\n', ' ');
    return "> **" + dpp::utility::markdown_escape(author.str()) +
           "**: " + dpp::utility::markdown_escape(plain) + " " + link + "\n";
  }

  void Notice(Bridge *bridge, const Anope::string &text) override {
    if (!this->cluster || !this->connected || text.empty())
      return;

    /* An event is about the channel rather than from anyone in it, so
     * it comes from the bot account, not from a user's webhook, as one
     * italic line. The whole text is literal and is escaped as such. */
    std::string body = Text::TruncateCodePoints(
        dpp::utility::markdown_escape(text.str(), true), 1998);
    size_t slashes = 0;
    while (slashes < body.length() && body[body.length() - 1 - slashes] == '\\')
      ++slashes;
    if (slashes % 2)
      body.erase(body.length() - 1);
    if (body.empty())
      return;

    dpp::message msg(dpp::snowflake(bridge->foreign_channel.c_str()),
                     "*" + body + "*");
    msg.set_allowed_mentions(false, false, false, false);
    try {
      this->cluster->message_create(msg);
    } catch (const dpp::exception &err) {
      Log(this->core->GetOwner()) << "BridgeServ: unable to relay an event to "
                                  << bridge->irc_channel << ": " << err.what();
    }
  }

  void React(Bridge *bridge, const Anope::string &remote_id,
             const Anope::string &channel, const Anope::string &emoji,
             bool add) override {
    if (!this->cluster || !this->connected)
      return;

    /* ":name:" is one of the guild's custom emoji, which the API wants as
     * "name:id". A name the guild does not have is dropped: sent as text
     * it would be rejected, and it is not a unicode emoji either. */
    std::string reaction = emoji.str();
    if (reaction.length() > 2 && reaction.front() == ':' &&
        reaction.back() == ':') {
      const std::string name = reaction.substr(1, reaction.length() - 2);
      const bool plain = std::all_of(name.begin(), name.end(), [](char chr) {
        return std::isalnum(static_cast<unsigned char>(chr)) || chr == '_';
      });
      const std::string id =
          plain ? this->filter->CustomEmoji(bridge->space.str(), name) : "";
      if (id.empty()) {
        Log(LOG_DEBUG) << "BridgeServ: no custom emoji named " << name
                       << " in space " << bridge->space;
        return;
      }
      reaction = name + ":" + id;
    }

    /* The reaction is the bot account's: Discord has no per-webhook
     * reactions, so two IRC users reacting with the same emoji collapse
     * into one, and an unreact from either removes it. */
    try {
      const dpp::snowflake message(remote_id.c_str());
      const dpp::snowflake where(channel.c_str());
      if (add)
        this->cluster->message_add_reaction(message, where, reaction);
      else
        this->cluster->message_delete_own_reaction(message, where, reaction);
    } catch (const dpp::exception &err) {
      Log(this->core->GetOwner()) << "BridgeServ: unable to react in "
                                  << bridge->irc_channel << ": " << err.what();
    }
  }

  void Typing(Bridge *bridge) override {
    if (!this->cluster || !this->connected)
      return;

    /* Discord has no per-webhook typing, so the indicator shows the bot
     * account's name rather than the IRC nick; it is still the only way
     * to show the channel that a reply is being written. */
    try {
      this->cluster->channel_typing(
          dpp::snowflake(bridge->foreign_channel.c_str()));
    } catch (const dpp::exception &err) {
      Log(this->core->GetOwner()) << "BridgeServ: unable to show typing in "
                                  << bridge->irc_channel << ": " << err.what();
    }
  }

  /* Renders an IRC line for Discord: a mention of one of this bridge's own
   * pseudo clients becomes a real Discord mention, and everything else is
   * markdown-escaped exactly as the whole body used to be. Every id that
   * was substituted is collected into `pinged`, which becomes the message's
   * allowed_mentions allow-list.
   *
   * Runs on the Anope thread — Relay() is called straight out of
   * BridgeCore::OnPrivmsg, never from the DPP thread — so reaching into the
   * core's nick/id mapping here is safe.
   */
  std::string RenderMentions(Bridge *bridge, const std::string &text,
                             std::vector<dpp::snowflake> &pinged) {
    const auto escape = [](const std::string &run) {
      return dpp::utility::markdown_escape(run, true);
    };
    if (!this->relay_mentions)
      return escape(text);

    const auto resolve = [&](const std::string &nick) -> std::string {
      const Anope::string id = this->core->RemoteIdForNick(bridge, nick);
      if (id.empty())
        return "";
      pinged.emplace_back(id.c_str());
      return "<@" + id.str() + ">";
    };
    return Text::ExpandMentions(text, resolve, escape);
  }

  void Relay(Bridge *bridge, const BridgeOutbound &out) override {
    if (!this->cluster || !this->connected)
      return;

    /* A reply to a message which lives in a thread is posted into that
     * thread; everything else goes to the channel itself. */
    dpp::snowflake thread_id = 0;
    const std::string quote =
        out.reply_to.empty() ? "" : this->RenderQuote(bridge, out.reply_to, thread_id);

    std::vector<dpp::snowflake> pinged;
    std::string text = Text::EscapeLineStart(
        this->RenderMentions(bridge, out.text.str(), pinged));

    /* Discord rejects messages longer than 2000 characters. The body is
     * truncated before the italic markers are added so that an oversized
     * action does not lose its closing marker, and the quote is never
     * truncated, only the body which follows it. */
    const size_t limit = out.action ? 1998 : 2000;
    text = Text::TruncateCodePoints(
        text, quote.length() < limit ? limit - quote.length() : 1);

    /* A trailing escape left behind by the truncation would escape the
     * closing marker of an action, or leak as a literal backslash. */
    size_t slashes = 0;
    while (slashes < text.length() && text[text.length() - 1 - slashes] == '\\')
      ++slashes;
    if (slashes % 2)
      text.erase(text.length() - 1);

    if (text.empty())
      return;

    if (out.action)
      text = "*" + text + "*";

    const dpp::snowflake channel(bridge->foreign_channel.c_str());
    dpp::message msg(channel, quote + text);

    /* A relayed line pings only the bridged users its "@nick" mentions
     * actually resolved to: every parse_* flag stays off, so @everyone, a
     * role mention, and a raw "<@id>" typed on IRC all stay inert. */
    msg.set_allowed_mentions(false, false, false, false, pinged, {});

    /* The link between the IRC line and what Discord made of it, filled
     * in from the created message once it is known. A line without a
     * msgid (an IRCd without the msgid capability) is not linked. */
    Relay::Links::Entry link;
    link.irc_msgid = out.msgid.str();
    link.remote_thread = thread_id ? thread_id.str() : "";
    link.author = out.nick.str();
    link.excerpt = Text::TruncateCodePoints(out.text.str(), 120);
    auto mailbox = this->mailbox;
    const auto remember = [mailbox, link](const dpp::confirmation_callback_t &cb) {
      if (link.irc_msgid.empty() || cb.is_error())
        return;
      Relay::Links::Entry entry = link;
      try {
        const auto &created = cb.get<dpp::message>();
        entry.remote_id = created.id.str();
        entry.remote_channel = created.channel_id.str();
      } catch (const std::bad_variant_access &) {
        return;
      }
      mailbox->Post([entry](DiscordProtocol *protocol) {
        protocol->core->RememberLink(entry);
      });
    };

    if (!bridge->endpoint_id.empty()) {
      const Anope::string key = bridge->irc_channel;
      try {
        dpp::webhook hook(dpp::snowflake(bridge->endpoint_id.c_str()),
                          bridge->endpoint_token.str());
        hook.name =
            Text::WebhookName(out.nick.str(), this->webhook_suffix.str());

        /* wait=true makes Discord answer with the created message, which
         * is the only way to learn the id a webhook post was given. */
        this->cluster->execute_webhook(
            hook, msg, true, thread_id, "",
            [mailbox, key, remember](const dpp::confirmation_callback_t &cb) {
              if (!cb.is_error()) {
                remember(cb);
                return;
              }

              const auto status = cb.http_info.status;
              const std::string error = cb.get_error().human_readable;
              mailbox->Post([key, status, error](DiscordProtocol *protocol) {
                protocol->OnWebhookSendFailed(key, status, error);
              });
            });
        return;
      } catch (const dpp::exception &err) {
        Log(this->core->GetOwner())
            << "BridgeServ: unable to relay to the webhook for "
            << bridge->irc_channel << ": " << err.what();
      }
    }

    /* No usable webhook; fall back to the bot account and try to set a
     * webhook up for the next message. */
    dpp::message fallback(thread_id ? thread_id : channel,
                          quote + "<" +
                              dpp::utility::markdown_escape(out.nick.str()) +
                              "> " + text);
    fallback.set_allowed_mentions(false, false, false, false, pinged, {});
    try {
      this->cluster->message_create(fallback, remember);
    } catch (const dpp::exception &err) {
      Log(this->core->GetOwner()) << "BridgeServ: unable to relay to "
                                  << bridge->irc_channel << ": " << err.what();
    }
    this->EnsureWebhook(bridge);
  }

  void ListSpaces(const Anope::string &requester,
                  const Anope::string &svc) override {
    if (!this->cluster || !this->connected) {
      this->core->DeliverListing(requester, svc, false, true, {});
      return;
    }

    auto mailbox = this->mailbox;
    const Anope::string nick = requester;
    this->cluster->current_user_get_guilds(
        [mailbox, nick, svc](const dpp::confirmation_callback_t &cb) {
          bool failed = cb.is_error();
          std::vector<Anope::string> lines;
          if (!failed) {
            try {
              for (const auto &[id, guild] : cb.get<dpp::guild_map>())
                lines.emplace_back(Anope::string(guild.name) + " (" + id.str() +
                                   ")");
            } catch (const dpp::exception &) {
              failed = true;
            }
          }

          std::sort(lines.begin(), lines.end());
          mailbox->Post([nick, svc, failed, lines](DiscordProtocol *protocol) {
            protocol->core->DeliverListing(nick, svc, false, failed, lines);
          });
        });
  }

  void ListChannels(const Anope::string &space, const Anope::string &requester,
                    const Anope::string &svc) override {
    if (!this->cluster || !this->connected) {
      this->core->DeliverListing(requester, svc, true, true, {});
      return;
    }

    auto mailbox = this->mailbox;
    const Anope::string nick = requester;
    this->cluster->channels_get(
        dpp::snowflake(space.c_str()),
        [mailbox, nick, svc](const dpp::confirmation_callback_t &cb) {
          bool failed = cb.is_error();
          std::vector<Anope::string> lines;
          if (!failed) {
            try {
              for (const auto &[id, channel] : cb.get<dpp::channel_map>()) {
                if (channel.get_type() != dpp::CHANNEL_TEXT &&
                    channel.get_type() != dpp::CHANNEL_ANNOUNCEMENT)
                  continue;

                lines.emplace_back(Anope::string(channel.name) + " (" +
                                   id.str() + ")");
              }
            } catch (const dpp::exception &) {
              failed = true;
            }
          }

          std::sort(lines.begin(), lines.end());
          mailbox->Post([nick, svc, failed, lines](DiscordProtocol *protocol) {
            protocol->core->DeliverListing(nick, svc, true, failed, lines);
          });
        });
  }

  void OnNotify() override {
    for (;;) {
      Mailbox::Job job;
      if (!this->mailbox->Pop(job))
        break;

      job(this);
    }

    if (const auto dropped = this->mailbox->TakeDropped())
      Log(this->core->GetOwner())
          << "BridgeServ: dropped " << dropped
          << " queued Discord events; the bridge is falling behind.";
  }
};

/* A connection which never comes up cannot be recovered from here, and the
 * module deliberately does not try. Live running found all three halves of
 * the problem:
 *
 *   * DPP fetches the gateway shard count on its own HTTPS thread and lets
 *     a failure escape there ("Uncaught exception thrown in HTTPS callback
 *     for GET /api/v10/gateway/bot"), so start() below neither returns nor
 *     throws: nothing notices, and BridgeServ LIST reports
 *     "discord: offline" with no further log. A rate limited query is
 *     routine — it happens whenever the process restarts twice in quick
 *     succession — and an unauthorised one lands in the same place.
 *   * cluster::shutdown() does not make start(st_wait) return, so the dead
 *     cluster's thread never finishes and cannot be joined.
 *   * Starting a replacement cluster beside it puts two threads in DPP's
 *     JSON decoding at once, which segfaults on musl: the named
 *     std::locale that DPP's timestamp parser constructs is not thread
 *     safe there (SIGSEGV in strchr under std::locale::locale, reached
 *     from dpp::guild_member::fill_from_json on GUILD_CREATE).
 *
 * Only a fresh process recovers, so recovery belongs to whatever
 * supervises services (a container restart policy, systemd). The fix for
 * the first point belongs in DPP, which should retry or report the
 * shard-count fetch rather than dropping the exception.
 */
void DiscordThread::Run() {
  try {
    this->cluster->start(dpp::st_wait);
  } catch (const dpp::exception &err) {
    const std::string error = err.what();
    this->mailbox->Post(
        [error](DiscordProtocol *protocol) { protocol->OnStopped(error); });
  }
}

BridgeProtocol *CreateDiscordProtocol(BridgeCore *core) {
  return new DiscordProtocol(core);
}
