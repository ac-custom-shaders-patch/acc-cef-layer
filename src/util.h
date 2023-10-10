#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <stdint.h>
#include <string>
#include <type_traits>

#include <include/internal/cef_string.h>
#include <include/internal/cef_time.h>

#include "composition.h"

void __log_message(const char*, ...);

template <typename... Args>
void __log_nothing(Args&& ...args) {}

#ifdef _DEBUG
#define log_message(...) __log_message(__VA_ARGS__)
#else
#define log_message(...) __log_nothing(__VA_ARGS__)
#endif

template<class T>
std::shared_ptr<T> to_com_ptr(T* obj)
{
	return std::shared_ptr<T>(obj, [](T* p) { if (p) p->Release(); });
}

struct noncopyable
{
protected:
	constexpr noncopyable() = default;
	~noncopyable() = default;
public:
	noncopyable(const noncopyable&) = delete;
	noncopyable& operator=(const noncopyable&) = delete;
};

extern std::chrono::high_resolution_clock::time_point time_start;

inline double time_now_ms()
{
	return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - time_start).count()) / 1e6;
}

#define ACCSP_FRAME_SIZE (128 * 1024)
#define ACCSP_MAX_COMMAND_SIZE (16 * 1024)

struct accsp_wb_entry
{
	struct vec2
	{
		float x, y;
		auto operator<=>(const vec2&) const = default;
	};

	uint64_t be_alive_time;
	float zoom_level;
	uint32_t _pad0;

	uint64_t handle;
	uint64_t popup_handle;
	std::array<float, 4> popup_dimensions;

	uint32_t width;
	uint32_t height;

	uint16_t loading_progress;
	uint8_t cursor;
	uint8_t audio_peak;
	uint32_t fe_flags;

	uint16_t mouse_x;
	uint16_t mouse_y;
	int16_t mouse_wheel;
    uint8_t mouse_flags;    
    uint8_t needs_next_frame;
	
	uint32_t be_flags;
	uint16_t _pad1;
	uint8_t _pad2;
	uint8_t _pad3;
	std::array<vec2, 2> touches;

	float scroll_x;
	float scroll_y;
	
	uint32_t commands_set;
	uint32_t response_set;
	char commands[ACCSP_FRAME_SIZE];
	char response[ACCSP_FRAME_SIZE];
};

static_assert(sizeof(accsp_wb_entry) == 112 + 2 * ACCSP_FRAME_SIZE);

namespace utils
{
	template <size_t Size>
	bool tpl_equals(const void* a, const void* b)
	{
		__pragma(warning(push))
		__pragma(warning(disable:4127))
		if (Size == 0) return true;
		if (Size == 1) return *(char*)a == *(char*)b;
		if (Size == 2) return *(uint16_t*)a == *(uint16_t*)b;
		if (Size == 3) return *(uint16_t*)a == *(uint16_t*)b && ((char*)a)[2] == ((char*)b)[2];
		if (Size == 4) return *(uint32_t*)a == *(uint32_t*)b;
		if (Size == 5) return *(uint32_t*)a == *(uint32_t*)b && ((char*)a)[4] == ((char*)b)[4];
		if (Size == 6) return *(uint32_t*)a == *(uint32_t*)b && ((uint16_t*)a)[2] == ((uint16_t*)b)[2];
		if (Size == 7) return *(uint32_t*)a == *(uint32_t*)b && tpl_equals<3>(&((char*)a)[4], &((char*)b)[4]);
		if (Size == 8) return *(uint64_t*)a == *(uint64_t*)b;
		if (Size == 9) return *(uint64_t*)a == *(uint64_t*)b && ((char*)a)[8] == ((char*)b)[8];
		if (Size == 10) return *(uint64_t*)a == *(uint64_t*)b && ((uint16_t*)a)[4] == ((uint16_t*)b)[4];
		if (Size == 11) return *(uint64_t*)a == *(uint64_t*)b && tpl_equals<3>(&((char*)a)[8], &((char*)b)[8]);
		if (Size == 12) return *(uint64_t*)a == *(uint64_t*)b && ((uint32_t*)a)[2] == ((uint32_t*)b)[2];
		if (Size == 13) return *(uint64_t*)a == *(uint64_t*)b && ((uint32_t*)a)[2] == ((uint32_t*)b)[2] && ((char*)a)[12] == ((char*)b)[12];
		if (Size == 14) return *(uint64_t*)a == *(uint64_t*)b && ((uint32_t*)a)[2] == ((uint32_t*)b)[2] && ((uint16_t*)a)[6] == ((uint16_t*)b)[6];
		if (Size == 15) return *(uint64_t*)a == *(uint64_t*)b && ((uint32_t*)a)[2] == ((uint32_t*)b)[2] && tpl_equals<3>(&((char*)a)[12], &((char*)b)[12]);
		if (Size == 16) return *(uint64_t*)a == *(uint64_t*)b && ((uint64_t*)a)[1] == ((uint64_t*)b)[1];
		__pragma(warning(pop))
		return std::memcmp(a, b, Size) == 0;
	}

