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

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

/** Pure text helpers for the Discord bridge.
 *
 * Nothing in here depends on Anope or on DPP; the functions operate on
 * std::string only so that they can be exercised standalone and so that they
 * are safe to call from the DPP thread pool (no shared state at all).
 */
namespace BridgeServ::Text
{
	/* mIRC formatting control characters. */
	static constexpr char BOLD = '\x02';
	static constexpr char MONOSPACE = '\x11';
	static constexpr char REVERSE = '\x16';
	static constexpr char STRIKETHROUGH = '\x1e';
	static constexpr char ITALIC = '\x1d';
	static constexpr char UNDERLINE = '\x1f';

	/** Truncates a string to at most the given number of bytes without ever
	 * leaving a partial UTF-8 sequence at the end.
	 * @param str The string to truncate.
	 * @param maxbytes The maximum number of bytes to keep.
	 * @return The truncated string.
	 */
	inline std::string TruncateUtf8(const std::string &str, size_t maxbytes)
	{
		if (str.length() <= maxbytes)
			return str;

		size_t len = maxbytes;

		// Rewind to the start of the sequence which the cut landed inside of.
		size_t lead = len;
		while (lead > 0 && (static_cast<unsigned char>(str[lead - 1]) & 0xc0) == 0x80)
			--lead;

		if (lead > 0)
		{
			const auto ch = static_cast<unsigned char>(str[lead - 1]);
			size_t expected = 1;
			if ((ch & 0xf8) == 0xf0)
				expected = 4;
			else if ((ch & 0xf0) == 0xe0)
				expected = 3;
			else if ((ch & 0xe0) == 0xc0)
				expected = 2;

			// If the sequence starting at lead - 1 did not fit entirely within
			// the budget then drop it in its entirety.
			if (expected > len - (lead - 1))
				len = lead - 1;
		}
		return str.substr(0, len);
	}

	/** Truncates a string to at most the given number of Unicode code
	 * points. Discord's message limits count characters, not bytes.
	 * @param str The string to truncate.
	 * @param maxchars The maximum number of code points to keep.
	 * @return The truncated string.
	 */
	inline std::string TruncateCodePoints(const std::string &str, size_t maxchars)
	{
		size_t chars = 0;
		for (size_t pos = 0; pos < str.length(); ++pos)
		{
			// Every code point starts with exactly one non-continuation byte.
			if ((static_cast<unsigned char>(str[pos]) & 0xc0) == 0x80)
				continue;
			if (chars == maxchars)
				return str.substr(0, pos);
			++chars;
		}
		return str;
	}

	/** Escapes a Markdown block marker at the start of a line.
	 *
	 * Inline escaping covers the emphasis characters but not headings,
	 * list items, or quotes, so a message starting with "# " or "1. "
	 * would otherwise be rendered by Discord as a block.
	 *
	 * @param str The message text.
	 * @return The text with a leading block marker escaped.
	 */
	inline std::string EscapeLineStart(const std::string &str)
	{
		if (str.empty())
			return str;

		bool marker = str[0] == '#' || str[0] == '-' || str[0] == '+';
		if (!marker && std::isdigit(static_cast<unsigned char>(str[0])))
		{
			size_t pos = 1;
			while (pos < str.length() && std::isdigit(static_cast<unsigned char>(str[pos])))
				++pos;
			marker = pos < str.length() && str[pos] == '.';
		}

		if (!marker)
			return str;
		return "\\" + str;
	}

	namespace Detail
	{
		/** Determines whether a delimiter occurs at the given offset. */
		inline bool Delim(const std::string &str, size_t pos, const char *delim)
		{
			return str.compare(pos, std::strlen(delim), delim) == 0;
		}

