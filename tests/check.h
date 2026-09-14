/*
 * Minimal unit test support for the dependency-free headers.
 *
 * A test file defines `static void RunTests()` and then invokes
 * CHECK_MAIN(). CHECK and CHECK_EQ report failures and keep running so a
 * single run shows every broken expectation; the process exits non-zero
 * when anything failed, which is what CTest keys on.
 */

#pragma once

#include <cstdio>
#include <string>

namespace Check
{
	inline int failures = 0;

	/* Render a string with control bytes escaped so IRC formatting codes
	 * and stray newlines are visible in a failure report.
	 */
	inline std::string Show(const std::string &s)
	{
		std::string out;
		out.reserve(s.size() + 2);
		out.push_back('"');
		for (unsigned char c : s)
		{
			if (c < 0x20)
			{
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\%03o", c);
				out += buf;
			}
			else
				out.push_back(static_cast<char>(c));
		}
		out.push_back('"');
		return out;
	}

	inline std::string Show(const char *s)
	{
		return Show(std::string(s));
	}

	inline std::string Show(bool v)
	{
		return v ? "true" : "false";
	}

	template<typename T>
	inline std::string Show(const T &v)
	{
		return std::to_string(v);
	}
}

#define CHECK(expr) \
	do \
	{ \
		if (!(expr)) \
		{ \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			++Check::failures; \
		} \
	} while (0)

#define CHECK_EQ(got, want) \
	do \
	{ \
		auto _g = (got); \
		auto _w = (want); \
		if (!(_g == _w)) \
		{ \
			std::printf("FAIL %s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #got, Check::Show(_g).c_str(), Check::Show(_w).c_str()); \
			++Check::failures; \
		} \
	} while (0)

#define CHECK_MAIN() \
	int main() \
	{ \
		RunTests(); \
		if (Check::failures) \
		{ \
			std::printf("%d check(s) failed\n", Check::failures); \
			return 1; \
		} \
		std::printf("ok\n"); \
		return 0; \
	}