	template <size_t Size>
	bool tpl_equals_ci(const char* a, const char* b)
	{
		return _strnicmp(a, b, Size) == 0;
	}

	template <size_t Size>
	bool tpl_equals_ci(const wchar_t* a, const wchar_t* b)
	{
		return _wcsnicmp(a, b, Size) == 0;
	}

	inline bool is_whitespace(char c)
	{
		return c == ' ' || c == '\t' || c == '\r';
	}

	template <std::size_t N>
	bool char_matches(char c, const char (&cs)[N])
	{
		for (auto i = 0U; i < N - 1; ++i)
		{
			if (cs[i] == c) return true;
		}
		return false;
	}

	inline size_t size_min(size_t a, size_t b)
	{
		return a < b ? a : b;
	}

	struct str_view
	{
		str_view() noexcept : data_("") { }

		template <std::size_t N>
		explicit str_view(const char (&cs)[N]) noexcept
			: data_(cs), length_(uint32_t(N - 1)) { }

		static str_view from_cstr(const char* data, int offset = 0) noexcept
		{
			if (!data) return {};
			return {&data[offset], 0, strlen(&data[offset])};
		}

		static str_view from_str(const std::string& data) noexcept
		{
			return {data.c_str(), 0, data.size()};
		}

		// Safe: start can be outside of input string
		str_view(const std::string& data, const size_t start, const size_t length) noexcept
		{
			const auto data_size = data.size(); 
			if (start < data_size)
			{
				data_ = &data[start];
				length_ = size_min(data_size - start, length);
			}
			else
			{
				data_ = "";
			}
		}

		str_view(const str_view& data, const size_t start, const size_t length) noexcept
		{
			const auto data_size = data.size(); 
			if (start < data_size)
			{
				data_ = &data[start];
				length_ = size_min(data_size - start, length);
			}
			else
			{
				data_ = "";
			}
		}

		// Unsafe: start and length have to be inside of input string
		str_view(const char* data, const size_t start, const size_t length) noexcept
			: data_(&data[start]), length_(length) {}

		// Basic access to data
		bool empty() const noexcept { return length_ == 0U; }
		size_t size() const noexcept { return length_; }
		const char* data() const noexcept { return data_; }
		const char* begin() const noexcept { return data_; }
		const char* end() const noexcept { return &data_[length_]; }
		bool operator==(const str_view& o) const noexcept { return size() == o.size() && memcmp(data_, o.data_, length_) == 0; }
		bool operator!=(const str_view& o) const noexcept { return !(*this == o); }
		const char& operator[](const size_t index) const noexcept { return data_[index]; }

		int64_t as(int64_t d) const;
		uint64_t as(uint64_t d) const;
		int32_t as(int32_t d) const { return int32_t(as(int64_t(d))); } 
		uint32_t as(uint32_t d) const { return uint32_t(as(uint64_t(d))); } 
		float as(float d) const;

		// Conversion to regular string
		std::string str() const noexcept
		{
			if (length_ > 0ULL) return std::string(data_, length_);
			return std::string();
		}

		// Common string-like functions
		size_t find_first_of(char c, const size_t index = 0U) const noexcept
		{
			for (auto i = index; i < length_; ++i)
			{
				if (data_[i] == c) return i;
			}
			return std::string::npos;
		}

		template <std::size_t N>
		size_t find_first_of(const char (&cs)[N], const size_t index = 0U) const noexcept
		{
			for (auto i = index; i < length_; ++i)
			{
				if (char_matches(data_[i], cs)) return i;
			}
			return std::string::npos;
		}

		size_t find_first_of(const std::string& cs, const size_t index = 0U) const noexcept;

		size_t find_last_of(const char c, const size_t index = UINT64_MAX) const noexcept
		{
			for (auto i = size_min(index, length_); i > 0; --i)
			{
				if (data_[i - 1] == c) return i - 1;
			}
			return std::string::npos;
		}

		template <std::size_t N>
		size_t find_last_of(const char (&cs)[N], const size_t index = 0U) const noexcept
		{
			for (auto i = size_min(index, length_); i > 0; --i)
			{
				if (char_matches(data_[i - 1], cs)) return i;
			}
			return std::string::npos;
		}