		/** Determines whether a character may appear in an IRC nickname.
		 *
		 * RFC 2812's nickname characters plus the ones InspIRCd allows.
		 * The ASCII ranges are spelled out rather than using std::isalnum
		 * so the result can never depend on the locale: a UTF-8 lead byte
		 * must not be absorbed into a nickname. Anything a bridge can
		 * actually hand out is covered, because SanitiseNick() emits only
		 * [A-Za-z0-9_] and MakeNick() rejects anything IRCD->IsNickValid()
		 * refuses.
		 */
		inline bool NickChar(unsigned char chr)
		{
			if ((chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z')
				|| (chr >= '0' && chr <= '9'))
				return true;
			return chr && std::strchr("-_[]\\`^{}|", chr) != nullptr;
		}

		/** Copies a verbatim (unparsed) span into the output. */
		inline void CopyRaw(const std::string &str, size_t begin, size_t end, std::string &out)
		{
			out.append(str, begin, end - begin);
		}

		inline void Render(const std::string &str, size_t begin, size_t end, std::string &out);

		/** Renders a paired-delimiter span if one is closed within the range.
		 * @return true if a span was consumed, in which case pos has been
		 *         advanced past the closing delimiter.
		 */
		inline bool Span(const std::string &str, size_t &pos, size_t end, std::string &out,
			const char *delim, char control, bool parse_inner)
		{
			const size_t dlen = std::strlen(delim);
			if (pos + dlen > end || !Delim(str, pos, delim))
				return false;

			const size_t inner = pos + dlen;
			const size_t close = str.find(delim, inner);
			if (close == std::string::npos || close >= end || close == inner)
				return false;

			out.push_back(control);
			if (parse_inner)
				Render(str, inner, close, out);
			else
				CopyRaw(str, inner, close, out);
			out.push_back(control);

			pos = close + dlen;
			return true;
		}

		inline void Render(const std::string &str, size_t begin, size_t end, std::string &out)
		{
			bool line_start = begin == 0;
			for (size_t pos = begin; pos < end; )
			{
				if (line_start)
				{
					line_start = false;

					// Block quotes: ">>> text" and "> text".
					size_t skip = pos;
					if (Delim(str, skip, ">>> "))
						skip += 3;
					else if (Delim(str, skip, "> "))
						skip += 1;
					else
					{
						// Headings: "# text" through "###### text".
						size_t hashes = skip;
						while (hashes < end && str[hashes] == '#' && hashes - skip < 6)
							++hashes;
						if (hashes > skip && hashes < end && str[hashes] == ' ')
							skip = hashes;
					}

					if (skip != pos)
					{
						while (skip < end && str[skip] == ' ')
							++skip;
						pos = skip;
						continue;
					}
				}

				if (str[pos] == '\n')
				{
					out.push_back('\n');
					line_start = true;
					++pos;
					continue;
				}

				// Only a handful of characters can open a span or an escape;
				// skip the per-delimiter searches for everything else.
				if (str[pos] == '\0' || std::strchr("`|*_~\\", str[pos]) == nullptr)
				{
					out.push_back(str[pos]);
					++pos;
					continue;
				}

				// Code spans are copied verbatim; everything else may nest.
				if (Span(str, pos, end, out, "```", MONOSPACE, false)
					|| Span(str, pos, end, out, "``", MONOSPACE, false)
					|| Span(str, pos, end, out, "`", MONOSPACE, false)
					|| Span(str, pos, end, out, "||", REVERSE, true)
					|| Span(str, pos, end, out, "***", BOLD, true)
					|| Span(str, pos, end, out, "**", BOLD, true)
					|| Span(str, pos, end, out, "__", UNDERLINE, true)
					|| Span(str, pos, end, out, "~~", STRIKETHROUGH, true)
					|| Span(str, pos, end, out, "*", ITALIC, true)
					|| Span(str, pos, end, out, "_", ITALIC, true))
				{
					continue;
				}

				// An escaped markdown character is emitted without its escape.
				if (str[pos] == '\\' && pos + 1 < end && !std::isalnum(static_cast<unsigned char>(str[pos + 1])))
				{
					out.push_back(str[pos + 1]);
					pos += 2;
					continue;
				}

				out.push_back(str[pos]);
				++pos;
			}
		}
	}

	/** Converts Discord flavoured Markdown into mIRC formatting.
	 * @param str The Discord message content.
	 * @return The message with IRC formatting control characters.
	 */
	inline std::string MarkdownToIrc(const std::string &str)
	{
		std::string out;
		out.reserve(str.length());
		Detail::Render(str, 0, str.length(), out);
		return out;
	}

