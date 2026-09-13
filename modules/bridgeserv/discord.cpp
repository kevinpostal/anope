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
#include "modules/bridgeserv/render.h"

#include <dpp/dpp.h>
#include <dpp/intents.h>
#include <dpp/webhook.h>

#include <algorithm>
#include <ctime>
#include <deque>
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

/** State which is shared between Anope's main thread and DPP's thread pool.
 *
 * DPP dispatches gateway events and REST completions on its own thread pool
 * and cluster::shutdown() does not cancel work which is already in flight, so
 * a callback can fire at any point during (and after) module unload.
 * Callbacks therefore never capture the protocol; they capture a shared_ptr to
 * this object and hand work back to the main thread with Post(). The protocol
 * clears the owner in its destructor, after which a late callback is a no-op.
 */
class DiscordState final
{
public:
	using Job = std::function<void(DiscordProtocol *)>;

private:
	/* The maximum number of queued jobs before the oldest is dropped. */
	static constexpr size_t MAX_JOBS = 4096;

	std::mutex mutex;
	DiscordProtocol *owner;
	Pipe *pipe;
	std::deque<Job> jobs;
	size_t dropped = 0;

	/* Discord channel ids which are bridged, so that the Discord thread can
	 * discard traffic for other channels without waking the main thread. */
	std::unordered_set<std::string> bridged_channels;

	/* Webhook ids which this module created or adopted, so that the Discord
	 * thread can discard the messages that it sent itself. */
	std::unordered_set<std::string> own_webhook_ids;

	/* The Discord user id of the bot account, so that the Discord thread can
	 * discard the bot-account fallback messages that it sent itself. */
	std::string own_user_id;

public:
	DiscordState(DiscordProtocol *protocol, Pipe *notifier)
		: owner(protocol)
		, pipe(notifier)
	{
	}

	/** Queues a job to run on the main thread. Safe to call from any thread. */
	void Post(Job job)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		if (!this->owner || !this->pipe)
			return;

		if (this->jobs.size() >= MAX_JOBS)
		{
			this->jobs.pop_front();
			++this->dropped;
		}
		this->jobs.push_back(std::move(job));
		this->pipe->Notify();
	}

	/** Takes the next queued job, if there is one. */
	bool PopJob(Job &job)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		if (this->jobs.empty())
			return false;

		job = std::move(this->jobs.front());
		this->jobs.pop_front();
		return true;
	}

	/** Takes and resets the number of jobs dropped from the queue. */
	size_t TakeDropped()
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return std::exchange(this->dropped, 0);
	}

	/** Detaches the protocol; all later callbacks become no-ops. */
	void Detach()
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->owner = nullptr;
		this->pipe = nullptr;
		this->jobs.clear();
	}

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
	std::shared_ptr<DiscordState> state;

