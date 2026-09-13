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

/// BEGIN CMAKE
/// # This module uses the C++ Discord library (DPP) which is vendored in
/// # vendor/brainboxdotcc. DPP requires OpenSSL (for its secure websocket
/// # connections) and zlib (for compressed gateway traffic).
/// find_package(OpenSSL REQUIRED)
/// find_package(ZLIB REQUIRED)
/// target_compile_definitions(vendored_brainboxdotcc PRIVATE DPP_BUILD DPP_NO_CORO)
/// target_include_directories(vendored_brainboxdotcc PUBLIC ${Anope_SOURCE_DIR}/vendor/brainboxdotcc/dpp/include ${Anope_SOURCE_DIR}/vendor/brainboxdotcc/dpp/include/dpp)
/// target_link_libraries(vendored_brainboxdotcc PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
/// target_link_libraries(${SO} PRIVATE vendored_brainboxdotcc OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
/// END CMAKE

#include "module.h"
#include "convert.h"
#include "modules/bridgeserv/render.h"

#include <openssl/sha.h>

#include <dpp/dpp.h>
#include <dpp/intents.h>
#include <dpp/webhook.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Text = BridgeServ::Text;

class ModuleBridgeServ;

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

/** A message which the Discord thread has rendered for relaying to IRC. */
struct RelayJob final
{
	Anope::string channel_id;
	Anope::string guild_id;
	Anope::string discord_id;
	Anope::string display;
	Anope::string text;
	Anope::string msg_id;
	bool edit = false;
	bool del = false;
};

/** State which is shared between Anope's main thread and DPP's thread pool.
 *
 * DPP dispatches gateway events and REST completions on its own thread pool
 * and cluster::shutdown() does not cancel work which is already in flight, so
 * a callback can fire at any point during (and after) module unload. Callbacks
 * therefore never capture the module; they capture a shared_ptr to this object
 * and hand work back to the main thread with Post(). The module clears the
 * owner in its destructor, after which any late callback becomes a no-op.
 */
class BridgeState final
{
public:
	using Job = std::function<void(ModuleBridgeServ *)>;

private:
	/* The maximum number of queued jobs before the oldest is dropped. */
	static constexpr size_t MAX_JOBS = 4096;

	std::mutex mutex;
	ModuleBridgeServ *owner;
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
	BridgeState(ModuleBridgeServ *module, Pipe *notifier)
		: owner(module)
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

	/** Detaches the module; all later callbacks become no-ops. */
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
class BridgeThread final
	: public Thread
{
	dpp::cluster *cluster;
	std::shared_ptr<BridgeState> state;

public:
	BridgeThread(dpp::cluster *c, std::shared_ptr<BridgeState> s)
		: cluster(c)
		, state(std::move(s))
	{
	}

	void Run() override;
};

/** A Discord user represented as an IRC pseudo client.
 *
 * One DiscordUser exists per Discord user regardless of how many bridges the
 * user is active in. The nick is the sanitised Discord display name (or
 * username), and the hostmask is:
 *   <nick>!<uid-sha256>@<bridge server>
 * where uid-sha256 is a prefix of the SHA-256 hash of the Discord snowflake
 * UID, making the identity unique and stable.
 */
class DiscordUser final
{
public:
	Anope::string discord_id;
	User *user = nullptr;
	std::set<Anope::string> chans;
	time_t last_active = Anope::CurTime;
};

/** A bridge between an IRC channel and a channel in a Discord guild. */
class Bridge final
	: public Serializable
{
public:
	Anope::string irc_channel;
	Anope::string guild;
	Anope::string foreign_channel;

	/* Webhook for sending messages with per-IRC-user identity. */
	Anope::string webhook_id;
	Anope::string webhook_token;

	/* Guard against concurrent webhook setup for the same bridge. */
	bool webhook_pending = false;

	/* The time at which webhook setup may be attempted again. */
	time_t webhook_retry_at = 0;

	/* When the webhook of this bridge last failed to deliver, and how many
	 * times in a row, so that a webhook which keeps failing backs off
	 * instead of causing a REST round trip per relayed message. */
	time_t webhook_failed_at = 0;
	unsigned webhook_failures = 0;

	/* Token bucket which throttles Discord to IRC relaying. */
	unsigned tokens = 0;
	time_t tokens_at = 0;
	unsigned throttled = 0;

	Bridge()
		: Serializable("Bridge")
	{
	}

	dpp::snowflake ForeignId() const
	{
		return dpp::snowflake(this->foreign_channel.c_str());
	}
};

class BridgeType final
	: public Serialize::Type
{
	ModuleBridgeServ *module;

public:
	explicit BridgeType(ModuleBridgeServ *creator);

	void Serialize(Serializable *obj, Serialize::Data &data) const override;
	Serializable *Unserialize(Serializable *obj, Serialize::Data &data) const override;
};

/** Quits pseudo clients which have been idle for too long. */
class BridgeReapTimer final
	: public Timer
{
	ModuleBridgeServ *module;

public:
	explicit BridgeReapTimer(ModuleBridgeServ *creator);

	bool Tick() override;
};

class CommandBSAdd final
	: public Command
{
	ModuleBridgeServ *module;

public:
	explicit CommandBSAdd(ModuleBridgeServ *creator);

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) override;
	bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSSet final
	: public Command
{
	ModuleBridgeServ *module;

public:
	explicit CommandBSSet(ModuleBridgeServ *creator);

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) override;
	bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSDel final
	: public Command
{
	ModuleBridgeServ *module;

public:
	explicit CommandBSDel(ModuleBridgeServ *creator);

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) override;
	bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSList final
	: public Command
{
	ModuleBridgeServ *module;

public:
	explicit CommandBSList(ModuleBridgeServ *creator);

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) override;
	bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSGuilds final
	: public Command
{
	ModuleBridgeServ *module;

public:
	explicit CommandBSGuilds(ModuleBridgeServ *creator);

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) override;
	bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class CommandBSChannels final
	: public Command
{
	ModuleBridgeServ *module;

public:
	explicit CommandBSChannels(ModuleBridgeServ *creator);

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) override;
	bool OnHelp(CommandSource &source, const Anope::string &subcommand) override;
};

