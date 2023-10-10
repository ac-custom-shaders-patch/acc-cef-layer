#include "stdafx.h"
#include "xxhashpp.h"

#define XXH_INLINE_ALL
#include <utility/xxhash/xxhash.h>

namespace utils { namespace utils_inner
	{
		uint64_t hash_code_raw_1(const void* data) { return XXH3_64bits(data, 1); }
		uint64_t hash_code_raw_2(const void* data) { return XXH3_64bits(data, 2); }
		uint64_t hash_code_raw_3(const void* data) { return XXH3_64bits(data, 3); }
		uint64_t hash_code_raw_4(const void* data) { return XXH3_64bits(data, 4); }
		uint64_t hash_code_raw_5(const void* data) { return XXH3_64bits(data, 5); }
		uint64_t hash_code_raw_6(const void* data) { return XXH3_64bits(data, 6); }
		uint64_t hash_code_raw_7(const void* data) { return XXH3_64bits(data, 7); }
		uint64_t hash_code_raw_8(const void* data) { return XXH3_64bits(data, 8); }
		uint64_t hash_code_raw_9(const void* data) { return XXH3_64bits(data, 9); }
		uint64_t hash_code_raw_10(const void* data) { return XXH3_64bits(data, 10); }
		uint64_t hash_code_raw_11(const void* data) { return XXH3_64bits(data, 11); }
		uint64_t hash_code_raw_12(const void* data) { return XXH3_64bits(data, 12); }
		uint64_t hash_code_raw_13(const void* data) { return XXH3_64bits(data, 13); }
		uint64_t hash_code_raw_14(const void* data) { return XXH3_64bits(data, 14); }
		uint64_t hash_code_raw_15(const void* data) { return XXH3_64bits(data, 15); }
		uint64_t hash_code_raw_16(const void* data) { return XXH3_64bits(data, 16); }
	}

	uint64_t hash_code_raw(const void* data, size_t size)
	{
		return XXH3_64bits(data, size);
	}
}
