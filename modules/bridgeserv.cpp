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

#include <openssl/sha.h>

#include <dpp/dpp.h>
#include <dpp/intents.h>
#include <dpp/webhook.h>

#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

class ModuleBridgeServ;
static ModuleBridgeServ *me = nullptr;

static Anope::string SanitizeMessage(Anope::string message)
{
	for (size_t i = 0; i < message.length(); ++i)
		if (message[i] == '\n' || message[i] == '\r')
			message[i] = ' ';
	return message;
}

/** A thread running the shared DPP socket engine. */
class BridgeThread final
	: public Thread
{
	dpp::cluster *cluster = nullptr;

public:
	explicit BridgeThread(dpp::cluster *c) : cluster(c) { }

	void Run() override
	{
		try
		{
			this->cluster->start(dpp::st_wait);
		}
		catch (const dpp::exception &)
		{
		}
	}
};

/** A minimal User subclass with a public constructor for creating pseudo
 * clients. The User constructor is protected; subclasses like BotInfo use
 * the same pattern to instantiate themselves.
 */
class PseudoUser final
	: public User
{
public:
	PseudoUser(const Anope::string &nick, const Anope::string &ident,
		const Anope::string &host, Server *server, const Anope::string &realname,
		const Anope::string &uid)
		: User(nick, ident, host, "", "", server, realname, Anope::CurTime, "", {}, uid, nullptr)
	{
	}
};

/** A Discord user represented as an IRC pseudo client.
 *
 * One DiscordUser exists per Discord user regardless of how many bridges
 * the user is active in. The nick is the sanitised Discord display name
 * (or username), and the hostmask is:
 *   <nick>!<uid-sha256>@discord.bridged
 * where uid-sha256 is the first 16 hex characters of the SHA-256 hash of
 * the Discord snowflake UID, making the identity unique and stable.
 */
class DiscordUser final
{
public:
	Anope::string discord_id;
	Anope::string nick;
	User *user = nullptr;
	std::set<Anope::string> chans;
};

/** A bridge between an IRC channel and a channel in a Discord guild. */
class Bridge final
	: public Serializable
{
public:
	Anope::string irc_channel;
	Anope::string guild;
	Anope::string foreign_channel;
	/* Webhook for sending messages with per-IRC-user identity */
	Anope::string webhook_id;
	Anope::string webhook_token;
	/* Guard against concurrent create_webhook calls for the same bridge */
	bool webhook_pending = false;

	explicit Bridge() : Serializable("Bridge") { }

	dpp::snowflake ForeignId() const
	{
		return dpp::snowflake(this->foreign_channel.c_str());
	}
};

class BridgeType final
	: public Serialize::Type
{
public:
	explicit BridgeType() : Serialize::Type("Bridge") { }

	void Serialize(Serializable *obj, Serialize::Data &data) const override
	{
		const auto *bridge = static_cast<const Bridge *>(obj);

		data.Store("irc-channel", bridge->irc_channel);
		data.Store("guild", bridge->guild);
		data.Store("foreign-channel", bridge->foreign_channel);
		data.Store("webhook-id", bridge->webhook_id);
		data.Store("webhook-token", bridge->webhook_token);
	}

	Serializable *Unserialize(Serializable *obj, Serialize::Data &data) const override;
};

