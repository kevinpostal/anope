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

#include "modules/bridgeserv/render.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

/** The IRC-side arithmetic of the bridge: flood control, wire budgets,
 * nickname derivation and the edit/delete history.
 *
 * Nothing in here depends on Anope; the functions operate on std::string
 * only so that they can be exercised standalone.
 */
namespace BridgeServ::Relay
{
	/** The flood control state of one bridge. */
	struct TokenBucket final
	{
		unsigned tokens = 0;
		time_t refilled_at = 0;
		/* Lines refused since the last one which was relayed. */
		unsigned dropped = 0;
	};

	/** Consumes a token, refilling the bucket first.
	 *
	 * A bucket starts full, allowing a burst of `burst` lines, after which
	 * one more line is allowed every `interval` seconds; unused refills
	 * accumulate up to `burst` again.
	 *
	 * @param bucket The bucket to draw from.
	 * @param burst The size of the bucket; zero disables throttling.
	 * @param interval The seconds per refilled token; clamped to at least one.
	 * @param now The current time.
	 * @return Whether a token was available.
	 */
	inline bool Take(TokenBucket &bucket, unsigned burst, time_t interval, time_t now)
	{
		if (!burst)
			return true;

		const time_t secs = std::max<time_t>(interval, 1);
		if (!bucket.refilled_at || now < bucket.refilled_at)
		{
			// A fresh bucket, or the clock went backwards: never let a
			// refill be gated on a time which may not come for a while.
			bucket.tokens = bucket.refilled_at ? std::min(bucket.tokens, burst) : burst;
			bucket.refilled_at = now;
		}

		const time_t elapsed = now - bucket.refilled_at;
		if (elapsed >= secs)
		{
			const time_t refill = elapsed / secs;
			bucket.tokens = static_cast<unsigned>(std::min<time_t>(burst, bucket.tokens + refill));
			bucket.refilled_at += refill * secs;
		}

		if (!bucket.tokens)
			return false;

		--bucket.tokens;
		return true;
	}

	/** Calculates how many bytes of message payload fit on one IRC line.
	 * @param source_len The longest form the message source can take on the
	 *                   wire (a UID, or "nick!ident@host").
	 * @param target_len The length of the target channel name.
	 * @return The payload budget, never less than 64 bytes.
	 */
	inline size_t PayloadBudget(size_t source_len, size_t target_len)
	{
		/* ":<source> PRIVMSG <target> :<payload>\r\n" */
		const size_t overhead = 1 + source_len + 1 + 8 + target_len + 2;
		if (overhead + 64 >= 510)
			return 64;
		return 510 - overhead;
	}

	/** The lines of a message as they will be sent to IRC. */
	struct WireLines final
	{
		std::vector<std::string> lines;
		/* Wire lines beyond max_lines which were not kept. */
		size_t dropped = 0;
	};

	/** Splits a rendered message into wire lines.
	 *
	 * Each line of the message is cut into chunks of at most `budget`
	 * bytes on UTF-8 boundaries, and at most `max_lines` chunks are kept in
	 * total, so a single very long line can never exceed the cap.
	 *
	 * @param text The rendered message.
	 * @param budget The payload budget from PayloadBudget().
	 * @param max_lines The maximum number of wire lines to keep.
	 * @return The wire lines and the number which were dropped.
	 */
	inline WireLines SplitForWire(const std::string &text, size_t budget, size_t max_lines)
	{
		WireLines out;
		for (const auto &line : Text::SplitRelayLines(text))
		{
			for (size_t pos = 0; pos < line.length(); )
			{
				const std::string chunk = Text::TruncateUtf8(line.substr(pos), budget);
				if (chunk.empty())
					break; // the budget can not fit even one character.
				pos += chunk.length();

				if (out.lines.size() < max_lines)
					out.lines.push_back(chunk);
				else
					++out.dropped;
			}
		}
		return out;
	}