class ModuleBridgeServ final
	: public Module
	, public Pipe
{
	BridgeType *btype = nullptr;
	dpp::cluster *core = nullptr;
	BridgeThread *thread = nullptr;
	std::shared_ptr<BridgeState> state;

	Anope::string token;
	Anope::string server_name;
	Anope::string client_name;
	Anope::string bridge_name;
	Anope::string webhook_suffix;
	time_t user_idle = 3600;
	size_t max_lines = 8;
	unsigned flood_lines = 6;
	time_t flood_secs = 4;
	bool relay_edits = true;
	bool relay_deletes = false;

	bool connected = false;
	Server *discord_server = nullptr;

	std::vector<Bridge *> bridges;
	std::map<Anope::string, DiscordUser *> discord_users;

	/* What was last relayed for a message, keyed by "<channel id>/<message
	 * id>": the hash covers the whole rendering so that an edit beyond the
	 * preview is still seen as a change, and the preview is what the delete
	 * notice quotes. */
	struct Relayed final
	{
		size_t hash = 0;
		Anope::string preview;
	};
	std::unordered_map<std::string, Relayed> relayed;
	std::deque<std::string> relayed_order;

	CommandBSAdd cmd_add;
	CommandBSSet cmd_set;
	CommandBSDel cmd_del;
	CommandBSList cmd_list;
	CommandBSGuilds cmd_guilds;
	CommandBSChannels cmd_channels;
	BridgeReapTimer reaper;

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
	static void HandleMessage(const std::shared_ptr<BridgeState> &state, const dpp::message &msg, bool edit)
	{
		const std::string channel_id = msg.channel_id.str();
		if (!state->IsBridged(channel_id))
			return;

		/* Messages which this module sent itself must not come back: either
		 * through one of its webhooks, or through the bot account when the
		 * webhook fallback is in use. Other bots are relayed as normal. */
		if (state->IsOwnWebhook(msg.webhook_id.str()) || state->IsSelf(msg.author.id.str()))
			return;

		RelayJob job;
		job.text = RenderMessage(msg);
		if (job.text.empty())
			return;

		job.channel_id = channel_id;
		job.guild_id = msg.guild_id.str();
		job.discord_id = msg.author.id.str();
		job.msg_id = msg.id.str();
		job.edit = edit;

		std::string display = msg.member.get_nickname();
		if (display.empty())
			display = msg.author.global_name;
		if (display.empty())
			display = msg.author.username;
		if (display.empty())
			display = "discord";
		job.display = display;

		state->Post([job](ModuleBridgeServ *module) { module->RelayToIrc(job); });
	}

	/* ------------------------------------------------------------------ */
	/* Discord connection lifecycle                                       */
	/* ------------------------------------------------------------------ */

	void StartCluster()
	{
		if (this->core)
			return;

		if (this->token.empty())
		{
			Log(this) << "BridgeServ: no token is configured; the Discord link is disabled.";
			return;
		}

		static constexpr uint32_t intents = dpp::i_guilds | dpp::i_guild_messages | dpp::i_message_content;
		this->core = new dpp::cluster(this->token.str(), intents);

		auto state = this->state;

		this->core->on_log([state](const dpp::log_t &event)
		{
			if (event.severity < dpp::ll_warning)
				return;

			const std::string message = event.message;
			state->Post([message](ModuleBridgeServ *module)
			{
				Log(module) << "DPP: " << message;
			});
		});

		this->core->on_ready([state](const dpp::ready_t &event)
		{
			const auto guilds = event.guild_count;

			/* The bot account is only known once the gateway says hello. */
			if (event.owner)
				state->SetSelf(event.owner->me.id.str());

			state->Post([guilds](ModuleBridgeServ *module) { module->OnDiscordReady(guilds); });
		});

		this->core->on_message_create([state](const dpp::message_create_t &event)
		{
			HandleMessage(state, event.msg, false);
		});

		this->core->on_message_update([state](const dpp::message_update_t &event)
		{
			HandleMessage(state, event.msg, true);
		});

		this->core->on_message_delete([state](const dpp::message_delete_t &event)
		{
			const std::string channel_id = event.channel_id.str();
			if (!state->IsBridged(channel_id))
				return;

			RelayJob job;
			job.channel_id = channel_id;
			job.guild_id = event.guild_id.str();
			job.msg_id = event.id.str();
			job.del = true;

			state->Post([job](ModuleBridgeServ *module) { module->RelayToIrc(job); });
		});

		this->thread = new BridgeThread(this->core, this->state);
		this->thread->Start();
	}

	void StopCluster()
	{
		this->connected = false;

		if (this->core)
			this->core->shutdown();

		if (this->thread)
		{
			this->thread->Join();
			delete this->thread;
			this->thread = nullptr;
		}

		delete this->core;
		this->core = nullptr;
	}

	/* ------------------------------------------------------------------ */
	/* IRC-side: juped bridge server                                      */
	/* ------------------------------------------------------------------ */

	bool EnsureDiscordServer()
	{
		if (this->discord_server)
			return true;
		if (!IRCD || !Servers::GetUplink() || !Servers::GetUplink()->IsSynced())
			return false;

		Server *existing = Server::Find(this->server_name, true);
		if (existing)
		{
			if (existing->IsJuped())
			{
				this->discord_server = existing;
				return true;
			}
			Log(this) << "BridgeServ: " << this->server_name << " is already in use by a real server; cannot create bridge server.";
			return false;
		}

		const Anope::string sid = IRCD->SID_Retrieve();
		this->discord_server = new Server(Me, this->server_name, "Discord bridge", sid, 1, true);
		IRCD->SendServer(this->discord_server);
		Log(this) << "BridgeServ: Introduced juped bridge server " << this->server_name;
		return true;
	}

	/* ------------------------------------------------------------------ */
	/* IRC-side: pseudo client management                                  */
	/* ------------------------------------------------------------------ */

	Anope::string MakeNick(const Anope::string &raw) const
	{
		/* Leave room for the uniquifying suffix that is added below. */
		const size_t maxlen = IRCD->MaxNick ? IRCD->MaxNick : 31;
		const size_t budget = maxlen > 3 ? maxlen - 3 : maxlen;

		Anope::string nick;
		for (const auto raw_chr : raw)
		{
			if (nick.length() >= budget)
				break;

			const auto chr = static_cast<unsigned char>(raw_chr);
			if (chr < 0x80 && std::isalnum(chr))
				nick += static_cast<char>(chr);
			else if (!nick.empty() && nick[nick.length() - 1] != '_')
				nick += '_';
		}
		while (!nick.empty() && nick[nick.length() - 1] == '_')
			nick.erase(nick.length() - 1);

		if (nick.empty())
			nick = "discord";
		if (!IRCD->IsNickValid(nick))
			nick = "d_" + nick;
		if (!IRCD->IsNickValid(nick))
			nick = "discord";

		const Anope::string base = nick;
		for (unsigned suffix = 2; User::Find(nick, true) || !IRCD->IsNickValid(nick); ++suffix)
		{
			if (suffix > 99)
				return "";

			nick = base + "_" + Anope::ToString(suffix);
		}
		return nick;
	}

	Anope::string MakeIdent(const Anope::string &discord_id) const
	{
		unsigned char digest[SHA256_DIGEST_LENGTH];
		SHA256(reinterpret_cast<const unsigned char *>(discord_id.c_str()), discord_id.length(), digest);

		const Anope::string raw(reinterpret_cast<const char *>(digest), SHA256_DIGEST_LENGTH);
		Anope::string hex = Anope::Hex(raw);

		/* networkinfo:userlen is 10 by default, which is shorter than the hash
		 * prefix that this module used to generate unconditionally. */
		const size_t maxlen = IRCD->MaxUser ? IRCD->MaxUser : 10;
		if (hex.length() > maxlen)
			hex = hex.substr(0, maxlen);

		if (hex.empty() || !IRCD->IsIdentValid(hex))
			return "discord";
		return hex;
	}

	DiscordUser *EnsureUser(const Anope::string &discord_id, const Anope::string &display)
	{
		const auto it = this->discord_users.find(discord_id);
		if (it != this->discord_users.end())
			return it->second;

		if (!this->EnsureDiscordServer())
			return nullptr;

		const Anope::string nick = this->MakeNick(display);
		if (nick.empty())
		{
			Log(this) << "BridgeServ: unable to allocate an IRC nick for Discord user " << discord_id << "; dropping message.";
			return nullptr;
		}

		User *user = User::OnIntroduce(nick, this->MakeIdent(discord_id), this->discord_server->GetName(),
			"", "", this->discord_server, "Discord: " + display, Anope::CurTime, "", IRCD->UID_Retrieve(), nullptr);
		if (!user)
		{
			/* The nick or UID collided with a real user; both sides have been
			 * killed by the factory, so there is nobody to relay as. */
			Log(this) << "BridgeServ: collision introducing pseudo client " << nick << "; dropping message.";
			return nullptr;
		}
		IRCD->SendClientIntroduction(user);

		auto *du = new DiscordUser();
		du->discord_id = discord_id;
		du->user = user;
		this->discord_users[discord_id] = du;

		Log(this) << "BridgeServ: Introduced pseudo client " << nick << " (" << discord_id << ")";
		return du;
	}

	void EnsureJoin(DiscordUser *du, const Anope::string &channel_name)
	{
		if (!du->user)
			return;

		bool created = false;
		Channel *chan = Channel::FindOrCreate(channel_name, created);
		if (chan->FindUser(du->user))
			return;

		chan->JoinUser(du->user, nullptr);
		IRCD->SendJoin(du->user, chan, nullptr);
		du->chans.insert(chan->name);
	}

	void PartPseudo(DiscordUser *du, const Anope::string &channel_name, const Anope::string &reason)
	{
		Channel *c = Channel::Find(channel_name);
		if (c && du->user && c->FindUser(du->user))
		{
			IRCD->SendPart(du->user, c, reason);
			c->DeleteUser(du->user);
		}
		du->chans.erase(channel_name);
	}

	/* ------------------------------------------------------------------ */
	/* Relay: Discord -> IRC                                              */
	/* ------------------------------------------------------------------ */

	/** Remembers the rendering of a relayed message for edit deduplication. */
	void Remember(const std::string &key, const Relayed &what)
	{
		const auto result = this->relayed.emplace(key, what);
		if (!result.second)
		{
			result.first->second = what;
			return;
		}

		this->relayed_order.push_back(key);
		while (this->relayed_order.size() > 256)
		{
			this->relayed.erase(this->relayed_order.front());
			this->relayed_order.pop_front();
		}
	}

	/** Calculates how many bytes of message payload fit on the wire. */
	size_t PayloadBudget(User *u, const Anope::string &target) const
	{
		size_t source = u->GetUID().length();
		source = std::max(source, u->nick.length() + u->GetIdent().length() + u->GetDisplayedHost().length() + 2);

		/* ":<source> PRIVMSG <target> :<payload>\r\n" */
		const size_t overhead = 1 + source + 1 + 8 + target.length() + 2;
		if (overhead + 64 >= 510)
			return 64;
		return 510 - overhead;
	}

	/** Consumes a relay token, refilling the bucket first. */
	bool TakeToken(Bridge *bridge)
	{
		if (!this->flood_lines)
			return true;

		const time_t secs = std::max<time_t>(this->flood_secs, 1);
		if (!bridge->tokens_at)
		{
			bridge->tokens = this->flood_lines;
			bridge->tokens_at = Anope::CurTime;
		}

		const time_t elapsed = Anope::CurTime - bridge->tokens_at;
		if (elapsed >= secs)
		{
			const time_t refill = elapsed / secs;
			bridge->tokens = static_cast<unsigned>(std::min<time_t>(this->flood_lines, bridge->tokens + refill));
			bridge->tokens_at += refill * secs;
		}

		if (!bridge->tokens)
			return false;

		--bridge->tokens;
		return true;
	}

	void RelayDelete(Bridge *bridge, const std::string &key)
	{
		if (!this->relay_deletes)
			return;

		const auto it = this->relayed.find(key);
		if (it == this->relayed.end())
			return;

		const Anope::string preview = it->second.preview;
		this->relayed.erase(it);

		auto *bi = this->GetClient();
		if (bi)
			IRCD->SendNotice(bi, bridge->irc_channel, Anope::Format(Language::Translate(_("message deleted (%s)")), preview.c_str()));
	}

public:
	void RelayToIrc(const RelayJob &job)
	{
		if (!IRCD)
			return;

		Bridge *bridge = this->FindChannel(job.channel_id);
		if (!bridge || bridge->irc_channel.empty())
			return;

		/* Only relay traffic from the guild the bridge was pointed at. */
		if (!job.guild_id.empty() && job.guild_id != "0" && !bridge->guild.equals_ci(job.guild_id))
			return;

		const std::string key = job.channel_id.str() + "/" + job.msg_id.str();
		if (job.del)
		{
			this->RelayDelete(bridge, key);
			return;
		}

		if (job.text.empty())
			return;

		Relayed record;
		record.hash = std::hash<std::string>{ }(job.text.str());
		record.preview = job.text.substr(0, 60);

		if (job.edit)
		{
			if (!this->relay_edits)
				return;

			const auto it = this->relayed.find(key);
			if (it == this->relayed.end() || it->second.hash == record.hash)
				return; // never relayed, or nothing visible changed.
		}

		DiscordUser *du = this->EnsureUser(job.discord_id, job.display);
		if (!du || !du->user)
			return;

		du->last_active = Anope::CurTime;
		this->EnsureJoin(du, bridge->irc_channel);

		auto lines = Text::SplitRelayLines(job.text.str());
		size_t skipped = 0;
		if (lines.size() > this->max_lines)
		{
			skipped = lines.size() - this->max_lines;
			lines.resize(this->max_lines);
		}

		const size_t budget = this->PayloadBudget(du->user, bridge->irc_channel);
		bool first = true;
		bool sent = false;
		for (const auto &line : lines)
		{
			std::string pending = line;
			if (first && job.edit)
				pending.insert(0, "(edit) ");
			first = false;

			while (!pending.empty())
			{
				const std::string chunk = Text::TruncateUtf8(pending, budget);
				if (chunk.empty())
					break; // the budget can not fit even one character.

				pending.erase(0, chunk.length());

				if (!this->TakeToken(bridge))
				{
					++bridge->throttled;
					continue;
				}

				if (bridge->throttled)
				{
					IRCD->SendNotice(du->user, bridge->irc_channel, Anope::Format(Language::Translate(_("%u bridge lines dropped (rate limit)")), bridge->throttled));
					bridge->throttled = 0;
				}
				IRCD->SendPrivmsg(du->user, bridge->irc_channel, chunk);
				sent = true;
			}
		}

		/* Only a message which reached the channel is remembered, so that an
		 * edit or delete of a message nobody saw stays silent. */
		if (sent)
			this->Remember(key, record);

		if (skipped)
			IRCD->SendNotice(du->user, bridge->irc_channel, Anope::Format(Language::Translate(_("... [message truncated, %zu more lines]")), skipped));
	}

	/* ------------------------------------------------------------------ */
	/* Relay: IRC -> Discord                                              */
	/* ------------------------------------------------------------------ */

private:
	void SendIrcToDiscord(User *u, Bridge *bridge, const std::string &text)
	{
		if (!this->core || !this->connected || text.empty())
			return;

		dpp::message msg(bridge->ForeignId(), text);

		/* Messages relayed from IRC never ping anybody; the wire payload gets
		 * an empty allowed_mentions.parse list. */
		msg.set_allowed_mentions(false, false, false, false);

		if (!bridge->webhook_id.empty())
		{
			auto state = this->state;
			const Anope::string key = bridge->irc_channel;
			try
			{
				dpp::webhook hook(dpp::snowflake(bridge->webhook_id.c_str()), bridge->webhook_token.str());
				hook.name = Text::WebhookName(u->nick.str(), this->webhook_suffix.str());

				this->core->execute_webhook(hook, msg, false, 0, "", [state, key](const dpp::confirmation_callback_t &cb)
				{
					if (!cb.is_error())
						return;

					const auto status = cb.http_info.status;
					const std::string error = cb.get_error().human_readable;
					state->Post([key, status, error](ModuleBridgeServ *module)
					{
						module->OnWebhookSendFailed(key, status, error);
					});
				});
				return;
			}
			catch (const dpp::exception &err)
			{
				Log(this) << "BridgeServ: unable to relay to the webhook for " << bridge->irc_channel << ": " << err.what();
			}
		}

		/* No usable webhook; fall back to the bot account and try to set a
		 * webhook up for the next message. */
		dpp::message fallback(bridge->ForeignId(), "<" + u->nick.str() + "> " + text);
		fallback.set_allowed_mentions(false, false, false, false);
		try
		{
			this->core->message_create(fallback);
		}
		catch (const dpp::exception &err)
		{
			Log(this) << "BridgeServ: unable to relay to " << bridge->irc_channel << ": " << err.what();
		}
		this->EnsureWebhook(bridge);
	}

	/* ------------------------------------------------------------------ */
	/* Webhook management                                                  */
	/* ------------------------------------------------------------------ */

	void CreateWebhook(const Anope::string &key, const Anope::string &channel)
	{
		Bridge *bridge = this->FindBridge(key);
		if (!bridge)
			return; // the bridge was removed while the listing was in flight.

		/* The bridge was repointed while the listing was in flight; the
		 * pending guard is released so that the new channel is set up. */
		if (!bridge->foreign_channel.equals_ci(channel))
		{
			bridge->webhook_pending = false;
			this->EnsureWebhook(bridge);
			return;
		}

		if (!this->core || !this->connected)
		{
			/* The Discord link dropped between listing and creating; release
			 * the guard so that a later message retries rather than leaving
			 * the bridge pending forever. */
			this->OnWebhookFailed(key, "the Discord link went away");
			return;
		}

		dpp::webhook hook;
		hook.channel_id = bridge->ForeignId();
		hook.name = this->bridge_name.str();

		auto state = this->state;
		try
		{
			this->core->create_webhook(hook, [state, key, channel](const dpp::confirmation_callback_t &cb)
			{
				if (cb.is_error())
				{
					const std::string error = cb.get_error().human_readable;
					state->Post([key, error](ModuleBridgeServ *module) { module->OnWebhookFailed(key, error); });
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
					state->Post([key](ModuleBridgeServ *module) { module->OnWebhookFailed(key, "malformed webhook response"); });
					return;
				}

				state->Post([key, channel, id, tok](ModuleBridgeServ *module) { module->OnWebhookReady(key, channel, id, tok, true); });
			});
		}
		catch (const dpp::exception &err)
		{
			this->OnWebhookFailed(key, err.what());
		}
	}

public:
	void OnWebhookReady(const Anope::string &key, const Anope::string &channel, const std::string &id, const std::string &tok, bool created)
	{
		Bridge *bridge = this->FindBridge(key);
		if (!bridge)
			return;

		/* The bridge may have been repointed while the lookup was in flight;
		 * adopting the old channel's webhook would send IRC traffic to the
		 * wrong Discord channel. */
		if (!bridge->foreign_channel.equals_ci(channel))
		{
			bridge->webhook_pending = false;
			Log(this) << "BridgeServ: discarding a webhook for " << key << " which no longer points at " << channel;
			this->EnsureWebhook(bridge);
			return;
		}

		bridge->webhook_pending = false;
		bridge->webhook_retry_at = 0;
		bridge->webhook_id = id;
		bridge->webhook_token = tok;
		bridge->QueueUpdate();

		this->state->AddOwnWebhook(id);
		Log(this) << "BridgeServ: " << (created ? "created" : "adopted") << " Discord webhook " << id << " for " << key;
	}

	void OnWebhookFailed(const Anope::string &key, const std::string &error)
	{
		Bridge *bridge = this->FindBridge(key);
		if (!bridge)
			return;

		bridge->webhook_pending = false;
		bridge->webhook_retry_at = Anope::CurTime + 60;
		Log(this) << "BridgeServ: webhook setup for " << key << " failed: " << error;
	}

	void OnWebhookSendFailed(const Anope::string &key, uint16_t status, const std::string &error)
	{
		Bridge *bridge = this->FindBridge(key);
		if (!bridge)
			return;

		/* The webhook was deleted on the Discord side or its token was
		 * revoked; drop it and set a new one up. */
		if (status == 401 || status == 403 || status == 404)
		{
			/* An isolated failure is retried at once so that a deleted
			 * webhook heals on the next message, but a webhook which keeps
			 * failing is backed off like any other setup failure. */
			if (Anope::CurTime - bridge->webhook_failed_at > 300)
				bridge->webhook_failures = 0;
			bridge->webhook_failed_at = Anope::CurTime;
			++bridge->webhook_failures;

			this->state->DelOwnWebhook(bridge->webhook_id.str());
			bridge->webhook_id.clear();
			bridge->webhook_token.clear();
			bridge->webhook_pending = false;
			bridge->webhook_retry_at = bridge->webhook_failures > 1 ? Anope::CurTime + 60 : 0;
			bridge->QueueUpdate();

			Log(this) << "BridgeServ: the webhook for " << key << " is no longer usable (" << status << "); a new one will be created.";
			this->EnsureWebhook(bridge);
			return;
		}
		Log(this) << "BridgeServ: relaying to the webhook for " << key << " failed: " << error;
	}

	void EnsureWebhook(Bridge *bridge)
	{
		if (!this->core || !this->connected)
			return;
		if (!bridge->webhook_id.empty() || bridge->webhook_pending)
			return;
		if (Anope::CurTime < bridge->webhook_retry_at)
			return;

		bridge->webhook_pending = true;

		auto state = this->state;
		const Anope::string key = bridge->irc_channel;
		const Anope::string channel = bridge->foreign_channel;
		const std::string wanted = this->bridge_name.str();

		/* Reuse the webhook from a previous run rather than creating a new one
		 * on every load, which would litter the channel with dead webhooks. */
		try
		{
			this->core->get_channel_webhooks(bridge->ForeignId(), [state, key, channel, wanted](const dpp::confirmation_callback_t &cb)
			{
				if (cb.is_error())
				{
					const std::string error = cb.get_error().human_readable;
					state->Post([key, error](ModuleBridgeServ *module) { module->OnWebhookFailed(key, error); });
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
					state->Post([key](ModuleBridgeServ *module) { module->OnWebhookFailed(key, "malformed webhook list response"); });
					return;
				}

				if (id.empty())
				{
					state->Post([key, channel](ModuleBridgeServ *module) { module->CreateWebhook(key, channel); });
					return;
				}
				state->Post([key, channel, id, tok](ModuleBridgeServ *module) { module->OnWebhookReady(key, channel, id, tok, false); });
			});
		}
		catch (const dpp::exception &err)
		{
			this->OnWebhookFailed(key, err.what());
		}
	}

	/** Deletes the webhook of a bridge which is going away. */
	void DeleteWebhook(Bridge *bridge)
	{
		if (bridge->webhook_id.empty())
			return;

		const std::string id = bridge->webhook_id.str();
		this->state->DelOwnWebhook(id);

		if (this->core && this->connected)
		{
			auto state = this->state;
			const Anope::string key = bridge->irc_channel;
			this->core->delete_webhook(dpp::snowflake(bridge->webhook_id.c_str()), [state, key](const dpp::confirmation_callback_t &cb)
			{
				if (!cb.is_error())
					return;

				const std::string error = cb.get_error().human_readable;
				state->Post([key, error](ModuleBridgeServ *module)
				{
					Log(module) << "BridgeServ: unable to delete the webhook for " << key << ": " << error;
				});
			});
		}

		bridge->webhook_id.clear();
		bridge->webhook_token.clear();
		bridge->webhook_pending = false;
		bridge->webhook_retry_at = 0;
	}

	/* ------------------------------------------------------------------ */
	/* Bridge and pseudo client bookkeeping                                */
	/* ------------------------------------------------------------------ */

	BotInfo *GetClient() const
	{
		return BotInfo::Find(this->client_name, true);
	}

	bool IsConnected() const { return this->connected; }
	dpp::cluster *GetCore() const { return this->core; }

	Bridge *FindBridge(const Anope::string &irc_channel) const
	{
		for (auto *bridge : this->bridges)
		{
			if (bridge->irc_channel.equals_ci(irc_channel))
				return bridge;
		}
		return nullptr;
	}

	Bridge *FindChannel(const Anope::string &foreign_channel) const
	{
		for (auto *bridge : this->bridges)
		{
			if (bridge->foreign_channel.equals_ci(foreign_channel))
				return bridge;
		}
		return nullptr;
	}

	const std::vector<Bridge *> &GetBridges() const { return this->bridges; }

	/** Publishes the set of bridged Discord channels to the Discord thread. */
	void SyncBridged()
	{
		std::unordered_set<std::string> channel_ids;
		channel_ids.reserve(this->bridges.size());
		for (const auto *bridge : this->bridges)
			channel_ids.insert(bridge->foreign_channel.str());

		this->state->SetBridged(std::move(channel_ids));
	}

	void JoinChannel(Bridge *bridge)
	{
		auto *bi = this->GetClient();
		if (bi && !bridge->irc_channel.empty())
			bi->Join(bridge->irc_channel);
	}

	void AddBridge(Bridge *bridge)
	{
		this->bridges.push_back(bridge);
		this->SyncBridged();
		this->state->AddOwnWebhook(bridge->webhook_id.str());

		if (IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced())
		{
			this->JoinChannel(bridge);
			this->EnsureWebhook(bridge);
		}
	}

	void DropBridge(Bridge *bridge)
	{
		const auto it = std::find(this->bridges.begin(), this->bridges.end(), bridge);
		if (it != this->bridges.end())
			this->bridges.erase(it);

		this->DeleteWebhook(bridge);
		delete bridge;

		this->SyncBridged();
		this->PruneChannels();
	}

	/** Parts pseudo clients from channels which are no longer bridged. */
	void PruneChannels()
	{
		for (auto &[_, du] : this->discord_users)
		{
			std::vector<Anope::string> stale;
			for (const auto &chan : du->chans)
			{
				if (!this->FindBridge(chan))
					stale.push_back(chan);
			}

			for (const auto &chan : stale)
				this->PartPseudo(du, chan, "Bridge removed");
		}
	}

	void RemoveAllDiscordUsers(const Anope::string &reason)
	{
		const bool synced = IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced();

		/* User::Quit calls OnUserQuit synchronously, which erases the record
		 * being quit, so the records to quit are snapshotted first. */
		std::vector<DiscordUser *> snapshot;
		snapshot.reserve(this->discord_users.size());
		for (const auto &[_, du] : this->discord_users)
			snapshot.push_back(du);

		for (auto *du : snapshot)
		{
			if (!du->user)
				continue;

			if (synced)
				IRCD->SendQuit(du->user, reason);
			du->user->Quit(reason);
		}

		/* Anything left behind had no pseudo client to quit. */
		for (const auto &[_, du] : this->discord_users)
			delete du;
		this->discord_users.clear();

		if (this->discord_server)
		{
			Server *server = this->discord_server;
			this->discord_server = nullptr;
			if (synced)
				IRCD->SendSquit(server, reason);
			server->Delete(reason);
		}
	}

	/** Quits pseudo clients which have not spoken for useridle seconds. */
	void ReapIdleUsers()
	{
		if (!this->user_idle)
			return;

		const bool synced = IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced();
		for (auto it = this->discord_users.begin(); it != this->discord_users.end(); )
		{
			DiscordUser *du = it->second;
			if (!du->user || Anope::CurTime - du->last_active <= this->user_idle)
			{
				++it;
				continue;
			}

			/* The record is erased by OnUserQuit when the deferred quit is
			 * processed, so it must not be erased here. */
			++it;
			if (synced)
				IRCD->SendQuit(du->user, "Idle");
			du->user->Quit("Idle");
		}
	}

	size_t CountBridgedUsers(const Anope::string &channel) const
	{
		size_t count = 0;
		for (const auto &[_, du] : this->discord_users)
		{
			if (du->chans.count(channel))
				++count;
		}
		return count;
	}

	/* ------------------------------------------------------------------ */
	/* Asynchronous command helpers                                        */
	/* ------------------------------------------------------------------ */

	/** Delivers an asynchronously fetched listing to the requesting user. */
	void DeliverListing(const std::string &nick, const std::string &svc, bool channels, bool failed, const std::vector<Anope::string> &lines)
	{
		User *u = User::Find(nick, true);
		auto *bi = BotInfo::Find(svc, true);
		if (!u || !bi)
			return; // the requester or the service went away.

		if (failed)
		{
			u->SendMessage(bi, channels
				? _("Failed to retrieve the channel list from Discord.")
				: _("Failed to retrieve the guild list from Discord."));
			return;
		}

		if (lines.empty())
		{
			u->SendMessage(bi, channels
				? _("No text channels were found in that guild.")
				: _("The bot is not present in any guilds."));
			return;
		}

		const size_t shown = std::min<size_t>(lines.size(), 100);
		for (size_t idx = 0; idx < shown; ++idx)
			u->SendMessage(bi, lines[idx]);

		if (lines.size() > shown)
			u->SendMessage(bi, Anope::Format(Language::Translate(u->Account(), _("(... and %zu more)")), lines.size() - shown));
	}

	void FetchGuilds(CommandSource &source)
	{
		if (!this->core || !this->connected)
		{
			source.Reply(_("Not connected to Discord."));
			return;
		}

		auto state = this->state;
		const std::string nick = source.GetNick().str();
		const std::string svc = source.service ? source.service->nick.str() : "";

		this->core->current_user_get_guilds([state, nick, svc](const dpp::confirmation_callback_t &cb)
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
			state->Post([nick, svc, failed, lines](ModuleBridgeServ *module)
			{
				module->DeliverListing(nick, svc, false, failed, lines);
			});
		});
	}

	void FetchChannels(CommandSource &source, const Anope::string &guild)
	{
		if (!this->core || !this->connected)
		{
			source.Reply(_("Not connected to Discord."));
			return;
		}

		auto state = this->state;
		const std::string nick = source.GetNick().str();
		const std::string svc = source.service ? source.service->nick.str() : "";

		this->core->channels_get(dpp::snowflake(guild.c_str()), [state, nick, svc](const dpp::confirmation_callback_t &cb)
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
			state->Post([nick, svc, failed, lines](ModuleBridgeServ *module)
			{
				module->DeliverListing(nick, svc, true, failed, lines);
			});
		});
	}

	/* ------------------------------------------------------------------ */
	/* Module                                                              */
	/* ------------------------------------------------------------------ */

	ModuleBridgeServ(const Anope::string &modname, const Anope::string &creator)
		: Module(modname, creator, VENDOR)
		, state(std::make_shared<BridgeState>(this, this))
		, cmd_add(this)
		, cmd_set(this)
		, cmd_del(this)
		, cmd_list(this)
		, cmd_guilds(this)
		, cmd_channels(this)
		, reaper(this)
	{
		this->SetAuthor("Anope");
		this->SetVersion("1.0");

		this->btype = new BridgeType(this);

		Log(this) << "BridgeServ: Loaded, using the vendored DPP (Discord++) library.";
	}

	~ModuleBridgeServ() override
	{
		/* Detach first: any Discord callback which is already in flight must
		 * not be able to reach this object once it starts being destroyed. */
		this->state->Detach();
		this->StopCluster();

		this->RemoveAllDiscordUsers("Discord bridge unloading");

		for (auto *bridge : this->bridges)
			delete bridge;
		this->bridges.clear();

		delete this->btype;
		this->btype = nullptr;
	}

	void OnReload(Configuration::Conf &conf) override
	{
		auto &block = conf.GetModule(this);

		this->client_name = block.Get<const Anope::string>("client", "BridgeServ");
		this->bridge_name = block.Get<const Anope::string>("bridgename", "IRC Bridge");
		this->webhook_suffix = block.Get<const Anope::string>("webhooksuffix", " (IRC)");
		this->user_idle = block.Get<time_t>("useridle", "1h");
		this->max_lines = std::max<size_t>(block.Get<size_t>("maxlines", "8"), 1);
		this->flood_lines = block.Get<unsigned>("floodlines", "6");
		this->flood_secs = std::max<time_t>(block.Get<time_t>("floodsecs", "4s"), 1);
		this->relay_edits = block.Get<bool>("relayedits", "yes");
		this->relay_deletes = block.Get<bool>("relaydeletes", "no");

		const Anope::string new_server = block.Get<const Anope::string>("server", "discord.bridged");
		if (!this->server_name.empty() && new_server != this->server_name)
			this->RemoveAllDiscordUsers("Bridge server renamed");
		this->server_name = new_server;

		const Anope::string new_token = block.Get<const Anope::string>("token");
		if (new_token != this->token || !this->core)
		{
			this->StopCluster();
			this->token = new_token;
			this->StartCluster();
		}
	}

	void OnUplinkSync(Server *s) override
	{
		/* Joining from OnServerConnect would write the join into the uplink
		 * socket during link negotiation, which InspIRCd rejects; the end of
		 * the burst is the first point at which a join is legal. */
		for (auto *bridge : this->bridges)
		{
			this->JoinChannel(bridge);
			this->EnsureWebhook(bridge);
		}
	}

	void OnNotify() override
	{
		for (;;)
		{
			BridgeState::Job job;
			if (!this->state->PopJob(job))
				break;

			job(this);
		}

		if (const auto dropped = this->state->TakeDropped())
			Log(this) << "BridgeServ: dropped " << dropped << " queued Discord events; the bridge is falling behind.";
	}

	void OnUserQuit(User *u, const Anope::string &msg) override
	{
		if (!this->discord_server || !u || u->server != this->discord_server)
			return;

		for (auto it = this->discord_users.begin(); it != this->discord_users.end(); ++it)
		{
			if (it->second->user != u)
				continue;

			delete it->second;
			this->discord_users.erase(it);
			break;
		}
	}

	void OnPrivmsg(User *u, Channel *c, Anope::string &msg, const Anope::map<Anope::string> &tags) override
	{
		if (!u || !c || msg.empty())
			return;

		/* Never relay services clients or our own pseudo clients; that would
		 * loop messages back into the channel they came from. */
		if (u == this->GetClient() || u->server == Me)
			return;
		if (this->discord_server && u->server == this->discord_server)
			return;
		if (!this->core || !this->connected)
			return;

		Bridge *bridge = this->FindBridge(c->name);
		if (!bridge)
			return;

		Anope::string ctcp_name;
		Anope::string ctcp_body;
		bool action = false;
		Anope::string payload = msg;
		if (Anope::ParseCTCP(msg, ctcp_name, ctcp_body))
		{
			/* An ACTION renders as italics on Discord; no other CTCP has a
			 * sensible rendering, so those are dropped. */
			if (!ctcp_name.equals_ci("ACTION") || ctcp_body.empty())
				return;

			action = true;
			payload = ctcp_body;
		}

		std::string text = dpp::utility::markdown_escape(Anope::RemoveFormatting(payload).str(), true);

		/* Discord rejects messages longer than 2000 characters. The body is
		 * truncated before the italic markers are added so that an oversized
		 * action does not lose its closing marker. */
		text = Text::TruncateUtf8(text, action ? 1998 : 2000);

		/* A trailing escape would escape the closing marker instead. */
		if (action)
		{
			size_t slashes = 0;
			while (slashes < text.length() && text[text.length() - 1 - slashes] == '\\')
				++slashes;
			if (slashes % 2)
				text.erase(text.length() - 1);

			text = "*" + text + "*";
		}

		this->SendIrcToDiscord(u, bridge, text);
	}

	void OnDiscordReady(uint32_t guilds)
	{
		this->connected = true;
		Log(this) << "BridgeServ: connected to Discord (" << guilds << " guild(s)).";

		/* Ready fires again after a reconnect; webhook setup is idempotent. */
		for (auto *bridge : this->bridges)
			this->EnsureWebhook(bridge);
	}

	void OnClusterStopped(const std::string &error)
	{
		this->connected = false;
		Log(this) << "BridgeServ: the Discord connection stopped: " << error;
	}
};

