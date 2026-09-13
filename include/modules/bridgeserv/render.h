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
#include <cstring>
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

	namespace Detail
	{
		/** Determines whether a delimiter occurs at the given offset. */
		inline bool Delim(const std::string &str, size_t pos, const char *delim)
		{
			return str.compare(pos, std::strlen(delim), delim) == 0;
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
}