public:
	DiscordThread(dpp::cluster *c, std::shared_ptr<DiscordState> s)
		: cluster(c)
		, state(std::move(s))
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
	std::shared_ptr<DiscordState> state;

	Anope::string token;
	Anope::string domain;
	Anope::string webhook_name;
	Anope::string webhook_suffix;

	bool connected = false;

	/* ------------------------------------------------------------------ */
	/* Rendering (called on the Discord thread; touches no Anope state)   */
	/* ------------------------------------------------------------------ */

	/** Formats a Discord timestamp token as a UTC time. */
	static std::string FormatTimestamp(const std::string &value)
	{
		const auto when = Anope::TryConvert<time_t>(value);
		if (!when.has_value())
			return "";

		const std::time_t raw = *when;
		std::tm parts = { };
#ifdef _WIN32
		if (gmtime_s(&parts, &raw))
			return "";
#else
		if (!gmtime_r(&raw, &parts))
			return "";
#endif

		char buf[32];
		if (!std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &parts))
			return "";
		return buf;
	}

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
					return FormatTimestamp(id);
			}
			return "";
		};

		std::string text = Text::MarkdownToIrc(Text::ExpandTokens(msg.content, resolve));

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
		if (text.empty() && !msg.embeds.empty() && !msg.embeds.front().title.empty())
			text = "[embed: " + msg.embeds.front().title + "]";

		if (!text.empty() && msg.message_reference.message_id)
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

	/** Handles an incoming Discord message on the Discord thread. */
	static void HandleMessage(const std::shared_ptr<DiscordState> &state, const dpp::message &msg, bool edit)
	{
		const std::string channel_id = msg.channel_id.str();
		if (!state->IsBridged(channel_id))
			return;

		/* Messages which this module sent itself must not come back: either
		 * through one of its webhooks, or through the bot account when the
		 * webhook fallback is in use. Other bots are relayed as normal. */
		if (state->IsOwnWebhook(msg.webhook_id.str()) || state->IsSelf(msg.author.id.str()))
			return;

		BridgeMessage relay;
		relay.text = RenderMessage(msg);
		if (relay.text.empty())
			return;

		relay.protocol = "discord";
		relay.channel = channel_id;
		relay.guild = msg.guild_id.str();
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

		state->Post([relay](DiscordProtocol *protocol) { protocol->core->RelayToIrc(relay); });
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

		static constexpr uint32_t intents = dpp::i_guilds | dpp::i_guild_messages | dpp::i_message_content;
		this->cluster = new dpp::cluster(this->token.str(), intents);

		auto state = this->state;

		this->cluster->on_log([state](const dpp::log_t &event)
		{
			if (event.severity < dpp::ll_warning)
				return;

			const std::string message = event.message;
			state->Post([message](DiscordProtocol *protocol)
			{
				Log(protocol->core->GetOwner()) << "DPP: " << message;
			});
		});

		this->cluster->on_ready([state](const dpp::ready_t &event)
		{
			const auto guilds = event.guild_count;

			/* The bot account is only known once the gateway says hello. */
			if (event.owner)
				state->SetSelf(event.owner->me.id.str());

			state->Post([guilds](DiscordProtocol *protocol) { protocol->OnReady(guilds); });
		});

		this->cluster->on_message_create([state](const dpp::message_create_t &event)
		{
			HandleMessage(state, event.msg, false);
		});

		this->cluster->on_message_update([state](const dpp::message_update_t &event)
		{
			HandleMessage(state, event.msg, true);
		});

		this->cluster->on_message_delete([state](const dpp::message_delete_t &event)
		{
			const std::string channel_id = event.channel_id.str();
			if (!state->IsBridged(channel_id))
				return;

			BridgeMessage relay;
			relay.protocol = "discord";
			relay.channel = channel_id;
			relay.guild = event.guild_id.str();
			relay.msg_id = event.id.str();
			relay.del = true;

			state->Post([relay](DiscordProtocol *protocol) { protocol->core->RelayToIrc(relay); });
		});

		this->thread = new DiscordThread(this->cluster, this->state);
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

		auto state = this->state;
		try
		{
			this->cluster->create_webhook(hook, [state, key, channel](const dpp::confirmation_callback_t &cb)
			{
				if (cb.is_error())
				{
					const std::string error = cb.get_error().human_readable;
					state->Post([key, error](DiscordProtocol *protocol) { protocol->OnWebhookFailed(key, error); });
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
					state->Post([key](DiscordProtocol *protocol) { protocol->OnWebhookFailed(key, "malformed webhook response"); });
					return;
				}

				state->Post([key, channel, id, tok](DiscordProtocol *protocol) { protocol->OnWebhookReady(key, channel, id, tok, true); });
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

		this->state->AddOwnWebhook(id);
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

			this->state->DelOwnWebhook(bridge->endpoint_id.str());
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

		auto state = this->state;
		const Anope::string key = bridge->irc_channel;
		const Anope::string channel = bridge->foreign_channel;
		const std::string wanted = this->webhook_name.str();

		/* Reuse the webhook from a previous run rather than creating a new one
		 * on every load, which would litter the channel with dead webhooks. */
		try
		{
			this->cluster->get_channel_webhooks(dpp::snowflake(channel.c_str()), [state, key, channel, wanted](const dpp::confirmation_callback_t &cb)
			{
				if (cb.is_error())
				{
					const std::string error = cb.get_error().human_readable;
					state->Post([key, error](DiscordProtocol *protocol) { protocol->OnWebhookFailed(key, error); });
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
					state->Post([key](DiscordProtocol *protocol) { protocol->OnWebhookFailed(key, "malformed webhook list response"); });
					return;
				}

				if (id.empty())
				{
					state->Post([key, channel](DiscordProtocol *protocol) { protocol->CreateWebhook(key, channel); });
					return;
				}
				state->Post([key, channel, id, tok](DiscordProtocol *protocol) { protocol->OnWebhookReady(key, channel, id, tok, false); });
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

		/* Ready fires again after a reconnect; webhook setup is idempotent. */
		for (auto *bridge : this->core->GetBridges())
		{
			if (bridge->protocol.equals_ci(this->GetName()))
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
		, state(std::make_shared<DiscordState>(this, this))
	{
	}

	~DiscordProtocol() override
	{
		/* Detach first: any Discord callback which is already in flight must
		 * not be able to reach this object once it starts being destroyed. */
		this->state->Detach();
		this->StopCluster();
	}

	const Anope::string &GetDomain() const override { return this->domain; }

	bool IsConnected() const override { return this->connected; }

	bool IsValidId(const Anope::string &id) const override { return ValidSnowflake(id); }

	void Configure(Configuration::Block &block) override
	{
		this->domain = block.Get<const Anope::string>("domain", "discord.bridge");
		this->webhook_name = block.Get<const Anope::string>("bridgename", "IRC Bridge");
		this->webhook_suffix = block.Get<const Anope::string>("webhooksuffix", " (IRC)");

		const Anope::string new_token = block.Get<const Anope::string>("token");
		if (new_token != this->token || !this->cluster)
		{
			this->StopCluster();
			this->token = new_token;
			this->StartCluster();
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

		this->state->SetBridged(std::move(channels));
		this->state->SetOwnWebhooks(std::move(webhooks));

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
		this->state->DelOwnWebhook(id);

		if (this->cluster && this->connected)
		{
			auto state = this->state;
			const Anope::string key = bridge->irc_channel;
			try
			{
				this->cluster->delete_webhook(dpp::snowflake(bridge->endpoint_id.c_str()), [state, key](const dpp::confirmation_callback_t &cb)
				{
					if (!cb.is_error())
						return;

					const std::string error = cb.get_error().human_readable;
					state->Post([key, error](DiscordProtocol *protocol)
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

		std::string text = dpp::utility::markdown_escape(out.text.str(), true);

		/* Discord rejects messages longer than 2000 characters. The body is
		 * truncated before the italic markers are added so that an oversized
		 * action does not lose its closing marker. */
		text = Text::TruncateUtf8(text, out.action ? 1998 : 2000);
		if (text.empty())
			return;

		/* A trailing escape would escape the closing marker instead. */
		if (out.action)
		{
			size_t slashes = 0;
			while (slashes < text.length() && text[text.length() - 1 - slashes] == '\\')
				++slashes;
			if (slashes % 2)
				text.erase(text.length() - 1);

			text = "*" + text + "*";
		}

		const dpp::snowflake channel(bridge->foreign_channel.c_str());
		dpp::message msg(channel, text);

		/* Messages relayed from IRC never ping anybody; the wire payload gets
		 * an empty allowed_mentions.parse list. */
		msg.set_allowed_mentions(false, false, false, false);

		if (!bridge->endpoint_id.empty())
		{
			auto state = this->state;
			const Anope::string key = bridge->irc_channel;
			try
			{
				dpp::webhook hook(dpp::snowflake(bridge->endpoint_id.c_str()), bridge->endpoint_token.str());
				hook.name = Text::WebhookName(out.nick.str(), this->webhook_suffix.str());

				this->cluster->execute_webhook(hook, msg, false, 0, "", [state, key](const dpp::confirmation_callback_t &cb)
				{
					if (!cb.is_error())
						return;

					const auto status = cb.http_info.status;
					const std::string error = cb.get_error().human_readable;
					state->Post([key, status, error](DiscordProtocol *protocol)
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
		dpp::message fallback(channel, "<" + out.nick.str() + "> " + text);
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

	void ListGuilds(const Anope::string &requester, const Anope::string &svc) override
	{
		if (!this->cluster || !this->connected)
		{
			this->core->DeliverListing(requester, svc, false, true, { });
			return;
		}

		auto state = this->state;
		const Anope::string nick = requester;
		this->cluster->current_user_get_guilds([state, nick, svc](const dpp::confirmation_callback_t &cb)
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
			state->Post([nick, svc, failed, lines](DiscordProtocol *protocol)
			{
				protocol->core->DeliverListing(nick, svc, false, failed, lines);
			});
		});
	}

	void ListChannels(const Anope::string &guild, const Anope::string &requester, const Anope::string &svc) override
	{
		if (!this->cluster || !this->connected)
		{
			this->core->DeliverListing(requester, svc, true, true, { });
			return;
		}

		auto state = this->state;
		const Anope::string nick = requester;
		this->cluster->channels_get(dpp::snowflake(guild.c_str()), [state, nick, svc](const dpp::confirmation_callback_t &cb)
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
			state->Post([nick, svc, failed, lines](DiscordProtocol *protocol)
			{
				protocol->core->DeliverListing(nick, svc, true, failed, lines);
			});
		});
	}

	void OnNotify() override
	{
		for (;;)
		{
			DiscordState::Job job;
			if (!this->state->PopJob(job))
				break;

			job(this);
		}

		if (const auto dropped = this->state->TakeDropped())
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
		this->state->Post([error](DiscordProtocol *protocol) { protocol->OnStopped(error); });
	}
}

BridgeProtocol *CreateDiscordProtocol(BridgeCore *core)
{
	return new DiscordProtocol(core);
}