	/** Folds a code point onto the ASCII character it imitates.
	 *
	 * Display names are often written entirely in one of the look-alike
	 * alphabets: the fullwidth forms, or the mathematical alphanumerics
	 * that clients offer as "fancy" fonts. To a reader those are ASCII, so
	 * they are folded back rather than discarded — otherwise a name made
	 * only of them keeps nothing and collapses to "bridge", which is no
	 * use in a channel whose point is showing who is there.
	 *
	 * This is deliberately not a general transliteration: an accented
	 * Latin letter is a letter in its own right, not a disguised ASCII
	 * one, and still becomes an underscore.
	 *
	 * @return The ASCII character, or 0 if the code point imitates none.
	 */
	inline char FoldToAscii(unsigned long codepoint)
	{
		/* Halfwidth and Fullwidth Forms: a shifted copy of printable ASCII. */
		if (codepoint >= 0xFF01 && codepoint <= 0xFF5E)
			return static_cast<char>(codepoint - 0xFEE0);

		/* Mathematical Alphanumeric Symbols: consecutive styles of 52
		 * letters, A-Z then a-z. The slots of the letters which Unicode had
		 * already encoded in Letterlike Symbols are left reserved, so the
		 * arithmetic holds for every assigned code point in the range. */
		if (codepoint >= 0x1D400 && codepoint <= 0x1D6A3)
		{
			const unsigned long index = (codepoint - 0x1D400) % 52;
			return static_cast<char>(index < 26 ? 'A' + index : 'a' + (index - 26));
		}

		/* The same block's digits: five styles of 0-9. */
		if (codepoint >= 0x1D7CE && codepoint <= 0x1D7FF)
			return static_cast<char>('0' + (codepoint - 0x1D7CE) % 10);

		/* The reserved slots above: letters which live in Letterlike
		 * Symbols instead, and so are what a styled name actually uses. */
		switch (codepoint)
		{
			case 0x2102: case 0x212D:                       return 'C';
			case 0x210A:                                    return 'g';
			case 0x210B: case 0x210C: case 0x210D:          return 'H';
			case 0x210E:                                    return 'h';
			case 0x2110: case 0x2111:                       return 'I';
			case 0x2112:                                    return 'L';
			case 0x2113:                                    return 'l';
			case 0x2115:                                    return 'N';
			case 0x2119:                                    return 'P';
			case 0x211A:                                    return 'Q';
			case 0x211B: case 0x211C: case 0x211D:          return 'R';
			case 0x2124: case 0x2128:                       return 'Z';
			case 0x212C:                                    return 'B';
			case 0x212F:                                    return 'e';
			case 0x2130:                                    return 'E';
			case 0x2131:                                    return 'F';
			case 0x2133:                                    return 'M';
			case 0x2134:                                    return 'o';
			default:                                        return 0;
		}
	}

	/** Derives the base of an IRC nickname from a display name.
	 *
	 * ASCII letters and digits are kept, as are the look-alike alphabets
	 * FoldToAscii() understands; every other run of characters becomes a
	 * single underscore, trailing underscores are trimmed, and a name with
	 * nothing usable in it becomes "bridge".
	 *
	 * @param raw The display name.
	 * @param budget The maximum length of the result.
	 * @return The nickname base.
	 */
	inline std::string SanitiseNick(const std::string &raw, size_t budget)
	{
		std::string base;
		for (size_t pos = 0; pos < raw.length(); )
		{
			if (base.length() >= budget)
				break;

			/* Decoded by hand: the sanitiser is deliberately free of any
			 * dependency, and only the code point value is wanted. A
			 * malformed sequence is consumed one byte at a time and lands
			 * on the underscore path below. */
			const auto lead = static_cast<unsigned char>(raw[pos]);
			unsigned long codepoint = lead;
			size_t length = 1;
			if (lead >= 0xF0)
				length = 4, codepoint = lead & 0x07;
			else if (lead >= 0xE0)
				length = 3, codepoint = lead & 0x0F;
			else if (lead >= 0xC0)
				length = 2, codepoint = lead & 0x1F;

			if (length > 1)
			{
				if (pos + length > raw.length())
					length = 1, codepoint = lead;
				else
				{
					for (size_t i = 1; i < length; ++i)
					{
						const auto cont = static_cast<unsigned char>(raw[pos + i]);
						if ((cont & 0xC0) != 0x80)
						{
							length = 1;
							codepoint = lead;
							break;
						}
						codepoint = (codepoint << 6) | (cont & 0x3F);
					}
				}
			}
			pos += length;

			char kept = 0;
			if (codepoint < 0x80)
			{
				if (std::isalnum(static_cast<unsigned char>(codepoint)))
					kept = static_cast<char>(codepoint);
			}
			else
			{
				const char folded = FoldToAscii(codepoint);
				if (folded && std::isalnum(static_cast<unsigned char>(folded)))
					kept = folded;
			}

			if (kept)
				base.push_back(kept);
			else if (!base.empty() && base.back() != '_')
				base.push_back('_');
		}
		while (!base.empty() && base.back() == '_')
			base.pop_back();

		if (base.empty())
			base = "bridge";
		return base;
	}