		size_t find_cstrl(const char* c, size_t c_len, size_t index = 0U) const;
		size_t find(const char* c, size_t index = 0U) const;
		size_t find(const str_view& c, size_t index = 0U) const;
		size_t find(const std::string& c, size_t index = 0U) const;

		// Modifications on current string
		void trim() noexcept
		{
			for (; length_ > 0U && is_whitespace(data_[length_ - 1]); --length_) {}
			for (; length_ > 0U && is_whitespace(data_[0]); ++data_, --length_) {}
		}

		// Modifications on current string
		template <std::size_t N>
		void trim(const char (&cs)[N]) noexcept
		{
			for (; length_ > 0U && char_matches(data_[length_ - 1], cs); --length_) {}
			for (; length_ > 0U && char_matches(data_[0], cs); ++data_, --length_) {}
		}

		// Creation of new substrings of different kind
		str_view substr(const size_t offset) const noexcept
		{
			if (offset >= length_) return str_view{};
			return str_view{data_, offset, length_ - offset};
		}

		str_view substr(const size_t offset, const size_t length) const noexcept
		{
			if (offset >= length_) return str_view{};
			return str_view{data_, offset, size_min(length_ - offset, length)};
		}

		std::pair<str_view, str_view> kv_split(char separator) const
		{
			std::pair<str_view, str_view> ret;
			const auto f = find_first_of(separator);
			if (f == std::string::npos)
			{
				ret.second = *this;
			}
			else
			{
				ret.first = substr(0, f);
				ret.first.trim();
				ret.second = substr(f + 1);
			}
			ret.second.trim();
			return ret;
		}

		std::pair<str_view, str_view> pair(char separator) const
		{
			std::pair<str_view, str_view> ret;
			const auto f = find_first_of(separator);
			if (f == std::string::npos)
			{
				ret.first = *this;
			}
			else
			{
				ret.first = substr(0, f);
				ret.second = substr(f + 1);
			}
			return ret;
		}

		std::vector<std::pair<str_view, str_view>> pairs(char separator) const
		{
			std::vector<std::pair<str_view, str_view>> result;
			auto& trick = *(std::vector<str_view>*)&result;
			split_to(trick, separator, false, false, false, UINT64_MAX);
			if (trick.size() & 1) trick.pop_back();
			return result;
		}

		std::vector<str_view> split(char separator, bool skip_empty, bool trim_result, size_t limit = UINT64_MAX) const
		{
			std::vector<str_view> result;
			split_to(result, separator, skip_empty, trim_result, false, limit);
			return result;
		}

		std::vector<str_view> split(const std::string& separator, bool skip_empty, bool trim_result, size_t limit = UINT64_MAX) const
		{
			std::vector<str_view> result;
			split_to(result, separator, skip_empty, trim_result, false, limit);
			return result;
		}

		operator CefString() const
		{
			return CefString(data(), size());
		}

		operator cef_string_t() const
		{
			cef_string_t r{};
			cef_string_utf8_to_utf16(data(), size(), &r);
			return r;
		}

		void split_to(std::vector<str_view>& result, char separator, bool skip_empty, bool trim_result, bool clear_target, size_t limit) const;
		void split_to(std::vector<str_view>& result, const std::string& separator, bool skip_empty, bool trim_result, bool clear_target, size_t limit) const;
		
		bool operator==(const std::string& cs) const { return length_ == cs.size() && strncmp(data_, cs.data(), length_) == 0; }
		bool operator!=(const std::string& cs) const { return !(*this == cs); }
		bool operator==(const char* cs) const { return strncmp(data_, cs, length_) == 0 && cs[length_] == 0; }
		bool operator!=(const char* cs) const { return !(*this == cs); }

		// Faster templated functions:
		template <std::size_t N>
		bool equals(const char (&cs)[N]) const noexcept
		{
			return N == 1 ? empty() : length_ == N - 1 && utils::tpl_equals<(N - 1) * sizeof(char)>(data_, cs);
		}

		template <std::size_t N>
		bool equals_ci(const char (&cs)[N]) const noexcept
		{
			return N == 1 ? empty() : length_ == N - 1 && utils::tpl_equals_ci<(N - 1) * sizeof(char)>(data_, cs);
		}

		template <std::size_t N>
		bool starts_with(const char (&cs)[N]) const noexcept
		{
			return N == 1 || length_ >= N - 1 && utils::tpl_equals<(N - 1) * sizeof(char)>(data_, cs);
		}