void BridgeThread::Run()
{
	try
	{
		this->cluster->start(dpp::st_wait);
	}
	catch (const dpp::exception &err)
	{
		const std::string error = err.what();
		this->state->Post([error](ModuleBridgeServ *module) { module->OnClusterStopped(error); });
	}
}

BridgeType::BridgeType(ModuleBridgeServ *creator)
	/* The type is deliberately unowned: an owned type is written to a separate
	 * per-module database which Anope only reads when the type is created
	 * after the databases have been loaded, so owned rows would never come
	 * back after a restart. */
	: Serialize::Type("Bridge")
	, module(creator)
{
}

void BridgeType::Serialize(Serializable *obj, Serialize::Data &data) const
{
	const auto *bridge = static_cast<const Bridge *>(obj);

	data.Store("irc-channel", bridge->irc_channel);
	data.Store("guild", bridge->guild);
	data.Store("foreign-channel", bridge->foreign_channel);
	data.Store("webhook-id", bridge->webhook_id);
	data.Store("webhook-token", bridge->webhook_token);
}

Serializable *BridgeType::Unserialize(Serializable *obj, Serialize::Data &data) const
{
	if (!this->module)
		return nullptr;

	const Anope::string irc_channel = data.Load("irc-channel");
	const Anope::string guild = data.Load("guild");
	const Anope::string foreign_channel = data.Load("foreign-channel");

	/* Rows which can not make a working bridge are skipped; the database
	 * loader treats a null return as "ignore this record". */
	if (irc_channel.empty() || !ValidSnowflake(guild) || !ValidSnowflake(foreign_channel))
		return nullptr;

	Bridge *bridge;
	if (obj)
		bridge = anope_dynamic_static_cast<Bridge *>(obj);
	else
	{
		if (this->module->FindBridge(irc_channel) || this->module->FindChannel(foreign_channel))
			return nullptr; // a duplicate of a bridge which is already loaded.

		bridge = new Bridge();
	}

	bridge->irc_channel = irc_channel;
	bridge->guild = guild;
	bridge->foreign_channel = foreign_channel;
	bridge->webhook_id = data.Load("webhook-id");
	bridge->webhook_token = data.Load("webhook-token");

	if (obj)
		this->module->SyncBridged();
	else
		this->module->AddBridge(bridge);

	return bridge;
}

