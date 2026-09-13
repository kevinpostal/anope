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

namespace Text = BridgeServ::Text;

class DiscordProtocol;

/* Work handed from DPP's thread pool back to the main thread. */
using Mailbox = BridgeServ::Async::Mailbox<DiscordProtocol>;

/** Checks whether a string is usable as a Discord snowflake. */
static bool ValidSnowflake(const Anope::string &value)
{
	if (value.empty() || value.length() > 20)
		return false;

	for (const auto chr : value)
	{
		if (chr < '0' || chr > '9')
			return false;
	}
	return Anope::TryConvert<uint64_t>(value).has_value();
}

/** Removes the control characters from a configured string. */
static std::string StripControl(const std::string &value)
{
	std::string out;
	out.reserve(value.length());
	for (const auto chr : value)
	{
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
class DiscordFilter final
{
	std::mutex mutex;

	/* Discord channel ids which are bridged. */
	std::unordered_set<std::string> bridged_channels;

	/* Webhook ids which this module created or adopted, so that the Discord
	 * thread can discard the messages that it sent itself. */
	std::unordered_set<std::string> own_webhook_ids;

	/* The Discord user id of the bot account, so that the Discord thread can
	 * discard the bot-account fallback messages that it sent itself. */
	std::string own_user_id;

public:
	bool IsBridged(const std::string &channel_id)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->bridged_channels.count(channel_id) > 0;
	}

	void SetBridged(std::unordered_set<std::string> channel_ids)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->bridged_channels = std::move(channel_ids);
	}

	bool IsOwnWebhook(const std::string &webhook_id)
	{
		if (webhook_id.empty() || webhook_id == "0")
			return false;

		std::lock_guard<std::mutex> lock(this->mutex);
		return this->own_webhook_ids.count(webhook_id) > 0;
	}

	void SetOwnWebhooks(std::unordered_set<std::string> webhook_ids)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->own_webhook_ids = std::move(webhook_ids);
	}

	void AddOwnWebhook(const std::string &webhook_id)
	{
		if (webhook_id.empty() || webhook_id == "0")
			return;

		std::lock_guard<std::mutex> lock(this->mutex);
		this->own_webhook_ids.insert(webhook_id);
	}

	void DelOwnWebhook(const std::string &webhook_id)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->own_webhook_ids.erase(webhook_id);
	}

	/** Determines whether a message was sent by the bot account itself. */
	bool IsSelf(const std::string &user_id)
	{
		if (user_id.empty() || user_id == "0")
			return false;

		std::lock_guard<std::mutex> lock(this->mutex);
		return this->own_user_id == user_id;
	}

	void SetSelf(const std::string &user_id)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->own_user_id = user_id;
	}
};

/** A thread running the shared DPP socket engine. */
class DiscordThread final
	: public Thread
{
	dpp::cluster *cluster;
	std::shared_ptr<Mailbox> mailbox;

public:
	DiscordThread(dpp::cluster *c, std::shared_ptr<Mailbox> m)
		: cluster(c)
		, mailbox(std::move(m))
	{
	}

	void Run() override;
};

