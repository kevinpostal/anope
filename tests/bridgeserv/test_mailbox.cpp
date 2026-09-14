/* Unit tests for the cross-thread job mailbox. */

#include "check.h"
#include "modules/bridgeserv/mailbox.h"

#include <atomic>
#include <thread>
#include <vector>

using BridgeServ::Async::Mailbox;

/* The owner is a plain counter; jobs add to it. */
using Counter = int;
using Box = Mailbox<Counter>;

static void TestOrder()
{
	Counter total = 0;
	int wakes = 0;
	Box box(&total, [&wakes] { ++wakes; });

	CHECK(box.IsAttached());
	CHECK(box.Post([](Counter *c) { *c = *c * 10 + 1; }));
	CHECK(box.Post([](Counter *c) { *c = *c * 10 + 2; }));
	CHECK(box.Post([](Counter *c) { *c = *c * 10 + 3; }));
	CHECK_EQ(wakes, 3);

	Box::Job job;
	while (box.Pop(job))
		job(&total);
	CHECK_EQ(total, 123);
	CHECK(!box.Pop(job));
	CHECK_EQ(box.TakeDropped(), size_t(0));
}

static void TestCapacity()
{
	Counter total = 0;
	Box box(&total, nullptr, 2);

	CHECK(box.Post([](Counter *c) { *c += 1; }));
	CHECK(box.Post([](Counter *c) { *c += 10; }));
	CHECK(box.Post([](Counter *c) { *c += 100; }));
	CHECK_EQ(box.TakeDropped(), size_t(1));
	CHECK_EQ(box.TakeDropped(), size_t(0));

	Box::Job job;
	while (box.Pop(job))
		job(&total);
	CHECK_EQ(total, 110);
}

static void TestWakeUnlocked()
{
	// A wake which drains the mailbox itself must not deadlock, which it
	// would if Post() held the lock while calling it.
	Counter total = 0;
	Box *self = nullptr;
	int wakes = 0;
	Box box(&total, [&]
	{
		++wakes;
		Box::Job job;
		while (self->Pop(job))
			job(&total);
	});
	self = &box;

	CHECK(box.Post([](Counter *c) { *c += 5; }));
	CHECK_EQ(wakes, 1);
	CHECK_EQ(total, 5);
}

static void TestDetach()
{
	Counter total = 0;
	int wakes = 0;
	Box box(&total, [&wakes] { ++wakes; });

	CHECK(box.Post([](Counter *c) { *c += 1; }));
	box.Detach();
	CHECK(!box.IsAttached());

	Box::Job job;
	CHECK(!box.Pop(job)); // the queue was emptied.
	CHECK(!box.Post([](Counter *c) { *c += 1; }));
	CHECK(!box.Pop(job));
	CHECK_EQ(wakes, 1);
	CHECK_EQ(total, 0);
}

static void TestThreads()
{
	Counter total = 0;
	std::atomic<int> wakes{ 0 };
	Box box(&total, [&wakes] { ++wakes; }, 100000);

	const int per_thread = 1000;
	const auto produce = [&box]
	{
		for (int i = 0; i < per_thread; ++i)
			box.Post([](Counter *c) { *c += 1; });
	};

	std::thread first(produce);
	std::thread second(produce);

	int seen = 0;
	while (seen < 2 * per_thread)
	{
		Box::Job job;
		if (!box.Pop(job))
		{
			std::this_thread::yield();
			continue;
		}
		job(&total);
		++seen;
	}

	first.join();
	second.join();

	CHECK_EQ(total, 2 * per_thread);
	CHECK_EQ(wakes.load(), 2 * per_thread);
	CHECK_EQ(box.TakeDropped(), size_t(0));

	Box::Job job;
	CHECK(!box.Pop(job));
}

static void RunTests()
{
	TestOrder();
	TestCapacity();
	TestWakeUnlocked();
	TestDetach();
	TestThreads();
}

CHECK_MAIN()