		template <std::size_t N>
		bool starts_with_ci(const char (&cs)[N]) const noexcept
		{
			return N == 1 || length_ >= N - 1 && utils::tpl_equals_ci<(N - 1) * sizeof(char)>(data_, cs);
		}

		template <std::size_t N>
		bool ends_with(const char (&cs)[N]) const noexcept
		{
			return N == 1 || length_ >= N - 1 && utils::tpl_equals<(N - 1) * sizeof(char)>(&data_[length_ - (N - 1)], cs);
		}

		template <std::size_t N>
		bool ends_with_ci(const char (&cs)[N]) const noexcept
		{
			return N == 1 || length_ >= N - 1 && utils::tpl_equals_ci<(N - 1) * sizeof(char)>(&data_[length_ - (N - 1)], cs);
		}

		template <std::size_t N>
		bool operator==(const char (&cs)[N]) const { return equals(cs); }

		template <std::size_t N>
		bool operator!=(const char (&cs)[N]) const { return !equals(cs); }

	private:
		const char* data_{};
		size_t length_{};
	};

	std::string& operator +=(std::string& l, const str_view& r);
	std::string operator +(std::string l, const str_view& r);
	std::ostream& operator<<(std::ostream& os, const str_view& self);

	std::string utf8_r(const wchar_t* s, size_t len);
	inline std::string utf8(const wchar_t* s) { return utf8_r(s, wcslen(s)); }
	inline std::string utf8(const std::wstring& s) { return utf8_r(s.c_str(), s.size()); }

	std::wstring utf16_r(const char* s, size_t len);
	inline std::wstring utf16(const char* s) { return utf16_r(s, strlen(s)); }
	inline std::wstring utf16(const std::string& s) { return utf16_r(s.c_str(), s.size()); }
	inline std::wstring utf16(const str_view& s) { return utf16_r(s.data(), s.size()); }
}

inline bool get_env_value(const wchar_t* key, bool default_value)
{	
	wchar_t var_data[32]{};
	return GetEnvironmentVariableW(key, var_data, 32) ? var_data[0] == '1' : default_value;
}

inline uint32_t get_env_value(const wchar_t* key, uint32_t default_value)
{	
	wchar_t var_data[32]{};
	return GetEnvironmentVariableW(key, var_data, 32) ? uint32_t(wcstoul(var_data, nullptr, 10)) : default_value;
}

inline std::wstring get_env_value(const wchar_t* key, const wchar_t* default_value)
{	
	wchar_t var_data[256]{};
	return std::wstring(GetEnvironmentVariableW(key, var_data, 256) ? var_data : default_value);
}

struct lson_builder
{
	std::string dst{"{"};

	// template<typename T>

	struct field_setter
	{
		lson_builder* parent;
		const char* key;

		template<typename T>
		field_setter& operator =(const T& value)
		{
			parent->add(key, value);
			return *this;
		}
	};

	field_setter operator[](const char* key)
	{
		return {this, key};
	}

	bool empty() const
	{
		return dst.size() <= 1;
	}

	static bool fitting_key(const char* key, size_t len)
	{
		if (len == 0 || !isalpha(key[0])) return false;
		for (auto i = 1U; i < len; ++i)
		{
			if (!isalnum(key[i]) && key[i] != '_') return false;
		}
		return true;
	}

	void add_key(const char* key)
	{		
		if (key)
		{
			const auto l = strlen(key);
			if (fitting_key(key, l))
			{
				const auto p = dst.size();
				dst.resize(p + l + 1);
				memcpy(&dst[p], key, l);
				dst[p + l] = '=';
			}
			else
			{
				dst.push_back('[');
				add_string(key, l);
				dst.push_back(']');
				dst.push_back('=');
			}
		}
	}

	void add_string(const char* data, size_t size)
	{
		dst.reserve(size + 2);
		dst.push_back('"');
		for (auto i = 0U; i < size; ++i)
		{
			auto c = data[i];
			if (c == 0)
			{
				dst.push_back('\\');
				dst.push_back('0');
			}
			else if (c == '\n')
			{
				dst.push_back('\\');
				dst.push_back('n');
			}
			else
			{
				if (c == '"' || c == '\\')
				{
					dst.push_back('\\');
				}
				dst.push_back(c);
			}
		}
		dst.push_back('"');
	}

	lson_builder& add_raw(const char* key, const std::string& child)
	{
		if (dst.size() > 1) dst.push_back(',');
		add_key(key);
		dst += child;
		return *this;
	}