BridgeReapTimer::BridgeReapTimer(ModuleBridgeServ *creator)
	: Timer(creator, 60)
	, module(creator)
{
}

bool BridgeReapTimer::Tick()
{
	this->module->ReapIdleUsers();
	return true;
}

/* -------------------------------------------------------------------------- */
/* Commands                                                                    */
/* -------------------------------------------------------------------------- */

/** Checks the granular permission of a command, with an oper fallback. */
static bool CheckAccess(CommandSource &source, const Anope::string &permission)
{
	if (source.HasPriv(permission) || source.IsServicesOper())
		return true;

	source.Reply(_("Access denied."));
	return false;
}

CommandBSAdd::CommandBSAdd(ModuleBridgeServ *creator)
	: Command(creator, "bridgeserv/add", 3, 3)
	, module(creator)
{
	this->SetDesc(_("Bridge an IRC channel to a Discord channel"));
	this->SetSyntax(_("\037#channel\037 \037guild-id\037 \037channel-id\037"));
}

void CommandBSAdd::Execute(CommandSource &source, const std::vector<Anope::string> &params)
{
	if (!CheckAccess(source, "bridgeserv/add"))
		return;

	const auto &irc_channel = params[0];
	const auto &guild = params[1];
	const auto &foreign_channel = params[2];

	if (!IRCD || !IRCD->IsChannelValid(irc_channel))
	{
		source.Reply(_("Invalid IRC channel name."));
		return;
	}

	if (!ValidSnowflake(guild))
	{
		source.Reply(_("Invalid Discord guild ID."));
		return;
	}

	if (!ValidSnowflake(foreign_channel))
	{
		source.Reply(_("Invalid Discord channel ID."));
		return;
	}

	if (this->module->FindBridge(irc_channel))
	{
		source.Reply(_("\002%s\002 is already bridged."), irc_channel.c_str());
		return;
	}

	if (const auto *other = this->module->FindChannel(foreign_channel))
	{
		source.Reply(_("That Discord channel is already bridged to \002%s\002."), other->irc_channel.c_str());
		return;
	}

	auto *bridge = new Bridge();
	bridge->irc_channel = irc_channel;
	bridge->guild = guild;
	bridge->foreign_channel = foreign_channel;

	this->module->AddBridge(bridge);
	bridge->QueueUpdate();

	Log(LOG_ADMIN, source, this) << "to bridge " << irc_channel << " to guild " << guild << " channel " << foreign_channel;
	source.Reply(_("Added bridge %s <-> guild %s channel %s."), irc_channel.c_str(), guild.c_str(), foreign_channel.c_str());
}