	/** Escapes the Markdown characters in a literal string.
	 *
	 * Text which is substituted into a message before MarkdownToIrc runs (a
	 * resolved display name, for example) must be escaped, otherwise a name
	 * like "**mods**" would be rendered as IRC formatting.
	 *
	 * @param str The literal text.
	 * @return The text with its Markdown characters escaped.
	 */
	inline std::string EscapeMarkdown(const std::string &str)
	{
		std::string out;
		out.reserve(str.length());
		for (const auto chr : str)
		{
			switch (chr)
			{
				case '*':
				case '_':
				case '`':
				case '~':
				case '|':
				case '>':
				case '#':
				case '\\':
					out.push_back('\\');
					break;
			}
			out.push_back(chr);
		}
		return out;
	}

	/** Expands the Discord mention, emoji, and timestamp tokens in a message.
	 *
	 * The resolver is called with the token kind ('@' for users, '&' for
	 * roles, '#' for channels, 'e' for custom emoji, and 't' for timestamps),
	 * the token identifier, and the token name if it carries one. It returns
	 * the complete replacement text for the token. Returning an empty string
	 * leaves the token in the message verbatim.
	 *
	 * @param str The Discord message content.
	 * @param resolve The token resolver.
	 * @return The message with its tokens expanded.
	 */
	inline std::string ExpandTokens(const std::string &str, const std::function<std::string(char kind, const std::string &id, const std::string &name)> &resolve)
	{
		static const auto numeric = [](const std::string &val)
		{
			if (val.empty())
				return false;
			for (const auto chr : val)
			{
				if (chr < '0' || chr > '9')
					return false;
			}
			return true;
		};

		std::string out;
		out.reserve(str.length());

		for (size_t pos = 0; pos < str.length(); )
		{
			if (str[pos] != '<')
			{
				out.push_back(str[pos]);
				++pos;
				continue;
			}

			const size_t close = str.find('>', pos + 1);
			if (close == std::string::npos)
			{
				out.append(str, pos, std::string::npos);
				break;
			}

			// The token body without its enclosing angle brackets.
			const std::string body = str.substr(pos + 1, close - pos - 1);
			char kind = 0;
			std::string id;
			std::string name;

			if (body.compare(0, 2, "@&") == 0)
			{
				kind = '&';
				id = body.substr(2);
			}
			else if (body.compare(0, 2, "@!") == 0)
			{
				kind = '@';
				id = body.substr(2);
			}
			else if (!body.empty() && body[0] == '@')
			{
				kind = '@';
				id = body.substr(1);
			}
			else if (!body.empty() && body[0] == '#')
			{
				kind = '#';
				id = body.substr(1);
			}
			else if (body.compare(0, 2, "t:") == 0)
			{
				kind = 't';
				const size_t sep = body.find(':', 2);
				if (sep == std::string::npos)
					id = body.substr(2);
				else
				{
					id = body.substr(2, sep - 2);
					name = body.substr(sep + 1);
				}
			}
			else
			{
				// Custom emoji: ":name:id" or animated "a:name:id".
				size_t offset = 0;
				if (body.compare(0, 2, "a:") == 0)
					offset = 2;
				else if (!body.empty() && body[0] == ':')
					offset = 1;

				if (offset)
				{
					const size_t sep = body.find(':', offset);
					if (sep != std::string::npos)
					{
						kind = 'e';
						name = body.substr(offset, sep - offset);
						id = body.substr(sep + 1);
					}
				}
			}

			std::string replacement;
			if (kind && numeric(id))
				replacement = resolve(kind, id, name);

			if (replacement.empty())
			{
				// Not a token we understand; leave it alone.
				out.push_back('<');
				++pos;
				continue;
			}

			out.append(replacement);
			pos = close + 1;
		}
		return out;
	}

