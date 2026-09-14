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

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

/** Cross-thread job delivery for the bridge protocols.
 *
 * Nothing in here depends on Anope; the mailbox is exercised standalone by
 * the unit tests and only the wake callback ties it to the main loop.
 */
namespace BridgeServ::Async
{
	/** A bounded queue of jobs for an owner which lives on another thread.
	 *
	 * Producers on any thread call Post(); the owner's thread drains the
	 * queue with Pop() and runs each job with the owner pointer. The owner
	 * calls Detach() before it is destroyed, after which every later Post()
	 * is discarded, so a producer which outlives the owner (a network
	 * library's thread pool, for example) can never reach freed memory as
	 * long as it holds the mailbox through a shared_ptr rather than the
	 * owner itself.
	 */
	template<typename Owner>
	class Mailbox final
	{
	public:
		using Job = std::function<void(Owner *)>;

	private:
		mutable std::mutex mutex;
		Owner *owner;
		std::function<void()> wake;
		const size_t capacity;
		std::deque<Job> jobs;
		size_t dropped = 0;

	public:
		/** Creates a mailbox.
		 * @param o The owner which the jobs run against.
		 * @param w Called, unlocked, after every accepted Post() so the
		 *          owner's thread can be woken. Must be safe to call from
		 *          any thread.
		 * @param cap The maximum number of queued jobs; the oldest is
		 *            dropped when a Post() would exceed it.
		 */
		Mailbox(Owner *o, std::function<void()> w, size_t cap = 4096)
			: owner(o)
			, wake(std::move(w))
			, capacity(cap ? cap : 1)
		{
		}

		Mailbox(const Mailbox &) = delete;
		Mailbox &operator=(const Mailbox &) = delete;

		/** Queues a job. Safe to call from any thread.
		 * @return false if the mailbox is detached and the job was discarded.
		 */
		bool Post(Job job)
		{
			std::function<void()> notify;
			{
				std::lock_guard<std::mutex> lock(this->mutex);
				if (!this->owner)
					return false;

				if (this->jobs.size() >= this->capacity)
				{
					this->jobs.pop_front();
					++this->dropped;
				}
				this->jobs.push_back(std::move(job));
				notify = this->wake;
			}

			// The wake runs outside the lock: it is typically a write to a
			// self-pipe, and the consumer may call Pop() from inside it.
			if (notify)
				notify();
			return true;
		}

		/** Takes the next queued job, if there is one. The caller runs it
		 * without the mailbox locked.
		 */
		bool Pop(Job &job)
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			if (this->jobs.empty())
				return false;

			job = std::move(this->jobs.front());
			this->jobs.pop_front();
			return true;
		}

		/** Takes and resets the number of jobs dropped because the mailbox
		 * was full.
		 */
		size_t TakeDropped()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return std::exchange(this->dropped, 0);
		}

		/** Detaches the owner; the queue is emptied and every later Post()
		 * is discarded.
		 */
		void Detach()
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			this->owner = nullptr;
			this->wake = nullptr;
			this->jobs.clear();
		}

		/** Whether the owner is still attached. */
		bool IsAttached() const
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			return this->owner != nullptr;
		}
	};
}