class DiscordProtocol final
	: public BridgeProtocol
	, public Pipe
{
	dpp::cluster *cluster = nullptr;
	DiscordThread *thread = nullptr;
	std::shared_ptr<Mailbox> mailbox;
	std::shared_ptr<DiscordFilter> filter;

	Anope::string token;
	Anope::string domain;
	Anope::string webhook_name;
	Anope::string webhook_suffix;

	bool connected = false;

	/* ------------------------------------------------------------------ */
	/* Rendering (called on the Discord thread; touches no Anope state)   */
	/* ------------------------------------------------------------------ */

	/** Renders a Discord message as IRC-ready text. */
	static Anope::string RenderMessage(const dpp::message &msg)
	{
		/* Mentioned users come with their guild membership attached, which is
		 * the only place the per-guild nickname is available. */
		std::map<std::string, std::string> mentioned;
		for (const auto &[user, member] : msg.mentions)
		{
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
		auto resolve = [&mentioned](char kind, const std::string &id, const std::string &name) -> std::string
		{
			switch (kind)
			{
				case '@':
				{
					const auto it = mentioned.find(id);
					if (it != mentioned.end())
						return "@" + Text::EscapeMarkdown(it->second);

					if (const auto *user = dpp::find_user(dpp::snowflake(id)))
						return "@" + Text::EscapeMarkdown(user->global_name.empty() ? user->username : user->global_name);

					return "@unknown-user";
				}

				case '&':
				{
					if (const auto *role = dpp::find_role(dpp::snowflake(id)))
						return "@" + Text::EscapeMarkdown(role->name);
					return "@" + id;
				}

				case '#':
				{
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

		const auto render = [&resolve](const std::string &markdown)
		{
			return Text::MarkdownToIrc(Text::ExpandTokens(markdown, resolve));
		};

		std::string text = render(msg.content);

		if (!msg.attachments.empty())
		{
			const size_t shown = std::min<size_t>(msg.attachments.size(), 4);
			for (size_t idx = 0; idx < shown; ++idx)
			{
				const auto &attachment = msg.attachments[idx];
				text += " [" + attachment.filename + ": " + attachment.url + "]";
			}
			if (msg.attachments.size() > shown)
				text += " [+" + std::to_string(msg.attachments.size() - shown) + " more]";
		}

		for (const auto &sticker : msg.stickers)
			text += " [sticker: " + sticker.name + "]";

		/* An embed is only interesting when there is nothing else to show;
		 * most embeds are just an unfurled link which is in the content. */
		if (text.empty() && !msg.embeds.empty())
		{
			const auto &embed = msg.embeds.front();
			std::string summary = embed.title;
			if (!embed.description.empty())
				summary += (summary.empty() ? "" : " \xe2\x80\x94 ") + Text::TruncateCodePoints(embed.description, 300);

			const size_t shown = std::min<size_t>(embed.fields.size(), 3);
			for (size_t idx = 0; idx < shown; ++idx)
				summary += " [" + embed.fields[idx].name + ": " + Text::TruncateCodePoints(embed.fields[idx].value, 100) + "]";

			if (!summary.empty())
				text = "[embed] " + render(summary);
		}

		/* A forwarded message carries its content in a snapshot. */
		if (text.empty() && msg.has_snapshot())
		{
			const auto &forwarded = msg.message_snapshots.messages;
			const size_t shown = std::min<size_t>(forwarded.size(), 3);
			for (size_t idx = 0; idx < shown; ++idx)
			{
				const std::string content = render(forwarded[idx].content);
				if (content.empty())
					continue;
				text += (text.empty() ? "" : "\n") + std::string("(forwarded) ") + content;
			}
		}

		/* A forward references the original message too, but is not a reply
		 * to it. */
		if (!text.empty() && msg.message_reference.message_id && !msg.has_snapshot())
			text.insert(0, "(reply) ");

		/* Carriage returns and NULs can not be sent to IRC; newlines are
		 * handled by the line splitter when the message is relayed. */
		std::string clean;
		clean.reserve(text.length());
		for (const auto chr : text)
		{
			if (chr != '\r' && chr != '\0')
				clean += chr;
		}
		return clean;
	}

	/** Resolves the bridged channel a Discord channel maps to: itself, or
	 * its parent when it is a thread of a bridged channel.
	 * @return The bridged channel id, or an empty string if there is none.
	 */
	static std::string BridgedChannel(DiscordFilter &filter, dpp::snowflake channel_id)
	{
		const std::string id = channel_id.str();
		if (filter.IsBridged(id))
			return id;

		if (const auto *channel = dpp::find_channel(channel_id))
		{
			const std::string parent = channel->parent_id.str();
			if (channel->parent_id && filter.IsBridged(parent))
				return parent;
		}
		return "";
	}

	/** Handles an incoming Discord message on the Discord thread. */
	static void HandleMessage(const std::shared_ptr<DiscordFilter> &filter, const std::shared_ptr<Mailbox> &mailbox, const dpp::message &msg, bool edit)
	{
		const std::string channel_id = BridgedChannel(*filter, msg.channel_id);
		if (channel_id.empty())
			return;

		/* Messages which this module sent itself must not come back: either
		 * through one of its webhooks, or through the bot account when the
		 * webhook fallback is in use. Other bots are relayed as normal. */
		if (filter->IsOwnWebhook(msg.webhook_id.str()) || filter->IsSelf(msg.author.id.str()))
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

		std::string display = msg.member.get_nickname();
		if (display.empty())
			display = msg.author.global_name;
		if (display.empty())
			display = msg.author.username;
		if (display.empty())
			display = "discord";
		relay.display = display;

		mailbox->Post([relay](DiscordProtocol *protocol) { protocol->core->RelayToIrc(relay); });
	}

	/* ------------------------------------------------------------------ */
	/* Connection lifecycle                                               */
	/* ------------------------------------------------------------------ */

	void StartCluster()
	{
		if (this->cluster)
			return;

		if (this->token.empty())
		{
			Log(this->core->GetOwner()) << "BridgeServ: no Discord token is configured; the Discord link is disabled.";
			return;
		}

		/* A completion which was in flight when the previous cluster went
		 * away will never arrive, so nothing may still be waiting on one. */
		for (auto *bridge : this->core->GetBridges())
		{
			if (bridge->protocol.equals_ci(this->GetName()))
				bridge->endpoint_pending = false;
		}

		static constexpr uint32_t intents = dpp::i_guilds | dpp::i_guild_messages | dpp::i_message_content;
		this->cluster = new dpp::cluster(this->token.str(), intents);

		auto mailbox = this->mailbox;
		auto filter = this->filter;

		this->cluster->on_log([mailbox](const dpp::log_t &event)
		{
			if (event.severity < dpp::ll_warning)
				return;

			const std::string message = event.message;
			mailbox->Post([message](DiscordProtocol *protocol)
			{
				Log(protocol->core->GetOwner()) << "DPP: " << message;
			});
		});

		this->cluster->on_ready([mailbox, filter](const dpp::ready_t &event)
		{
			const auto guilds = event.guild_count;

			/* The bot account is only known once the gateway says hello. */
			if (event.owner)
				filter->SetSelf(event.owner->me.id.str());

			mailbox->Post([guilds](DiscordProtocol *protocol) { protocol->OnReady(guilds); });
		});

		this->cluster->on_message_create([mailbox, filter](const dpp::message_create_t &event)
		{
			HandleMessage(filter, mailbox, event.msg, false);
		});

		this->cluster->on_message_update([mailbox, filter](const dpp::message_update_t &event)
		{
			HandleMessage(filter, mailbox, event.msg, true);
		});

		this->cluster->on_message_delete([mailbox, filter](const dpp::message_delete_t &event)
		{
			const std::string channel_id = BridgedChannel(*filter, event.channel_id);
			if (channel_id.empty())
				return;

			BridgeMessage relay;
			relay.protocol = "discord";
			relay.channel = channel_id;
			relay.space = event.guild_id.str();
			relay.msg_id = event.id.str();
			relay.del = true;

			mailbox->Post([relay](DiscordProtocol *protocol) { protocol->core->RelayToIrc(relay); });
		});

		this->thread = new DiscordThread(this->cluster, this->mailbox);
		this->thread->Start();
	}

	void StopCluster()
	{
		this->connected = false;

		if (this->cluster)
			this->cluster->shutdown();

		if (this->thread)
		{
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

	void CreateWebhook(const Anope::string &key, const Anope::string &channel)
	{
		Bridge *bridge = this->core->FindIrc(key);
		if (!bridge)
			return; // the bridge was removed while the listing was in flight.

		/* The bridge was repointed while the listing was in flight; the
		 * pending guard is released so that the new channel is set up. */
		if (!bridge->foreign_channel.equals_ci(channel))
		{
			bridge->endpoint_pending = false;
			this->EnsureWebhook(bridge);
			return;
		}

		if (!this->cluster || !this->connected)
		{
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
		try
		{
			this->cluster->create_webhook(hook, [mailbox, key, channel](const dpp::confirmation_callback_t &cb)
			{
				if (cb.is_error())
				{
					const std::string error = cb.get_error().human_readable;
					mailbox->Post([key, error](DiscordProtocol *protocol) { protocol->OnWebhookFailed(key, error); });
					return;
				}

				std::string id;
				std::string tok;
				try
				{
					const auto created = cb.get<dpp::webhook>();
					id = created.id.str();
					tok = created.token;
				}
				catch (const dpp::exception &)
				{
					mailbox->Post([key](DiscordProtocol *protocol) { protocol->OnWebhookFailed(key, "malformed webhook response"); });
					return;
				}

				mailbox->Post([key, channel, id, tok](DiscordProtocol *protocol) { protocol->OnWebhookReady(key, channel, id, tok, true); });
			});
		}
		catch (const dpp::exception &err)
		{
			this->OnWebhookFailed(key, err.what());
		}
	}

	void OnWebhookReady(const Anope::string &key, const Anope::string &channel, const std::string &id, const std::string &tok, bool created)
	{
		Bridge *bridge = this->core->FindIrc(key);
		if (!bridge)
			return;

		/* The bridge may have been repointed while the lookup was in flight;
		 * adopting the old channel's webhook would send IRC traffic to the
		 * wrong Discord channel. */
		if (!bridge->foreign_channel.equals_ci(channel))
		{
			bridge->endpoint_pending = false;
			Log(this->core->GetOwner()) << "BridgeServ: discarding a webhook for " << key << " which no longer points at " << channel;
			this->EnsureWebhook(bridge);
			return;
		}

		bridge->endpoint_pending = false;
		bridge->endpoint_retry_at = 0;
		bridge->endpoint_id = id;
		bridge->endpoint_token = tok;
		this->core->SaveBridge(bridge);

		this->filter->AddOwnWebhook(id);
		Log(this->core->GetOwner()) << "BridgeServ: " << (created ? "created" : "adopted") << " Discord webhook " << id << " for " << key;
	}

	void OnWebhookFailed(const Anope::string &key, const std::string &error)
	{
		Bridge *bridge = this->core->FindIrc(key);
		if (!bridge)
			return;

		bridge->endpoint_pending = false;
		bridge->endpoint_retry_at = Anope::CurTime + 60;
		Log(this->core->GetOwner()) << "BridgeServ: webhook setup for " << key << " failed: " << error;
	}

	void OnWebhookSendFailed(const Anope::string &key, uint16_t status, const std::string &error)
	{
		Bridge *bridge = this->core->FindIrc(key);
		if (!bridge)
			return;

		/* The webhook was deleted on the Discord side or its token was
		 * revoked; drop it and set a new one up. */
		if (status == 401 || status == 403 || status == 404)
		{
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
			bridge->endpoint_retry_at = bridge->endpoint_failures > 1 ? Anope::CurTime + 60 : 0;
			this->core->SaveBridge(bridge);

			Log(this->core->GetOwner()) << "BridgeServ: the webhook for " << key << " is no longer usable (" << status << "); a new one will be created.";
			this->EnsureWebhook(bridge);
			return;
		}
		Log(this->core->GetOwner()) << "BridgeServ: relaying to the webhook for " << key << " failed: " << error;
	}

	void EnsureWebhook(Bridge *bridge)
	{
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
		try
		{
			this->cluster->get_channel_webhooks(dpp::snowflake(channel.c_str()), [mailbox, key, channel, wanted](const dpp::confirmation_callback_t &cb)
			{
				if (cb.is_error())
				{
					const std::string error = cb.get_error().human_readable;
					mailbox->Post([key, error](DiscordProtocol *protocol) { protocol->OnWebhookFailed(key, error); });
					return;
				}

				std::string id;
				std::string tok;
				try
				{
					for (const auto &[hook_id, hook] : cb.get<dpp::webhook_map>())
					{
						if (hook.name != wanted || hook.token.empty())
							continue;

						id = hook_id.str();
						tok = hook.token;
						break;
					}
				}
				catch (const dpp::exception &)
				{
					mailbox->Post([key](DiscordProtocol *protocol) { protocol->OnWebhookFailed(key, "malformed webhook list response"); });
					return;
				}

				if (id.empty())
				{
					mailbox->Post([key, channel](DiscordProtocol *protocol) { protocol->CreateWebhook(key, channel); });
					return;
				}
				mailbox->Post([key, channel, id, tok](DiscordProtocol *protocol) { protocol->OnWebhookReady(key, channel, id, tok, false); });
			});
		}
		catch (const dpp::exception &err)
		{
			this->OnWebhookFailed(key, err.what());
		}
	}

	void OnReady(uint32_t guilds)
	{
		this->connected = true;
		Log(this->core->GetOwner()) << "BridgeServ: connected to Discord (" << guilds << " guild(s)).";

		/* Ready fires again after a reconnect; webhook setup is idempotent,
		 * and a fresh connection is not made to wait out a backoff that was
		 * earned on the old one. */
		for (auto *bridge : this->core->GetBridges())
		{
			if (!bridge->protocol.equals_ci(this->GetName()))
				continue;

			bridge->endpoint_retry_at = 0;
			this->EnsureWebhook(bridge);
		}
	}

	void OnStopped(const std::string &error)
	{
		this->connected = false;
		Log(this->core->GetOwner()) << "BridgeServ: the Discord connection stopped: " << error;
	}

	friend class DiscordThread;

public:
	explicit DiscordProtocol(BridgeCore *c)
		: BridgeProtocol("discord", c)
		, mailbox(std::make_shared<Mailbox>(this, [this] { this->Notify(); }))
		, filter(std::make_shared<DiscordFilter>())
	{
	}

	~DiscordProtocol() override
	{
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

	bool IsValidId(const Anope::string &id) const override { return ValidSnowflake(id); }

	void Configure(Configuration::Block &block) override
	{
		Anope::string domain_conf = block.Get<const Anope::string>("domain", "discord.bridge");
		if (IRCD && !IRCD->IsHostValid(domain_conf))
		{
			Log(this->core->GetOwner()) << "BridgeServ: domain " << domain_conf << " is not a valid hostname; using discord.bridge";
			domain_conf = "discord.bridge";
		}
		this->domain = domain_conf;

		/* Discord rejects control characters in webhook names and limits
		 * them to 80 characters. */
		Anope::string name = Text::TruncateCodePoints(StripControl(block.Get<const Anope::string>("bridgename", "IRC Bridge").str()), 80);
		if (name.empty())
		{
			Log(this->core->GetOwner()) << "BridgeServ: bridgename is empty; using \"IRC Bridge\".";
			name = "IRC Bridge";
		}
		const bool name_changed = !this->webhook_name.empty() && this->webhook_name != name;
		this->webhook_name = name;
		this->webhook_suffix = StripControl(block.Get<const Anope::string>("webhooksuffix", " (IRC)").str());

		const Anope::string new_token = block.Get<const Anope::string>("token");
		if (new_token != this->token || !this->cluster)
		{
			this->StopCluster();
			this->token = new_token;
			this->StartCluster();
		}

		/* Existing webhooks carry the old name; they are replaced so that
		 * the name is what the operator configured everywhere. */
		if (name_changed && this->cluster && this->connected)
		{
			for (auto *bridge : this->core->GetBridges())
			{
				if (!bridge->protocol.equals_ci(this->GetName()) || bridge->endpoint_id.empty())
					continue;

				this->OnBridgeRemoved(bridge);
				this->core->SaveBridge(bridge);
				this->EnsureWebhook(bridge);
			}
		}
	}

	void OnBridgesChanged() override
	{
		std::unordered_set<std::string> channels;
		std::unordered_set<std::string> webhooks;
		for (const auto *bridge : this->core->GetBridges())
		{
			if (!bridge->protocol.equals_ci(this->GetName()))
				continue;

			channels.insert(bridge->foreign_channel.str());
			if (!bridge->endpoint_id.empty())
				webhooks.insert(bridge->endpoint_id.str());
		}

		this->filter->SetBridged(std::move(channels));
		this->filter->SetOwnWebhooks(std::move(webhooks));

		if (!this->connected)
			return;

		for (auto *bridge : this->core->GetBridges())
		{
			if (bridge->protocol.equals_ci(this->GetName()))
				this->EnsureWebhook(bridge);
		}
	}

	void OnBridgeRemoved(Bridge *bridge) override
	{
		if (bridge->endpoint_id.empty())
			return;

		const std::string id = bridge->endpoint_id.str();
		this->filter->DelOwnWebhook(id);

		if (this->cluster && this->connected)
		{
			auto mailbox = this->mailbox;
			const Anope::string key = bridge->irc_channel;
			try
			{
				this->cluster->delete_webhook(dpp::snowflake(bridge->endpoint_id.c_str()), [mailbox, key](const dpp::confirmation_callback_t &cb)
				{
					if (!cb.is_error())
						return;

					const std::string error = cb.get_error().human_readable;
					mailbox->Post([key, error](DiscordProtocol *protocol)
					{
						Log(protocol->core->GetOwner()) << "BridgeServ: unable to delete the webhook for " << key << ": " << error;
					});
				});
			}
			catch (const dpp::exception &err)
			{
				Log(this->core->GetOwner()) << "BridgeServ: unable to delete the webhook for " << key << ": " << err.what();
			}
		}

		bridge->endpoint_id.clear();
		bridge->endpoint_token.clear();
		bridge->endpoint_pending = false;
		bridge->endpoint_retry_at = 0;
	}

	void Relay(Bridge *bridge, const BridgeOutbound &out) override
	{
		if (!this->cluster || !this->connected)
			return;

		std::string text = Text::EscapeLineStart(dpp::utility::markdown_escape(out.text.str(), true));

		/* Discord rejects messages longer than 2000 characters. The body is
		 * truncated before the italic markers are added so that an oversized
		 * action does not lose its closing marker. */
		text = Text::TruncateCodePoints(text, out.action ? 1998 : 2000);

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
		dpp::message msg(channel, text);

		/* Messages relayed from IRC never ping anybody; the wire payload gets
		 * an empty allowed_mentions.parse list. */
		msg.set_allowed_mentions(false, false, false, false);

		if (!bridge->endpoint_id.empty())
		{
			auto mailbox = this->mailbox;
			const Anope::string key = bridge->irc_channel;
			try
			{
				dpp::webhook hook(dpp::snowflake(bridge->endpoint_id.c_str()), bridge->endpoint_token.str());
				hook.name = Text::WebhookName(out.nick.str(), this->webhook_suffix.str());

				this->cluster->execute_webhook(hook, msg, false, 0, "", [mailbox, key](const dpp::confirmation_callback_t &cb)
				{
					if (!cb.is_error())
						return;

					const auto status = cb.http_info.status;
					const std::string error = cb.get_error().human_readable;
					mailbox->Post([key, status, error](DiscordProtocol *protocol)
					{
						protocol->OnWebhookSendFailed(key, status, error);
					});
				});
				return;
			}
			catch (const dpp::exception &err)
			{
				Log(this->core->GetOwner()) << "BridgeServ: unable to relay to the webhook for " << bridge->irc_channel << ": " << err.what();
			}
		}

		/* No usable webhook; fall back to the bot account and try to set a
		 * webhook up for the next message. */
		dpp::message fallback(channel, "<" + dpp::utility::markdown_escape(out.nick.str()) + "> " + text);
		fallback.set_allowed_mentions(false, false, false, false);
		try
		{
			this->cluster->message_create(fallback);
		}
		catch (const dpp::exception &err)
		{
			Log(this->core->GetOwner()) << "BridgeServ: unable to relay to " << bridge->irc_channel << ": " << err.what();
		}
		this->EnsureWebhook(bridge);
	}

	void ListSpaces(const Anope::string &requester, const Anope::string &svc) override
	{
		if (!this->cluster || !this->connected)
		{
			this->core->DeliverListing(requester, svc, false, true, { });
			return;
		}

		auto mailbox = this->mailbox;
		const Anope::string nick = requester;
		this->cluster->current_user_get_guilds([mailbox, nick, svc](const dpp::confirmation_callback_t &cb)
		{
			bool failed = cb.is_error();
			std::vector<Anope::string> lines;
			if (!failed)
			{
				try
				{
					for (const auto &[id, guild] : cb.get<dpp::guild_map>())
						lines.emplace_back(Anope::string(guild.name) + " (" + id.str() + ")");
				}
				catch (const dpp::exception &)
				{
					failed = true;
				}
			}

			std::sort(lines.begin(), lines.end());
			mailbox->Post([nick, svc, failed, lines](DiscordProtocol *protocol)
			{
				protocol->core->DeliverListing(nick, svc, false, failed, lines);
			});
		});
	}

	void ListChannels(const Anope::string &space, const Anope::string &requester, const Anope::string &svc) override
	{
		if (!this->cluster || !this->connected)
		{
			this->core->DeliverListing(requester, svc, true, true, { });
			return;
		}

		auto mailbox = this->mailbox;
		const Anope::string nick = requester;
		this->cluster->channels_get(dpp::snowflake(space.c_str()), [mailbox, nick, svc](const dpp::confirmation_callback_t &cb)
		{
			bool failed = cb.is_error();
			std::vector<Anope::string> lines;
			if (!failed)
			{
				try
				{
					for (const auto &[id, channel] : cb.get<dpp::channel_map>())
					{
						if (channel.get_type() != dpp::CHANNEL_TEXT && channel.get_type() != dpp::CHANNEL_ANNOUNCEMENT)
							continue;

						lines.emplace_back(Anope::string(channel.name) + " (" + id.str() + ")");
					}
				}
				catch (const dpp::exception &)
				{
					failed = true;
				}
			}

			std::sort(lines.begin(), lines.end());
			mailbox->Post([nick, svc, failed, lines](DiscordProtocol *protocol)
			{
				protocol->core->DeliverListing(nick, svc, true, failed, lines);
			});
		});
	}

	void OnNotify() override
	{
		for (;;)
		{
			Mailbox::Job job;
			if (!this->mailbox->Pop(job))
				break;

			job(this);
		}

		if (const auto dropped = this->mailbox->TakeDropped())
			Log(this->core->GetOwner()) << "BridgeServ: dropped " << dropped << " queued Discord events; the bridge is falling behind.";
	}
};

void DiscordThread::Run()
{
	try
	{
		this->cluster->start(dpp::st_wait);
	}
	catch (const dpp::exception &err)
	{
		const std::string error = err.what();
		this->mailbox->Post([error](DiscordProtocol *protocol) { protocol->OnStopped(error); });
	}
}

BridgeProtocol *CreateDiscordProtocol(BridgeCore *core)
{
	return new DiscordProtocol(core);
}