	/** Renders the "@nick" mentions of an IRC line for a network which has
	 * a mention syntax of its own.
	 *
	 * A mention is an '@' at the start of the line or after a character
	 * which cannot appear in a nickname, followed by a run of nickname
	 * characters; "user@host" is therefore not a mention of "host".
	 *
	 * The runs between mentions are passed to `escape` and each nickname to
	 * `resolve`, so a replacement reaches the remote network verbatim while
	 * everything around it is escaped exactly as the whole body would have
	 * been. A resolver which returns an empty string leaves the "@nick" as
	 * escaped literal text, which is what an unknown nickname must do.
	 *
	 * @param str The message text, with IRC formatting already removed.
	 * @param resolve Called with a nickname; returns its replacement.
	 * @param escape Called with each run of literal text.
	 * @return The rendered message.
	 */
	inline std::string ExpandMentions(const std::string &str,
		const std::function<std::string(const std::string &nick)> &resolve,
		const std::function<std::string(const std::string &text)> &escape)
	{
		std::string out;
		out.reserve(str.length());
		size_t literal = 0;
		for (size_t pos = 0; pos < str.length(); ++pos)
		{
			if (str[pos] != '@')
				continue;
			if (pos && Detail::NickChar(static_cast<unsigned char>(str[pos - 1])))
				continue;

			size_t end = pos + 1;
			while (end < str.length() && Detail::NickChar(static_cast<unsigned char>(str[end])))
				++end;
			if (end == pos + 1)
				continue;

			const std::string replacement = resolve(str.substr(pos + 1, end - pos - 1));
			if (replacement.empty())
				continue;

			out += escape(str.substr(literal, pos - literal));
			out += replacement;
			literal = end;
			pos = end - 1;
		}
		out += escape(str.substr(literal));
		return out;
	}

	/** Builds a webhook username for an IRC user.
	 *
	 * Discord rejects webhook usernames which contain "discord" or "clyde"
	 * and truncates them at 80 characters. Both words are broken by replacing
	 * their second character with a digit, which keeps the original casing of
	 * the nickname intact.
	 *
	 * @param nick The nickname of the IRC user.
	 * @param suffix The suffix which marks the user as coming from IRC.
	 * @return A username which Discord will accept.
	 */
	inline std::string WebhookName(const std::string &nick, const std::string &suffix)
	{
		static const char *const banned[] = { "discord", "clyde" };

		std::string name = nick;
		for (const auto *word : banned)
		{
			const size_t len = std::strlen(word);
			for (size_t pos = 0; pos + len <= name.length(); )
			{
				bool match = true;
				for (size_t idx = 0; idx < len; ++idx)
				{
					if (std::tolower(static_cast<unsigned char>(name[pos + idx])) != word[idx])
					{
						match = false;
						break;
					}
				}

				if (!match)
				{
					++pos;
					continue;
				}

				name[pos + 1] = '1';
				pos += len;
			}
		}
		return TruncateUtf8(name + suffix, 80);
	}

	/** Splits a rendered message into the lines to relay to IRC.
	 * @param str The rendered message.
	 * @return The non-empty lines of the message.
	 */
	inline std::vector<std::string> SplitRelayLines(const std::string &str)
	{
		std::vector<std::string> lines;
		for (size_t pos = 0; pos < str.length(); )
		{
			const size_t eol = str.find('\n', pos);
			const size_t len = (eol == std::string::npos ? str.length() : eol) - pos;
			if (len)
				lines.push_back(str.substr(pos, len));
			if (eol == std::string::npos)
				break;
			pos = eol + 1;
		}
		return lines;
	}

	/** Formats a Unix timestamp as a UTC time.
	 * @param unix_seconds The timestamp as a decimal string.
	 * @return The time as "YYYY-MM-DD HH:MM UTC", or an empty string if the
	 *         timestamp is not a non-negative integer.
	 */
	inline std::string FormatUnixTime(const std::string &unix_seconds)
	{
		if (unix_seconds.empty() || !std::isdigit(static_cast<unsigned char>(unix_seconds[0])))
			return "";

		char *end = nullptr;
		const long long when = std::strtoll(unix_seconds.c_str(), &end, 10);
		if (*end || when < 0)
			return "";

		const std::time_t raw = static_cast<std::time_t>(when);
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
}