class ModuleBridgeServ final
	: public Module
	, public Pipe
{
	friend class Bridge;
	friend class BridgeType;

	BridgeType *btype = nullptr;
	dpp::cluster *core = nullptr;
	BridgeThread *thread = nullptr;
	Anope::string token;
	Anope::string server_name;
	bool active_conn = false;
	Server *discord_server = nullptr;

	std::vector<Bridge *> bridges;
	std::map<Anope::string, DiscordUser *> discord_users;
	std::deque<std::function<void()>> jobs;
	std::mutex mutex;

	/* ------------------------------------------------------------------ */
	/* Helpers                                                              */
	/* ------------------------------------------------------------------ */

	BotInfo *GetClient() const
	{
		return BotInfo::Find(Config->GetModule(this).Get<const Anope::string>("client", "BridgeServ"), true);
	}

	void Reply(User *u, BotInfo *bi, const Anope::string &message)
	{
		if (u)
			u->SendMessage(bi, message);
	}

	void ReplyTo(const Anope::string &nick, BotInfo *bi, const Anope::string &message)
	{
		User *u = User::Find(nick);
		if (u)
			u->SendMessage(bi, message);
	}

	void QueueJob(std::function<void()> job)
	{
		{
			std::lock_guard<std::mutex> lk(this->mutex);
			this->jobs.push_back(std::move(job));
		}
		this->Notify();
	}

	void Usage(User *u, BotInfo *bi)
	{
		this->Reply(u, bi, "Syntax: ADD <ircchannel> <guild> <channel>");
		this->Reply(u, bi, "        SET <ircchannel> <guild> <channel>");
		this->Reply(u, bi, "        DEL <ircchannel>");
		this->Reply(u, bi, "        LIST");
		this->Reply(u, bi, "        GUILDS");
		this->Reply(u, bi, "        CHANNELS <guild>");
	}

	Bridge *FindBridge(const Anope::string &irc_channel) const
	{
		for (auto *bridge : this->bridges)
			if (bridge->irc_channel.equals_ci(irc_channel))
				return bridge;
		return nullptr;
	}

	Bridge *FindChannel(const Anope::string &foreign_channel) const
	{
		for (auto *bridge : this->bridges)
			if (bridge->foreign_channel.equals_ci(foreign_channel))
				return bridge;
		return nullptr;
	}

	/* ------------------------------------------------------------------ */
	/* Discord connection lifecycle                                         */
	/* ------------------------------------------------------------------ */

	void StartCluster()
	{
		if (this->core || this->token.empty())
			return;

		static constexpr uint32_t intents = dpp::i_guild_messages | dpp::i_message_content;
		this->core = new dpp::cluster(this->token.c_str(), intents);

		this->core->on_message_create([this](const dpp::message_create_t &event)
		{
			if (event.msg.author.is_bot())
				return;
			if (event.msg.content.empty() && event.msg.attachments.empty())
				return;

			Anope::string discord_id(event.msg.author.id.str().c_str());
			Anope::string display = !event.msg.author.global_name.empty()
				? Anope::string(event.msg.author.global_name)
				: Anope::string(event.msg.author.username);
			Anope::string message = event.msg.content;
			if (message.empty())
				message = "[attachment]";
			Anope::string channel(event.msg.channel_id.str().c_str());

			{
				std::lock_guard<std::mutex> lk(me->mutex);
				if (!me->FindChannel(channel))
					return;

				me->jobs.emplace_back([channel, discord_id, display, message]()
				{
					me->RelayToIrc(channel, discord_id, display, message);
				});
			}
			me->Notify();
		});

		this->thread = new BridgeThread(this->core);
		this->thread->Start();
		this->active_conn = true;
	}

	void StopCluster()
	{
		if (!this->core && !this->thread)
		{
			this->active_conn = false;
			return;
		}

		this->active_conn = false;
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
	/* IRC-side: juped bridge server                                       */
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

		Anope::string sid = IRCD->SID_Retrieve();
		this->discord_server = new Server(Me, this->server_name, "Discord bridge", sid, 1, true);
		IRCD->SendServer(this->discord_server);
		Log(this) << "BridgeServ: Introduced juped bridge server " << this->server_name;
		return true;
	}

	/* ------------------------------------------------------------------ */
	/* IRC-side: pseudo client management                                  */
	/* ------------------------------------------------------------------ */

	Anope::string MakeNick(const Anope::string &raw)
	{
		Anope::string nick;
		for (size_t i = 0; i < raw.length() && nick.length() < 24; ++i)
		{
			unsigned char ch = raw[i];
			if (std::isalnum(ch))
				nick += static_cast<char>(ch);
			else if (ch != ' ')
				nick += '_';
		}
		if (nick.empty())
			nick = "discord";
		if (!IRCD->IsNickValid(nick))
			nick = "d_" + nick;
		if (!IRCD->IsNickValid(nick))
			nick = "discord";

		Anope::string base = nick;
		unsigned suffix = 1;
		while (User::Find(nick))
		{
			nick = base;
			nick += "_";
			nick += Anope::ToString(++suffix);
		}
		return nick;
	}

	Anope::string MakeIdent(const Anope::string &discord_id)
	{
		unsigned char digest[SHA256_DIGEST_LENGTH];
		SHA256(reinterpret_cast<const unsigned char *>(discord_id.c_str()), discord_id.length(), digest);
		Anope::string raw(reinterpret_cast<const char *>(digest), SHA256_DIGEST_LENGTH);
		Anope::string hex = Anope::Hex(raw);
		if (hex.length() > 16)
			hex = hex.substr(0, 16);
		return hex;
	}

	DiscordUser *EnsureUser(const Anope::string &discord_id, const Anope::string &display_name)
	{
		auto it = this->discord_users.find(discord_id);
		if (it != this->discord_users.end())
			return it->second;

		if (!this->EnsureDiscordServer())
			return nullptr;

		Anope::string nick = this->MakeNick(display_name);
		Anope::string ident = this->MakeIdent(discord_id);

		Anope::string uid = IRCD->UID_Retrieve();
		User *user = new PseudoUser(nick, ident, this->discord_server->GetName(),
			this->discord_server, "Discord: " + display_name, uid);

		auto *du = new DiscordUser();
		du->discord_id = discord_id;
		du->nick = nick;
		du->user = user;

		this->discord_users[discord_id] = du;

		IRCD->SendClientIntroduction(user);
		Log(this) << "BridgeServ: Introduced pseudo client " << nick << " (" << discord_id << ")";
		return du;
	}

	void EnsureJoin(DiscordUser *du, const Anope::string &channel_name)
	{
		if (!du->user)
			return;

		Channel *chan = Channel::Find(channel_name);
		if (!chan)
		{
			bool created = false;
			chan = Channel::FindOrCreate(channel_name, created);
		}
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

	void PruneChannels()
	{
		std::vector<DiscordUser *> snapshot;
		snapshot.reserve(this->discord_users.size());
		for (auto &[_, du] : this->discord_users)
			snapshot.push_back(du);

		for (auto *du : snapshot)
		{
			std::vector<Anope::string> to_remove;
			for (const auto &ch : du->chans)
				if (!this->FindBridge(ch))
					to_remove.push_back(ch);
			for (const auto &ch : to_remove)
				this->PartPseudo(du, ch, "Bridge removed");
		}
	}

	void RemoveAllDiscordUsers(const Anope::string &reason)
	{
		std::vector<DiscordUser *> snapshot;
		snapshot.reserve(this->discord_users.size());
		for (auto &[_, du] : this->discord_users)
			snapshot.push_back(du);

		for (auto *du : snapshot)
		{
			if (!du->user)
			{
				delete du;
				continue;
			}
			if (IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced())
				IRCD->SendQuit(du->user, reason);
			du->user->Quit(reason);
			/* OnUserQuit will erase and delete the record for us. */
		}
		this->discord_users.clear();

		if (this->discord_server)
		{
			Server *server = this->discord_server;
			this->discord_server = nullptr;
			if (IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced())
				IRCD->SendSquit(server, reason);
			server->Delete(reason);
		}
	}

	/* ------------------------------------------------------------------ */
	/* Relay: Discord -> IRC                                               */
	/* ------------------------------------------------------------------ */

	void RelayToIrc(const Anope::string &foreign_channel, const Anope::string &discord_id,
		const Anope::string &display_name, Anope::string message)
	{
		if (!this->active_conn || !IRCD)
			return;

		Bridge *bridge = this->FindChannel(foreign_channel);
		if (!bridge || bridge->irc_channel.empty())
			return;

		DiscordUser *du = this->EnsureUser(discord_id, display_name);
		if (!du)
			return;

		this->EnsureJoin(du, bridge->irc_channel);

		if (message.length() > 400)
		{
			message = message.substr(0, 400);
			message += "...";
		}
		message = SanitizeMessage(message);

		IRCD->SendPrivmsg(du->user, bridge->irc_channel, message);
	}

	/* ------------------------------------------------------------------ */
	/* Webhook helpers (IRC -> Discord with per-user name)                 */
	/* ------------------------------------------------------------------ */

	void EnsureWebhook(Bridge *bridge)
	{
		if (!this->core || bridge->webhook_pending)
			return;

		bridge->webhook_pending = true;

		Anope::string key = bridge->irc_channel;
		dpp::webhook w;
		w.channel_id = bridge->ForeignId();
		w.name = "IRC bridge";

		this->core->create_webhook(w, [this, key](const dpp::confirmation_callback_t &cb)
		{
			if (cb.is_error())
			{
				this->QueueJob([key]()
				{
					Bridge *b = me->FindBridge(key);
					if (b)
						b->webhook_pending = false;
				});
				return;
			}

			dpp::webhook created;
			try
			{
				created = cb.get<dpp::webhook>();
			}
			catch (const dpp::exception &)
			{
				return;
			}

			Anope::string id(created.id.str().c_str());
			Anope::string tok(created.token.c_str());

			this->QueueJob([key, id, tok]()
			{
				Bridge *b = me->FindBridge(key);
				if (!b)
					return;
				b->webhook_id = id;
				b->webhook_token = tok;
				b->webhook_pending = false;
				b->QueueUpdate();
				Log(me) << "BridgeServ: Created Discord webhook " << id << " for " << key;
			});
		});
	}

	/* ------------------------------------------------------------------ */
	/* Relay: IRC -> Discord                                               */
	/* ------------------------------------------------------------------ */

	void SendIrcToDiscord(User *u, Bridge *bridge, const Anope::string &message)
	{
		if (!this->core || !this->active_conn)
			return;

		if (bridge->webhook_id.empty())
			this->EnsureWebhook(bridge);

		if (!bridge->webhook_id.empty())
		{
			try
			{
				dpp::webhook wh(dpp::snowflake(std::string(bridge->webhook_id.c_str())), bridge->webhook_token.c_str());
				wh.name = u->nick.c_str();
				dpp::message m(bridge->ForeignId(), message.c_str());
				this->core->execute_webhook(wh, m, false, 0, "", [](const dpp::confirmation_callback_t &) {});
				return;
			}
			catch (const dpp::exception &)
			{
			}
		}

		try
		{
			std::string payload = u->nick.c_str();
			payload += ": ";
			payload += message.c_str();
			this->core->message_create(dpp::message(bridge->ForeignId(), payload));
		}
		catch (const dpp::exception &)
		{
		}
	}

	/* ------------------------------------------------------------------ */
	/* Bridge lifecycle                                                    */
	/* ------------------------------------------------------------------ */

	void JoinChannel(Bridge *bridge)
	{
		auto *bi = this->GetClient();
		if (bi && !bridge->irc_channel.empty())
			bi->Join(bridge->irc_channel);
	}

	void AddBridge(Bridge *bridge)
	{
		{
			std::lock_guard<std::mutex> lk(this->mutex);
			this->bridges.push_back(bridge);
		}
		if (IRCD && Servers::GetUplink() && Servers::GetUplink()->IsSynced())
			this->JoinChannel(bridge);
	}

	/* ------------------------------------------------------------------ */
	/* Commands                                                            */
	/* ------------------------------------------------------------------ */

	void DoAdd(User *u, BotInfo *bi, const std::vector<Anope::string> &params)
	{
		if (params.size() < 4)
		{
			this->Usage(u, bi);
			return;
		}

		const Anope::string &irc_channel = params[1];
		const Anope::string &guild = params[2];
		const Anope::string &foreign_channel = params[3];

		if (!IRCD || !IRCD->IsChannelValid(irc_channel))
		{
			this->Reply(u, bi, "Invalid IRC channel name.");
			return;
		}

		if (this->FindBridge(irc_channel))
		{
			this->Reply(u, bi, "That IRC channel is already bridged.");
			return;
		}

		if (guild.empty() || !Anope::TryConvert<uint64_t>(guild))
		{
			this->Reply(u, bi, "Invalid Discord guild ID.");
			return;
		}

		if (foreign_channel.empty() || !Anope::TryConvert<uint64_t>(foreign_channel))
		{
			this->Reply(u, bi, "Invalid Discord channel ID.");
			return;
		}

		auto *bridge = new Bridge();
		bridge->irc_channel = irc_channel;
		bridge->guild = guild;
		bridge->foreign_channel = foreign_channel;

		this->AddBridge(bridge);
		bridge->QueueUpdate();

		this->Reply(u, bi, Anope::Format("Added bridge %s <-> guild %s channel %s.", irc_channel.c_str(), guild.c_str(), foreign_channel.c_str()));
	}

	void DoSet(User *u, BotInfo *bi, const std::vector<Anope::string> &params)
	{
		if (params.size() < 4)
		{
			this->Usage(u, bi);
			return;
		}

		const Anope::string &irc_channel = params[1];
		const Anope::string &guild = params[2];
		const Anope::string &foreign_channel = params[3];

		Bridge *bridge = this->FindBridge(irc_channel);
		if (!bridge)
		{
			this->Reply(u, bi, "No such bridge.");
			return;
		}

		if (guild.empty() || !Anope::TryConvert<uint64_t>(guild))
		{
			this->Reply(u, bi, "Invalid Discord guild ID.");
			return;
		}

		if (foreign_channel.empty() || !Anope::TryConvert<uint64_t>(foreign_channel))
		{
			this->Reply(u, bi, "Invalid Discord channel ID.");
			return;
		}

		bridge->guild = guild;
		bridge->foreign_channel = foreign_channel;
		bridge->webhook_id.clear();
		bridge->webhook_token.clear();
		bridge->QueueUpdate();

		this->PruneChannels();

		this->Reply(u, bi, Anope::Format("Updated bridge %s -> guild %s channel %s.", irc_channel.c_str(), guild.c_str(), foreign_channel.c_str()));
	}

	void DoDel(User *u, BotInfo *bi, const std::vector<Anope::string> &params)
	{
		if (params.size() < 2)
		{
			this->Usage(u, bi);
			return;
		}

		Bridge *bridge = this->FindBridge(params[1]);
		if (!bridge)
		{
			this->Reply(u, bi, "No such bridge.");
			return;
		}

		{
			std::lock_guard<std::mutex> lk(this->mutex);
			auto it = std::find(this->bridges.begin(), this->bridges.end(), bridge);
			if (it != this->bridges.end())
				this->bridges.erase(it);
		}

		delete bridge;

		this->PruneChannels();

		this->Reply(u, bi, "Bridge removed.");
	}

	void DoList(User *u, BotInfo *bi)
	{
		if (this->bridges.empty())
		{
			this->Reply(u, bi, "No bridges are configured.");
			return;
		}

		for (auto *bridge : this->bridges)
			this->Reply(u, bi, Anope::Format("[%s] %s <-> discord guild %s channel %s", this->active_conn ? "connected" : "stopped", bridge->irc_channel.c_str(), bridge->guild.c_str(), bridge->foreign_channel.c_str()));
	}

	void DoGuilds(User *u, BotInfo *bi)
	{
		if (!this->core || !this->active_conn)
		{
			this->Reply(u, bi, "Not connected to Discord.");
			return;
		}

		Anope::string nick = u->nick;

		this->core->current_user_get_guilds([this, nick, bi](const dpp::confirmation_callback_t &cb)
		{
			if (cb.is_error())
			{
				this->QueueJob([nick, bi]()
				{
					me->ReplyTo(nick, bi, "Failed to retrieve guilds from Discord.");
				});
				return;
			}

			dpp::guild_map guilds = cb.get<dpp::guild_map>();
			std::vector<Anope::string> lines;
			lines.reserve(guilds.size());
			for (const auto &g : guilds)
				lines.emplace_back(Anope::string(g.second.name.c_str()) + " (" + Anope::string(g.second.id.str().c_str()) + ")");

			this->QueueJob([nick, bi, lines]()
			{
				if (lines.empty())
					me->ReplyTo(nick, bi, "The bot is not present in any guilds.");
				else
					for (const auto &line : lines)
						me->ReplyTo(nick, bi, line);
			});
		});
	}

	void DoChannels(User *u, BotInfo *bi, const std::vector<Anope::string> &params)
	{
		if (params.size() < 2)
		{
			this->Usage(u, bi);
			return;
		}

		if (!this->core || !this->active_conn)
		{
			this->Reply(u, bi, "Not connected to Discord.");
			return;
		}

		const Anope::string &guild = params[1];
		if (guild.empty() || !Anope::TryConvert<uint64_t>(guild))
		{
			this->Reply(u, bi, "Invalid Discord guild ID.");
			return;
		}

		Anope::string nick = u->nick;

		this->core->channels_get(dpp::snowflake(guild.c_str()), [this, nick, bi](const dpp::confirmation_callback_t &cb)
		{
			if (cb.is_error())
			{
				this->QueueJob([nick, bi]()
				{
					me->ReplyTo(nick, bi, "Failed to retrieve channels from Discord.");
				});
				return;
			}

			dpp::channel_map channels = cb.get<dpp::channel_map>();
			std::vector<Anope::string> lines;
			lines.reserve(channels.size());
			for (const auto &c : channels)
			{
				if (c.second.get_type() != dpp::CHANNEL_TEXT && c.second.get_type() != dpp::CHANNEL_ANNOUNCEMENT)
					continue;
				lines.emplace_back(Anope::string(c.second.name.c_str()) + " (" + Anope::string(c.second.id.str().c_str()) + ")");
			}

			this->QueueJob([nick, bi, lines]()
			{
				if (lines.empty())
					me->ReplyTo(nick, bi, "No text channels found in that guild.");
				else
					for (const auto &line : lines)
						me->ReplyTo(nick, bi, line);
			});
		});
	}

public:
	ModuleBridgeServ(const Anope::string &modname, const Anope::string &creator)
		: Module(modname, creator, VENDOR)
	{
		me = this;

		this->SetAuthor("Anope");
		this->SetVersion("1.0");

		this->btype = new BridgeType();

		Configuration::Block &block = Config->GetModule(this);
		this->token = block.Get<const Anope::string>("token", "");
		this->server_name = block.Get<const Anope::string>("server", "discord.bridged");
		this->StartCluster();

		Log(this) << "BridgeServ: Loaded, using the vendored DPP (Discord++) library.";
	}

	~ModuleBridgeServ() override
	{
		this->StopCluster();

		{
			std::lock_guard<std::mutex> lk(this->mutex);
			this->jobs.clear();
		}

		this->RemoveAllDiscordUsers("Discord bridge shutting down");

		for (auto *bridge : this->bridges)
			delete bridge;
		this->bridges.clear();

		delete this->btype;
		this->btype = nullptr;

		me = nullptr;
	}

	void OnReload(Configuration::Conf &conf) override
	{
		(void)conf;

		Anope::string new_token = Config->GetModule(this).Get<const Anope::string>("token", "");
		if (new_token != this->token)
		{
			this->StopCluster();
			this->token = new_token;
			this->StartCluster();
		}
	}

	void OnServerConnect() override
	{
		for (auto *bridge : this->bridges)
			this->JoinChannel(bridge);
	}

	void OnNotify() override
	{
		for (;;)
		{
			std::function<void()> job;
			{
				std::lock_guard<std::mutex> lk(this->mutex);
				if (this->jobs.empty())
					break;
				job = std::move(this->jobs.front());
				this->jobs.pop_front();
			}
			job();
		}
	}

	void OnUserQuit(User *u, const Anope::string &msg) override
	{
		(void)msg;
		if (!this->discord_server || !u || u->server != this->discord_server)
			return;

		for (auto it = this->discord_users.begin(); it != this->discord_users.end(); ++it)
		{
			if (it->second->user == u)
			{
				delete it->second;
				this->discord_users.erase(it);
				break;
			}
		}
	}

	void OnPrivmsg(User *u, Channel *c, Anope::string &msg, const Anope::map<Anope::string> &tags) override
	{
		(void)tags;
		if (!u || !c || msg.empty())
			return;

		if (u == this->GetClient() || u->server == Me)
			return;
		if (this->discord_server && u->server == this->discord_server)
			return;
		if (!this->core || !this->active_conn)
			return;

		Bridge *bridge = this->FindBridge(c->name);
		if (!bridge)
			return;

		Anope::string message = msg;
		if (message.length() > 1800)
		{
			message = message.substr(0, 1800);
			message += "...";
		}
		message = SanitizeMessage(message);

		this->SendIrcToDiscord(u, bridge, message);
	}

	EventReturn OnBotPrivmsg(User *u, BotInfo *bi, Anope::string &message, const Anope::map<Anope::string> &tags) override
	{
		(void)tags;
		if (!u || !bi || message.empty())
			return EVENT_CONTINUE;

		spacesepstream ss(message);
		Anope::string command;
		ss.GetToken(command);
		command = command.upper();

		bool handled = command == "ADD" || command == "SET" || command == "DEL"
			|| command == "LIST" || command == "GUILDS" || command == "CHANNELS"
			|| command == "HELP";
		if (!handled)
			return EVENT_CONTINUE;

		if (!u->IsServicesOper())
		{
			this->Reply(u, bi, "Access denied.");
			return EVENT_STOP;
		}

		std::vector<Anope::string> params;
		{
			Anope::string token;
			while (ss.GetToken(token))
				params.push_back(token);
		}

		if (command == "ADD")
			this->DoAdd(u, bi, params);
		else if (command == "SET")
			this->DoSet(u, bi, params);
		else if (command == "DEL")
			this->DoDel(u, bi, params);
		else if (command == "LIST")
			this->DoList(u, bi);
		else if (command == "GUILDS")
			this->DoGuilds(u, bi);
		else if (command == "CHANNELS")
			this->DoChannels(u, bi, params);
		else
			this->Usage(u, bi);

		return EVENT_STOP;
	}
};

Serializable *BridgeType::Unserialize(Serializable *obj, Serialize::Data &data) const
{
	Bridge *bridge;
	if (obj)
		bridge = static_cast<Bridge *>(obj);
	else
	{
		bridge = new Bridge();
		if (me)
			me->AddBridge(bridge);
	}

	bridge->irc_channel = data.Load("irc-channel");
	bridge->guild = data.Load("guild");
	bridge->foreign_channel = data.Load("foreign-channel");
	bridge->webhook_id = data.Load("webhook-id");
	bridge->webhook_token = data.Load("webhook-token");
	return bridge;
}

MODULE_INIT(ModuleBridgeServ)