	/** Escapes a value for an IRCv3 message tag.
	 *
	 * Anope writes tag values onto the wire as given, so every value the
	 * bridge puts on a tag goes through here and every value it reads back
	 * goes through UnescapeTagValue(). The table is the one from the
	 * message-tags specification: ';' -> "\:", ' ' -> "\s", '\' -> "\\",
	 * CR -> "\r", LF -> "\n".
	 */
	inline std::string EscapeTagValue(const std::string &raw)
	{
		std::string out;
		out.reserve(raw.length());
		for (const auto chr : raw)
		{
			switch (chr)
			{
				case ';':  out += "\\:"; break;
				case ' ':  out += "\\s"; break;
				case '\\': out += "\\\\"; break;
				case '\r': out += "\\r"; break;
				case '\n': out += "\\n"; break;
				default:   out.push_back(chr); break;
			}
		}
		return out;
	}

	/** The inverse of EscapeTagValue(). A backslash before any other
	 * character yields that character, and a trailing lone backslash is
	 * dropped, as the specification requires of a parser.
	 */
	inline std::string UnescapeTagValue(const std::string &escaped)
	{
		std::string out;
		out.reserve(escaped.length());
		for (size_t pos = 0; pos < escaped.length(); ++pos)
		{
			if (escaped[pos] != '\\')
			{
				out.push_back(escaped[pos]);
				continue;
			}
			if (++pos >= escaped.length())
				break;
			switch (escaped[pos])
			{
				case ':': out.push_back(';'); break;
				case 's': out.push_back(' '); break;
				case 'r': out.push_back('\r'); break;
				case 'n': out.push_back('\n'); break;
				default:  out.push_back(escaped[pos]); break;
			}
		}
		return out;
	}

	/** The unescaped value of the first of `names` which `tags` carries
	 * with a value, or an empty string when it carries none of them. An
	 * empty value says as little as an absent tag to every caller here, so
	 * a name which carries one is skipped over.
	 *
	 * Client tags get renamed when they leave draft: `+reply` is ratified
	 * and is what this bridge sends, but a client written against the
	 * draft sends `+draft/reply`, and both have to be read or that
	 * client's reply is silently dropped. The order of `names` is the
	 * precedence — pass the ratified name first.
	 */
	template <typename Map>
	inline std::string TagValue(const Map &tags, std::initializer_list<const char *> names)
	{
		for (const auto *name : names)
		{
			const auto it = tags.find(name);
			if (it == tags.end())
				continue;
			auto value = UnescapeTagValue(it->second.c_str());
			if (!value.empty())
				return value;
		}
		return {};
	}

	/** The IRC msgid the bridge stamps on a message it relayed from a
	 * remote network: "<protocol prefix>-<remote id>", "dc-<snowflake>"
	 * for Discord. The mapping is stateless in both directions, which is
	 * what lets a reply or a reaction to a relayed message be resolved
	 * long after the message itself has been forgotten.
	 */
	inline std::string RemoteMsgId(const std::string &protocol_prefix, const std::string &remote_id)
	{
		return protocol_prefix + "-" + remote_id;
	}

	/** Recovers the remote id from a msgid made by RemoteMsgId().
	 * @return Whether the msgid was one of ours for that protocol.
	 */
	inline bool ParseRemoteMsgId(const std::string &msgid, const std::string &protocol_prefix, std::string &remote_id)
	{
		const std::string prefix = protocol_prefix + "-";
		if (msgid.length() <= prefix.length() || msgid.compare(0, prefix.length(), prefix) != 0)
			return false;
		remote_id = msgid.substr(prefix.length());
		return true;
	}