	lson_builder& add(const char* key, const lson_builder& child)
	{
		if (dst.size() > 1) dst.push_back(',');
		add_key(key);
		dst += child.dst;
		dst.push_back('}');
		return *this;
	}

	/*template<typename Callback>
	void child(const char* key, Callback&& callback)
	{
		lson_builder c;
		callback(c);
		add_raw(key, c.finalize());
	}*/

	template<typename T>
		requires(std::is_arithmetic_v<T> && !std::_Is_character_or_bool<T>::value)
	lson_builder& add(const char* key, T src)
	{
		add_raw(key, std::to_string(src));
		return *this;
	}

	template<typename T>
		requires(std::is_arithmetic_v<T> && !std::_Is_character_or_bool<T>::value)
	lson_builder& add_opt(const char* key, T src)
	{
		if (src != T{})
		{
			add_raw(key, std::to_string(src));
		}
		return *this;
	}

	template<typename T>
		requires(std::is_enum_v<T>)
	lson_builder& add(const char* key, T src)
	{
		add_raw(key, std::to_string(std::underlying_type_t<T>(src)));
		return *this;
	}
	
	lson_builder& add(const char* key, const cef_time_t& src)
	{
		time_t time;
		cef_time_to_timet(&src, &time);
		add(key, time);
		return *this;
	}
	
	lson_builder& add(const char* key, bool src)
	{
		add_raw(key, src ? "true" : "false");
		return *this;
	}

	lson_builder& add(const char* key, const char* src)
	{
		if (!src) return *this;
		add(key, std::string(src));
		return *this;
	}

	lson_builder& add_opt(const char* key, const std::string& src)
	{
		if (src.empty()) return *this;
		add(key, src);
		return *this;
	}

	lson_builder& add(const char* key, const std::string& src)
	{
		if (dst.size() > 1) dst.push_back(',');
		add_key(key);
		add_string(src.data(), src.size());
		return *this;
	}

	lson_builder& add(const char* key, const utils::str_view& src)
	{
		if (dst.size() > 1) dst.push_back(',');
		add_key(key);
		add_string(src.data(), src.size());
		return *this;
	}

	lson_builder& add(const char* key, const CefString& src)
	{
		return add(key, src.ToString());
	}

	lson_builder& add(const char* key, const cef_string_t& src)
	{
		if (!src.length || !src.str)
		{
			return add(key, std::string());
		}
		cef_string_utf8_t out{};
		cef_string_utf16_to_utf8(src.str, src.length, &out);
		add(key, std::string(out.str, out.length));
		out.dtor(out.str);
		return *this;
	}

	template<typename T>
	lson_builder& add(const char* key, const std::vector<T>& src)
	{
		lson_builder child;
		for (auto i : src)
		{
			child.add(nullptr, i);
		}
		add(key, child);
		return *this;
	}

	lson_builder& add_opt(const char* key, const CefString& src)
	{
		if (src.empty()) return *this;
		return add(key, src.ToString());
	}

	lson_builder& add_opt(const char* key, const cef_string_t& src)
	{
		if (src.length == 0 || !src.str) return *this;
		return add(key, src);
	}

	std::string finalize()
	{
		dst.push_back('}');
		return std::move(dst);
	}
};

struct accsp_mapped : noncopyable
{
	HANDLE entry_handle{};
	LPVOID entry{};
	size_t size{};

	accsp_mapped(const std::wstring& filename, size_t size, bool existing_only = true);
	~accsp_mapped();

	utils::str_view view() const { return utils::str_view((const char*)entry, 0, size); }
};

template<typename T>
struct accsp_mapped_typed : accsp_mapped
{
	T* entry;

	explicit accsp_mapped_typed(const std::wstring& filename, bool existing_only = true) : accsp_mapped(filename, sizeof(T), existing_only)
	{
		entry = (T*)accsp_mapped::entry;
	}

	operator T*() const { return entry; }
	T& operator *() const { return *entry; }
	T* operator ->() const { return entry; }
};

template <typename... Args>
std::string strformat(const char* format, Args&& ...args)
{
	std::string ret;
	if (const int size = std::snprintf(ret.data(), ret.capacity() + 1, format, args...); size > ret.capacity())
	{
		ret.resize(size);
		std::snprintf(ret.data(), ret.capacity() + 1, format, args...);
	}
	else
	{
		ret.resize(size);
	}
	return ret;
}

#define STACK_ENTITY\
	void AddRef() const override {}\
	bool HasAtLeastOneRef() const override { return true; }\
	bool HasOneRef() const override { return true; }\
	bool Release() const override { return false; }