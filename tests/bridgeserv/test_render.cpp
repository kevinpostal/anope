/* Unit tests for the Discord bridge text helpers. */

#include "check.h"
#include "modules/bridgeserv/render.h"

#include <chrono>

using namespace BridgeServ::Text;

static void TestMarkdownToIrc()
{
	CHECK_EQ(MarkdownToIrc("**b** _i_ ||s|| `c`"), std::string("\002b\002 \035i\035 \026s\026 \021c\021"));
	CHECK_EQ(MarkdownToIrc("__u__ ~~s~~ *i*"), std::string("\037u\037 \036s\036 \035i\035"));
	CHECK_EQ(MarkdownToIrc("***bi***"), std::string("\002bi\002"));

	// Block markers only apply at the start of a line.
	CHECK_EQ(MarkdownToIrc("## Title\n> quoted\n>>> block\nplain"), std::string("Title\nquoted\nblock\nplain"));
	CHECK_EQ(MarkdownToIrc("a > b"), std::string("a > b"));

	// Emphasis nests; code spans do not.
	CHECK_EQ(MarkdownToIrc("**bold _it_** `**raw**`"), std::string("\002bold \035it\035\002 \021**raw**\021"));

	// Unterminated delimiters are literal.
	CHECK_EQ(MarkdownToIrc("2 * 3 and _oops"), std::string("2 * 3 and _oops"));
	CHECK_EQ(MarkdownToIrc("**"), std::string("**"));

	// Fenced blocks keep their newlines.
	CHECK_EQ(MarkdownToIrc("```\nint x;\n```"), std::string("\021\nint x;\n\021"));

	// A long run of would-be delimiters must not go quadratic.
	const std::string stars(4000, '*');
	const auto started = std::chrono::steady_clock::now();
	CHECK_EQ(MarkdownToIrc(stars), stars);
	const auto took = std::chrono::steady_clock::now() - started;
	CHECK(took < std::chrono::seconds(1));
}

static void TestExpandTokens()
{
	const auto resolve = [](char kind, const std::string &id, const std::string &name) -> std::string
	{
		if (kind == '@' && id == "123")
			return "@nick";
		if (kind == 'e')
			return ":" + name + ":";
		if (kind == 't')
			return "<time " + id + ">";
		return "";
	};

	// Unknown tokens are left verbatim.
	CHECK_EQ(ExpandTokens("hi <@123> <#45>", resolve), std::string("hi @nick <#45>"));
	CHECK_EQ(ExpandTokens("<@!123>", resolve), std::string("@nick"));
	CHECK_EQ(ExpandTokens("<a:wave:99>", resolve), std::string(":wave:"));
	CHECK_EQ(ExpandTokens("<:wave:99>", resolve), std::string(":wave:"));
	CHECK_EQ(ExpandTokens("<t:0:f>", resolve), std::string("<time 0>"));

	// Angle brackets which are not tokens survive.
	CHECK_EQ(ExpandTokens("a < b and <unclosed", resolve), std::string("a < b and <unclosed"));
	CHECK_EQ(ExpandTokens("<@abc>", resolve), std::string("<@abc>"));
	CHECK_EQ(ExpandTokens("<@>", resolve), std::string("<@>"));
}

static void TestEscapeMarkdown()
{
	CHECK_EQ(MarkdownToIrc("@" + EscapeMarkdown("**mods**")), std::string("@**mods**"));
	CHECK_EQ(MarkdownToIrc(EscapeMarkdown("a\\b")), std::string("a\\b"));
	CHECK_EQ(MarkdownToIrc(EscapeMarkdown("# not a heading")), std::string("# not a heading"));
}

static void TestTruncate()
{
	CHECK_EQ(TruncateUtf8("\xc3\xa9\xc3\xa9", 3), std::string("\xc3\xa9"));
	CHECK_EQ(TruncateUtf8("\xc3\xa9\xc3\xa9", 4), std::string("\xc3\xa9\xc3\xa9"));
	CHECK_EQ(TruncateUtf8("\xe2\x82\xac!", 2), std::string(""));
	CHECK_EQ(TruncateUtf8("abcdef", 3), std::string("abc"));

	CHECK_EQ(TruncateCodePoints("h\xc3\xa9llo", 2), std::string("h\xc3\xa9"));
	CHECK_EQ(TruncateCodePoints("h\xc3\xa9llo", 10), std::string("h\xc3\xa9llo"));
	CHECK_EQ(TruncateCodePoints("h\xc3\xa9llo", 0), std::string(""));
	CHECK_EQ(TruncateCodePoints("\xe2\x82\xac\xe2\x82\xac", 1), std::string("\xe2\x82\xac"));
}

static void TestEscapeLineStart()
{
	CHECK_EQ(EscapeLineStart("# heading"), std::string("\\# heading"));
	CHECK_EQ(EscapeLineStart("- item"), std::string("\\- item"));
	CHECK_EQ(EscapeLineStart("+ item"), std::string("\\+ item"));
	CHECK_EQ(EscapeLineStart("12. x"), std::string("\\12. x"));
	CHECK_EQ(EscapeLineStart("12 x"), std::string("12 x"));
	CHECK_EQ(EscapeLineStart("a-b"), std::string("a-b"));
	CHECK_EQ(EscapeLineStart(""), std::string(""));
}

static void TestWebhookName()
{
	CHECK_EQ(WebhookName("discordfan", " (IRC)"), std::string("d1scordfan (IRC)"));
	CHECK_EQ(WebhookName("ClydeBot", " (IRC)"), std::string("C1ydeBot (IRC)"));
	CHECK_EQ(WebhookName("plain", ""), std::string("plain"));
	CHECK_EQ(WebhookName(std::string(90, 'x'), " (IRC)").length(), size_t(80));
}

static void TestSplitRelayLines()
{
	const std::vector<std::string> want = { "a", "b" };
	CHECK(SplitRelayLines("a\n\nb\n") == want);
	CHECK(SplitRelayLines("").empty());
	CHECK(SplitRelayLines("\n\n").empty());
}

static void TestFormatUnixTime()
{
	CHECK_EQ(FormatUnixTime("0"), std::string("1970-01-01 00:00 UTC"));
	CHECK_EQ(FormatUnixTime("1700000000"), std::string("2023-11-14 22:13 UTC"));
	CHECK_EQ(FormatUnixTime(""), std::string(""));
	CHECK_EQ(FormatUnixTime("abc"), std::string(""));
	CHECK_EQ(FormatUnixTime("12x"), std::string(""));
	CHECK_EQ(FormatUnixTime("-5"), std::string(""));
	CHECK_EQ(FormatUnixTime(" 5"), std::string(""));
}

static void RunTests()
{
	TestMarkdownToIrc();
	TestExpandTokens();
	TestEscapeMarkdown();
	TestTruncate();
	TestEscapeLineStart();
	TestWebhookName();
	TestSplitRelayLines();
	TestFormatUnixTime();
}

CHECK_MAIN()