bool CommandBSAdd::OnHelp(CommandSource &source, const Anope::string &subcommand)
{
	this->SendSyntax(source);
	source.Reply(" ");
	source.Reply(_(
		"Creates a bridge between an IRC channel and a channel in a "
		"Discord guild. The guild and channel are given by their "
		"numeric ids, as shown by the \002GUILDS\002 and \002CHANNELS\002 "
		"commands. The bridge service joins the IRC channel and starts "
		"relaying messages in both directions immediately."
	));
	return true;
}

CommandBSSet::CommandBSSet(ModuleBridgeServ *creator)
	: Command(creator, "bridgeserv/set", 3, 3)
	, module(creator)
{
	this->SetDesc(_("Re-point an existing bridge at another Discord channel"));
	this->SetSyntax(_("\037#channel\037 \037guild-id\037 \037channel-id\037"));
}

void CommandBSSet::Execute(CommandSource &source, const std::vector<Anope::string> &params)
{
	if (!CheckAccess(source, "bridgeserv/set"))
		return;

	const auto &irc_channel = params[0];
	const auto &guild = params[1];
	const auto &foreign_channel = params[2];

	Bridge *bridge = this->module->FindBridge(irc_channel);
	if (!bridge)
	{
		source.Reply(_("No such bridge."));
		return;
	}

	if (!ValidSnowflake(guild))
	{
		source.Reply(_("Invalid Discord guild ID."));
		return;
	}

	if (!ValidSnowflake(foreign_channel))
	{
		source.Reply(_("Invalid Discord channel ID."));
		return;
	}

	const auto *other = this->module->FindChannel(foreign_channel);
	if (other && other != bridge)
	{
		source.Reply(_("That Discord channel is already bridged to \002%s\002."), other->irc_channel.c_str());
		return;
	}

	/* The old webhook belongs to the old channel; it is of no use here. */
	if (!bridge->foreign_channel.equals_ci(foreign_channel))
		this->module->DeleteWebhook(bridge);

	bridge->guild = guild;
	bridge->foreign_channel = foreign_channel;
	bridge->QueueUpdate();

	this->module->SyncBridged();
	this->module->PruneChannels();
	this->module->EnsureWebhook(bridge);

	Log(LOG_ADMIN, source, this) << "to point " << irc_channel << " at guild " << guild << " channel " << foreign_channel;
	source.Reply(_("Updated bridge %s <-> guild %s channel %s."), irc_channel.c_str(), guild.c_str(), foreign_channel.c_str());
}

