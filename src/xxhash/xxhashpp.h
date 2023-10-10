#pragma once

namespace utils { namespace utils_inner
	{
		uint64_t hash_code_raw_1(const void* data);
		uint64_t hash_code_raw_2(const void* data);
		uint64_t hash_code_raw_3(const void* data);
		uint64_t hash_code_raw_4(const void* data);
		uint64_t hash_code_raw_5(const void* data);
		uint64_t hash_code_raw_6(const void* data);
		uint64_t hash_code_raw_7(const void* data);
		uint64_t hash_code_raw_8(const void* data);
		uint64_t hash_code_raw_9(const void* data);
		uint64_t hash_code_raw_10(const void* data);
		uint64_t hash_code_raw_11(const void* data);
		uint64_t hash_code_raw_12(const void* data);
		uint64_t hash_code_raw_13(const void* data);
		uint64_t hash_code_raw_14(const void* data);
		uint64_t hash_code_raw_15(const void* data);
		uint64_t hash_code_raw_16(const void* data);
	}

	uint64_t hash_code_raw(const void* data, size_t size);

	template <size_t Size>
	uint64_t hash_code_tpl(const void* data)
	{
		__pragma(warning(push))
		__pragma(warning(disable:4127))
		static_assert(Size > 0, "Incorrect size");
		if (Size == 1) return utils_inner::hash_code_raw_1(data);
		if (Size == 2) return utils_inner::hash_code_raw_2(data);
		if (Size == 3) return utils_inner::hash_code_raw_3(data);
		if (Size == 4) return utils_inner::hash_code_raw_4(data);
		if (Size == 5) return utils_inner::hash_code_raw_5(data);
		if (Size == 6) return utils_inner::hash_code_raw_6(data);
		if (Size == 7) return utils_inner::hash_code_raw_7(data);
		if (Size == 8) return utils_inner::hash_code_raw_8(data);
		if (Size == 9) return utils_inner::hash_code_raw_9(data);
		if (Size == 10) return utils_inner::hash_code_raw_10(data);
		if (Size == 11) return utils_inner::hash_code_raw_11(data);
		if (Size == 12) return utils_inner::hash_code_raw_12(data);
		if (Size == 13) return utils_inner::hash_code_raw_13(data);
		if (Size == 14) return utils_inner::hash_code_raw_14(data);
		if (Size == 15) return utils_inner::hash_code_raw_15(data);
		if (Size == 16) return utils_inner::hash_code_raw_16(data);
		__pragma(warning(pop))
		return hash_code_raw(data, Size);
	}

	template <typename T>
		requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
	uint64_t hash_code_ref(const T& data)
	{
		return hash_code_tpl<sizeof(T)>(&data);
	}
}
