/* Unit tests for the IRC-side relay arithmetic. */

#include "check.h"
#include "modules/bridgeserv/relay.h"

#include <map>
#include <string>

using namespace BridgeServ::Relay;

static void TestTake()
{
	TokenBucket bucket;

	// A fresh bucket allows a burst and nothing more.
	for (int i = 0; i < 6; ++i)
		CHECK(Take(bucket, 6, 4, 100));
	CHECK(!Take(bucket, 6, 4, 100));
	CHECK(!Take(bucket, 6, 4, 103));

	// Then exactly one line per interval.
	CHECK(Take(bucket, 6, 4, 104));
	CHECK(!Take(bucket, 6, 4, 104));

	// Unused intervals accumulate.
	CHECK(Take(bucket, 6, 4, 112));
	CHECK(Take(bucket, 6, 4, 112));
	CHECK(!Take(bucket, 6, 4, 112));

	// The bucket never grows beyond the burst.
	for (int i = 0; i < 6; ++i)
		CHECK(Take(bucket, 6, 4, 1000));
	CHECK(!Take(bucket, 6, 4, 1000));

	// A zero burst disables throttling.
	TokenBucket open;
	for (int i = 0; i < 100; ++i)
		CHECK(Take(open, 0, 4, 100));

	// A clock which goes backwards does not stall the bucket.
	TokenBucket skewed;
	for (int i = 0; i < 6; ++i)
		CHECK(Take(skewed, 6, 4, 100));
	CHECK(!Take(skewed, 6, 4, 90));
	CHECK(Take(skewed, 6, 4, 104));

	// A zero interval is treated as one second.
	TokenBucket fast;
	CHECK(Take(fast, 1, 0, 100));
	CHECK(!Take(fast, 1, 0, 100));
	CHECK(Take(fast, 1, 0, 101));
}

static void TestPayloadBudget()
{
	CHECK_EQ(PayloadBudget(30, 5), size_t(463));
	CHECK_EQ(PayloadBudget(400, 100), size_t(64));
	CHECK_EQ(PayloadBudget(0, 0), size_t(498));
}

static void TestSplitForWire()
{
	const auto chunks = SplitForWire("0123456789", 4, 100);
	const std::vector<std::string> want = { "0123", "4567", "89" };
	CHECK(chunks.lines == want);
	CHECK_EQ(chunks.dropped, size_t(0));

	// Chunks never split a UTF-8 sequence.
	const auto utf8 = SplitForWire("\xc3\xa9\xc3\xa9\xc3\xa9", 4, 100);
	const std::vector<std::string> want_utf8 = { "\xc3\xa9\xc3\xa9", "\xc3\xa9" };
	CHECK(utf8.lines == want_utf8);

	// The cap applies after chunking, so one long line can not exceed it.
	const auto capped = SplitForWire("0123456789\nabcdefghij", 4, 2);
	const std::vector<std::string> want_capped = { "0123", "4567" };
	CHECK(capped.lines == want_capped);
	CHECK_EQ(capped.dropped, size_t(4));

	const auto multi = SplitForWire("a\nb\nc\nd\ne", 400, 2);
	CHECK_EQ(multi.lines.size(), size_t(2));
	CHECK_EQ(multi.dropped, size_t(3));

	CHECK(SplitForWire("", 400, 8).lines.empty());
	CHECK(SplitForWire("\n\n", 400, 8).lines.empty());

	// A budget which can not fit one character drops the line rather than
	// looping forever.
	const auto tiny = SplitForWire("\xe2\x82\xac", 2, 8);
	CHECK(tiny.lines.empty());
}

static void TestSanitiseNick()
{
	CHECK_EQ(SanitiseNick("J\xc3\xb6hn D\xc3\xb8" "e", 30), std::string("J_hn_D_e"));
	CHECK_EQ(SanitiseNick("  ___ ", 30), std::string("bridge"));
	CHECK_EQ(SanitiseNick("", 30), std::string("bridge"));
	CHECK_EQ(SanitiseNick("abcdef", 3), std::string("abc"));
	CHECK_EQ(SanitiseNick("abc!!!", 30), std::string("abc"));
	CHECK_EQ(SanitiseNick("a b", 2), std::string("a"));

	// A name written entirely in a look-alike alphabet is still a name: a
	// roster which showed these as "bridge" would be useless. Mathematical
	// sans-serif bold "ACIDVEGAS" and fullwidth "dollx".
	CHECK_EQ(SanitiseNick("\xf0\x9d\x97\x94\xf0\x9d\x97\x96\xf0\x9d\x97\x9c\xf0\x9d\x97\x97"
			      "\xf0\x9d\x97\xa9\xf0\x9d\x97\x98\xf0\x9d\x97\x9a\xf0\x9d\x97\x94"
			      "\xf0\x9d\x97\xa6", 30), std::string("ACIDVEGAS"));
	CHECK_EQ(SanitiseNick("\xef\xbd\x84\xef\xbd\x8f\xef\xbd\x8c\xef\xbd\x8c\xef\xbd\x98", 30),
		 std::string("dollx"));

	// Mathematical digits fold too, and the letters Unicode moved out of
	// the block into Letterlike Symbols still resolve: script "L".
	CHECK_EQ(SanitiseNick("\xf0\x9d\x9f\x8f", 30), std::string("1"));
	CHECK_EQ(SanitiseNick("\xe2\x84\x92", 30), std::string("L"));

	// Decorated names keep the readable part rather than the decoration.
	CHECK_EQ(SanitiseNick("\xe2\x96\x88\xe2\x96\x93" "Deviance" "\xe2\x96\x93\xe2\x96\x88", 30),
		 std::string("Deviance"));

	// A truncated multi-byte sequence must not run off the end.
	CHECK_EQ(SanitiseNick("ab\xf0\x9d\x97", 30), std::string("ab"));
}