bool CommandBSSet::OnHelp(CommandSource &source, const Anope::string &subcommand)
{
	this->SendSyntax(source);
	source.Reply(" ");
	source.Reply(_(
		"Re-points an existing bridge at a different Discord guild and "
		"channel. The webhook of the previous channel is deleted and a "
		"new one is created for the new channel."
	));
	return true;
}

CommandBSDel::CommandBSDel(ModuleBridgeServ *creator)
	: Command(creator, "bridgeserv/del", 1, 1)
	, module(creator)
{
	this->SetDesc(_("Remove a bridge"));
	this->SetSyntax(_("\037#channel\037"));
}

void CommandBSDel::Execute(CommandSource &source, const std::vector<Anope::string> &params)
{
	if (!CheckAccess(source, "bridgeserv/del"))
		return;

	Bridge *bridge = this->module->FindBridge(params[0]);
	if (!bridge)
	{
		source.Reply(_("No such bridge."));
		return;
	}

	const Anope::string irc_channel = bridge->irc_channel;
	this->module->DropBridge(bridge);

	Log(LOG_ADMIN, source, this) << "to remove the bridge for " << irc_channel;
	source.Reply(_("Bridge removed."));
}

bool CommandBSDel::OnHelp(CommandSource &source, const Anope::string &subcommand)
{
	this->SendSyntax(source);
	source.Reply(" ");
	source.Reply(_(
		"Removes an existing bridge and deletes its Discord webhook. "
		"Pseudo clients for Discord users are removed from the channel "
		"but remain on IRC for any other bridge they are active in."
	));
	return true;
}

