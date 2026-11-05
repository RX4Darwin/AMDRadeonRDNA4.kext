/*
 * trial.hpp
 * AMDRadeonRDNA4
 *
 * Options for one test boot. A test is armed from macOS with
 *
 *   sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial="armed rdna4-heads=3"
 *
 * The driver reads the variable the first time an option is asked for and
 * writes "used ..." back before it acts on any of them, so the boot after a
 * test that hangs or panics runs without the test's options. A boot-arg of
 * the same name always wins.
 *
 * The parsing is freestanding, for the host test; option() is the kernel's.
 */

#ifndef Trial_hpp
#define Trial_hpp

#include <stddef.h>
#include <stdint.h>

namespace Trial
{

constexpr size_t kMaxText = 256;
constexpr const char kArmed[] = "armed";

/* Whether `text` arms a test: its first word is "armed". */
inline bool armed(const char *text)
{
	const size_t n = sizeof(kArmed) - 1;

	if (!text)
		return false;

	while (*text == ' ')
		text++;

	for (size_t i = 0; i < n; i++) {
		if (text[i] != kArmed[i])
			return false;
	}

	return text[n] == 0 || text[n] == ' ';
}

/* The next space-separated word at or after *p: [begin, end). */
inline bool nextWord(const char *&p, const char *&begin, const char *&end)
{
	while (*p == ' ')
		p++;

	if (!*p)
		return false;

	begin = p;
	while (*p && *p != ' ')
		p++;

	end = p;

	return true;
}

/* The value of one digit in base 10 or 16, or -1. */
inline int digitValue(char c, bool hex)
{
	if (c >= '0' && c <= '9')
		return c - '0';

	if (hex && c >= 'a' && c <= 'f')
		return c - 'a' + 10;

	if (hex && c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

/* [v, end) as a 32-bit number: decimal, or hex with 0x. */
inline bool parseNumber(const char *v, const char *end, uint32_t &value)
{
	const bool hex = end - v > 2 && v[0] == '0' &&
			 (v[1] == 'x' || v[1] == 'X');
	uint64_t n = 0;

	if (hex)
		v += 2;

	if (v >= end)
		return false;

	for (; v < end; v++) {
		const int d = digitValue(*v, hex);

		if (d < 0)
			return false;
		n = n * (hex ? 16 : 10) + static_cast<uint64_t>(d);

		if (n > 0xFFFFFFFFull)
			return false;
	}

	value = static_cast<uint32_t>(n);

	return true;
}

/* Where the value of word [begin, end) starts if the word is "key=...". */
inline const char *valueOf(const char *begin, const char *end, const char *key)
{
	size_t k = 0;

	while (key[k] && begin + k < end && begin[k] == key[k])
		k++;

	if (key[k] || begin + k >= end || begin[k] != '=')
		return nullptr;

	return begin + k + 1;
}

/*
 * The value of `key` among the "key=value" words of `text`. False if the key
 * is absent or its value is not a number.
 */
inline bool find(const char *text, const char *key, uint32_t &value)
{
	const char *p = text, *begin, *end;

	if (!text || !key || !*key)
		return false;

	while (nextWord(p, begin, end)) {
		const char *v = valueOf(begin, end, key);

		if (v)
			return parseNumber(v, end, value);
	}

	return false;
}

#ifdef KERNEL
/*
 * This boot's value of `key`: the boot-arg if there is one, else the armed
 * test's. The first call consumes the test (writes "used ..." to NVRAM).
 */
bool option(const char *key, uint32_t &value);

/* Whether the variable could be read yet (NVRAM comes up after the first framebuffers). */
bool loaded();
#endif

} /* namespace Trial */

#endif /* Trial_hpp */