static void TestHistory()
{
	History history(2);
	CHECK_EQ(history.Size(), size_t(0));

	history.Remember("A", { 1, "a", "", "" });
	history.Remember("B", { 2, "b", "", "" });
	history.Remember("C", { 3, "c", "", "" });
	CHECK(history.Find("A") == nullptr);
	CHECK(history.Find("B") != nullptr);
	CHECK(history.Find("C") != nullptr);
	CHECK_EQ(history.Size(), size_t(2));

	// Updating an entry does not consume capacity.
	history.Remember("B", { 22, "bb", "", "" });
	CHECK_EQ(history.Size(), size_t(2));
	CHECK(history.Find("C") != nullptr);
	CHECK_EQ(history.Find("B")->hash, size_t(22));
	CHECK_EQ(history.Find("B")->preview, std::string("bb"));

	CHECK(history.Forget("B"));
	CHECK(!history.Forget("B"));
	CHECK(history.Find("B") == nullptr);
	CHECK_EQ(history.Size(), size_t(1));

	// A forgotten key does not count against later eviction.
	history.Remember("D", { 4, "d", "", "" });
	history.Remember("B", { 5, "b", "", "" });
	CHECK(history.Find("C") == nullptr);
	CHECK(history.Find("D") != nullptr);
	CHECK(history.Find("B") != nullptr);
}

static void TestTagValues()
{
	const std::string raw = "a;b c\\d\r\n";
	const std::string escaped = "a\\:b\\sc\\\\d\\r\\n";
	CHECK_EQ(EscapeTagValue(raw), escaped);
	CHECK_EQ(UnescapeTagValue(escaped), raw);
	CHECK_EQ(EscapeTagValue(""), std::string(""));

	// A parser is lenient where an encoder is strict: an unknown escape
	// yields the character and a trailing backslash is dropped.
	CHECK_EQ(UnescapeTagValue("\\x\\"), std::string("x"));
	CHECK_EQ(UnescapeTagValue("plain"), std::string("plain"));
}

static void TestTagLookup()
{
	// A client written before the reply tag was ratified sends the draft
	// name; its reply still has to be read.
	std::map<std::string, std::string> tags = { { "+draft/reply", "a\\sb" } };
	CHECK_EQ(TagValue(tags, { "+reply", "+draft/reply" }), std::string("a b"));
	CHECK_EQ(TagValue(tags, { "+draft/react" }), std::string(""));

	// The ratified name wins when a client sends both.
	tags["+reply"] = "ratified";
	CHECK_EQ(TagValue(tags, { "+reply", "+draft/reply" }), std::string("ratified"));

	// An empty value says as little as no tag at all, so the next name
	// still gets its turn.
	tags["+reply"] = "";
	CHECK_EQ(TagValue(tags, { "+reply", "+draft/reply" }), std::string("a b"));
}

static void TestRemoteMsgId()
{
	CHECK_EQ(RemoteMsgId("dc", "123"), std::string("dc-123"));

	std::string out;
	CHECK(ParseRemoteMsgId("dc-123", "dc", out));
	CHECK_EQ(out, std::string("123"));
	CHECK(!ParseRemoteMsgId("x~1~2", "dc", out));
	CHECK(!ParseRemoteMsgId("dc-", "dc", out));
	CHECK(!ParseRemoteMsgId("dcx-1", "dc", out));
	CHECK(!ParseRemoteMsgId("", "dc", out));
}

static void TestLinks()
{
	Links links(2);
	CHECK_EQ(links.Size(), size_t(0));

	links.Remember({ "i1", "r1", "chan", "", "alice", "one" });
	links.Remember({ "i2", "r2", "chan", "thread", "bob", "two" });
	links.Remember({ "i3", "r3", "chan", "", "carol", "three" });

	// The oldest link is evicted from both directions at once.
	CHECK(links.ByIrc("i1") == nullptr);
	CHECK(links.ByRemote("r1") == nullptr);
	CHECK_EQ(links.Size(), size_t(2));

	// Both lookups find the same entry.
	const auto *by_irc = links.ByIrc("i2");
	const auto *by_remote = links.ByRemote("r2");
	CHECK(by_irc != nullptr);
	CHECK(by_irc == by_remote);
	CHECK_EQ(by_irc->remote_thread, std::string("thread"));
	CHECK_EQ(by_irc->author, std::string("bob"));

	// Re-linking an IRC message replaces its remote side without
	// consuming capacity.
	links.Remember({ "i3", "r33", "chan", "", "carol", "three again" });
	CHECK_EQ(links.Size(), size_t(2));
	CHECK(links.ByRemote("r3") == nullptr);
	CHECK(links.ByRemote("r33") != nullptr);
	CHECK(links.ByIrc("i2") != nullptr);
}

static void RunTests()
{
	TestTake();
	TestPayloadBudget();
	TestSplitForWire();
	TestSanitiseNick();
	TestHistory();
	TestTagValues();
	TestTagLookup();
	TestRemoteMsgId();
	TestLinks();
}

CHECK_MAIN()