CommandBSList::CommandBSList(ModuleBridgeServ *creator)
	: Command(creator, "bridgeserv/list", 0, 0)
	, module(creator)
{
	this->SetDesc(_("List the configured bridges"));
}

void CommandBSList::Execute(CommandSource &source, const std::vector<Anope::string> &params)
{
	if (!CheckAccess(source, "bridgeserv/list"))
		return;

	if (this->module->IsConnected())
		source.Reply(_("Discord: online"));
	else
		source.Reply(_("Discord: offline"));

	const auto &bridges = this->module->GetBridges();
	if (bridges.empty())
	{
		source.Reply(_("No bridges are configured."));
		return;
	}

	ListFormatter list(source.GetAccount());
	list.AddColumn(_("Channel")).AddColumn(_("Guild")).AddColumn(_("Discord channel")).AddColumn(_("Webhook")).AddColumn(_("Users"));

	for (const auto *bridge : bridges)
	{
		ListFormatter::ListEntry entry;
		entry["Channel"] = bridge->irc_channel;
		entry["Guild"] = bridge->guild;
		entry["Discord channel"] = bridge->foreign_channel;
		entry["Webhook"] = bridge->webhook_id.empty() ? _("no") : _("yes");
		entry["Users"] = Anope::ToString(this->module->CountBridgedUsers(bridge->irc_channel));
		list.AddEntry(entry);
	}

	list.SendTo(source);
}