	/** What was last relayed for each recent message, so that an edit is
	 * only relayed when something visible changed and a delete notice can
	 * quote the message. The oldest entry is evicted at capacity.
	 */
	class History final
	{
	public:
		struct Entry final
		{
			/* Covers the whole rendering, so an edit beyond the preview is
			 * still seen as a change. */
			size_t hash = 0;
			/* The first 120 code points of the rendering, which a delete
			 * notice quotes and a reply from IRC is decorated with. */
			std::string preview;
			/* Who sent it, and the remote thread it was sent in (empty
			 * when it was sent in the channel itself). */
			std::string author;
			std::string remote_thread;
		};

	private:
		const size_t capacity;
		std::unordered_map<std::string, Entry> entries;
		std::deque<std::string> order;

	public:
		explicit History(size_t cap = 256)
			: capacity(cap ? cap : 1)
		{
		}

		/** Records, or replaces, the entry for a message. */
		void Remember(const std::string &key, const Entry &entry)
		{
			const auto result = this->entries.emplace(key, entry);
			if (!result.second)
			{
				result.first->second = entry;
				return;
			}

			this->order.push_back(key);
			while (this->order.size() > this->capacity)
			{
				this->entries.erase(this->order.front());
				this->order.pop_front();
			}
		}

		/** Finds the entry for a message, if it is still remembered. */
		const Entry *Find(const std::string &key) const
		{
			const auto it = this->entries.find(key);
			return it == this->entries.end() ? nullptr : &it->second;
		}

		/** Forgets a message.
		 * @return Whether it was remembered.
		 */
		bool Forget(const std::string &key)
		{
			if (!this->entries.erase(key))
				return false;

			this->order.erase(std::find(this->order.begin(), this->order.end(), key));
			return true;
		}

		size_t Size() const
		{
			return this->entries.size();
		}
	};

	/** The messages which originated on IRC and were posted to a remote
	 * network, so that a later reply or reaction on either side can be
	 * mapped to the other. There is no stateless id for these: the remote
	 * network allocates the id when the message is posted. The oldest link
	 * is evicted at capacity.
	 */
	class Links final
	{
	public:
		struct Entry final
		{
			std::string irc_msgid;
			/* The id the remote network gave the message, and the channel
			 * and thread (empty when none) it was posted into. */
			std::string remote_id;
			std::string remote_channel;
			std::string remote_thread;
			/* The IRC nickname at the time, and the first 120 code points
			 * of the text, for quoting. */
			std::string author;
			std::string excerpt;
		};

	private:
		const size_t capacity;
		std::unordered_map<std::string, Entry> by_irc;
		/* remote id -> irc msgid */
		std::unordered_map<std::string, std::string> by_remote;
		std::deque<std::string> order;

		void Drop(const std::string &irc_msgid)
		{
			const auto it = this->by_irc.find(irc_msgid);
			if (it == this->by_irc.end())
				return;
			this->by_remote.erase(it->second.remote_id);
			this->by_irc.erase(it);
		}

	public:
		explicit Links(size_t cap = 1024)
			: capacity(cap ? cap : 1)
		{
		}

		/** Records, or replaces, the link of an IRC message. */
		void Remember(Entry entry)
		{
			const bool replaced = this->by_irc.count(entry.irc_msgid);
			if (replaced)
				this->Drop(entry.irc_msgid);

			this->by_remote[entry.remote_id] = entry.irc_msgid;
			const std::string key = entry.irc_msgid;
			this->by_irc.emplace(key, std::move(entry));
			if (replaced)
				return;

			this->order.push_back(key);
			while (this->order.size() > this->capacity)
			{
				this->Drop(this->order.front());
				this->order.pop_front();
			}
		}

		const Entry *ByIrc(const std::string &irc_msgid) const
		{
			const auto it = this->by_irc.find(irc_msgid);
			return it == this->by_irc.end() ? nullptr : &it->second;
		}

		const Entry *ByRemote(const std::string &remote_id) const
		{
			const auto it = this->by_remote.find(remote_id);
			return it == this->by_remote.end() ? nullptr : this->ByIrc(it->second);
		}

		size_t Size() const
		{
			return this->by_irc.size();
		}
	};
}
