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
			std::string preview;
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
}