bool CommandBSList::OnHelp(CommandSource &source, const Anope::string &subcommand)
{
	this->SendSyntax(source);
	source.Reply(" ");
	source.Reply(_(
		"Lists the configured bridges, whether each one has a Discord "
		"webhook for per-user identity, and how many Discord users are "
		"currently present in each IRC channel."
	));
	return true;
}

CommandBSGuilds::CommandBSGuilds(ModuleBridgeServ *creator)
	: Command(creator, "bridgeserv/guilds", 0, 0)
	, module(creator)
{
	this->SetDesc(_("List the Discord guilds the bot is in"));
}

void CommandBSGuilds::Execute(CommandSource &source, const std::vector<Anope::string> &params)
{
	if (!CheckAccess(source, "bridgeserv/guilds"))
		return;

	this->module->FetchGuilds(source);
}

bool CommandBSGuilds::OnHelp(CommandSource &source, const Anope::string &subcommand)
{
	this->SendSyntax(source);
	source.Reply(" ");
	source.Reply(_(
		"Lists the Discord guilds which the bot account has been "
		"invited to, as \037name\037 (\037id\037). The ids are what the "
		"\002ADD\002 and \002SET\002 commands expect."
	));
	return true;
}

CommandBSChannels::CommandBSChannels(ModuleBridgeServ *creator)
	: Command(creator, "bridgeserv/channels", 1, 1)
	, module(creator)
{
	this->SetDesc(_("List the text channels of a Discord guild"));
	this->SetSyntax(_("\037guild-id\037"));
}

void CommandBSChannels::Execute(CommandSource &source, const std::vector<Anope::string> &params)
{
	if (!CheckAccess(source, "bridgeserv/channels"))
		return;

	if (!ValidSnowflake(params[0]))
	{
		source.Reply(_("Invalid Discord guild ID."));
		return;
	}

	this->module->FetchChannels(source, params[0]);
}

bool CommandBSChannels::OnHelp(CommandSource &source, const Anope::string &subcommand)
{
	this->SendSyntax(source);
	source.Reply(" ");
	source.Reply(_(
		"Lists the text channels of a Discord guild as \037name\037 "
		"(\037id\037). The guild is given by its id, as shown by the "
		"\002GUILDS\002 command."
	));
	return true;
}

MODULE_INIT(ModuleBridgeServ)
