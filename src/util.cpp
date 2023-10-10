#include <cstdarg>
#include <memory>

#include "platform.h"
#include "util.h"

void __log_message(const char* msg, ...)
{
	if (msg)
	{
		char buff[4096];
		va_list args;
		va_start(args, msg);
		if (vsprintf_s(buff, 4096, msg, args) > 0)
		{
			OutputDebugStringA(buff);
		}
	}
}

namespace utils
{
	inline double pow10(int n)
	{
		auto ret = 1.0;
		auto r = 10.0;
		if (n < 0)
		{
			n = -n;
			r = 0.1;
		}

		while (n)
		{
			if (n & 1)
			{
				ret *= r;
			}
			r *= r;
			n >>= 1;
		}
		return ret;
	}

	template<typename T>
	bool is_digit(T c, int& v)
	{
		v = c - '0';
		return v >= 0 && v <= 9;
	}

	template<typename T>
	double parse_gen(const T*& c, const T* ce, double d)
	{
		if (!c || c == ce)
		{
			return d;
		}

		auto sign = 1;
		auto int_part = 0.0;
		auto frac_part = 0.0;
		auto has_frac = false;
		auto has_exp = false;

		// +/- sign
		if (*c == '-')
		{
			++c;
			sign = -1;
		}
		else if (*c == '+')
		{
			++c;
		}

		auto s = c;
		int v;
		while (true)
		{
			auto h = c == ce ? '\0' : *c;
			if (is_digit(h, v))
			{
				int_part = int_part * 10 + v;
			}
			else if (h == '.')
			{
				has_frac = true;
				++c;
				break;
			}
			else if (h == 'e' || h == 'E')
			{
				has_exp = true;
				++c;
				break;
			}
			else
			{
				return s == c ? d : sign * int_part;
			}
			++c;
		}

		if (has_frac)
		{
			auto frac_exp = 0.1;
			while (true)
			{
				auto h = c == ce ? '\0' : *c;
				if (is_digit(h, v))
				{
					frac_part += frac_exp * v;
					frac_exp *= 0.1;
				}
				else if (h == 'e' || h == 'E')
				{
					has_exp = true;
					++c;
					break;
				}
				else
				{
					return sign * (int_part + frac_part);
				}
				++c;
			}
		}

		// parsing exponent part
		auto exp_part = 1.0;
		if (has_exp)
		{
			auto exp_sign = 1;
			auto h = c == ce ? '\0' : *c;
			if (h == '-')
			{
				exp_sign = -1;
				h = *++c;
			}
			else if (h == '+')
			{
				h = *++c;
			}

			auto e = 0;
			while (is_digit(h, v))
			{
				e = e * 10 + v;
				++c;
				h = c == ce ? '\0' : *c;
			}

			exp_part = pow10(exp_sign * e);
		}

		return sign * (int_part + frac_part) * exp_part;
	}

	#define HN 16
	static constexpr uint64_t hextable[] = {
		HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN,
		HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, HN, HN, HN, HN, HN, HN, HN, 10, 11, 12, 13, 14, 15, HN,
		HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, 10, 11, 12, 13, 14, 15, HN, HN, HN, HN,
		HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN,
		HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN,
		HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN,
		HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN, HN,
		HN, HN, HN, HN, HN, HN, HN, HN, HN
	};

	template<typename T>
	uint32_t hexdec_address(T v);

	template<>
	uint32_t hexdec_address(char v)
	{
		return *reinterpret_cast<const uint8_t*>(&v);
	}

	template<>
	uint32_t hexdec_address(wchar_t v)
	{
		return std::min(255U, uint32_t(*reinterpret_cast<const uint16_t*>(&v)));
	}

	template<typename T>
	uint64_t parse_gen_hex(const T*& c, const T* ce)
	{
		auto ret = 0ULL;
		while (true)
		{
			const auto i = hextable[hexdec_address(T(c == ce ? '\0' : *c))];
			if (i == HN) return ret;
			ret = (ret << 4) | i;
			++c;
		}
	}

	template<typename T>
	uint64_t parse_gen(const T*& c, const T* ce, uint64_t d)
	{
		if (!c) return d;

		if ((!ce || c + 2 < ce) && c[0] == '0' && c[1] == 'x')
		{
			c += 2;
			return parse_gen_hex(c, ce);
		}
		
		auto ret = 0ULL;
		const auto start = c;
		for (int v; c != ce && is_digit(*c, v); ++c)
		{
			ret = ret * 10ULL + uint64_t(v);
		}
		return c == start ? d : ret;
	}

	template<typename T>
	int64_t parse_gen(const T*& c, const T* ce, int64_t d)
	{
		if (!c) return d;

		if ((!ce || c + 2 < ce) && c[0] == '0' && c[1] == 'x')
		{
			c += 2;
			return parse_gen_hex(c, ce);
		}

		if (c == ce)
		{
			return d;
		}

		auto sign = 1;
		if (*c == '-')
		{
			sign = -1;
			++c;
		}

		auto ret = 0ULL;
		const auto start = c;
		for (int v; c != ce && is_digit(*c, v); ++c)
		{
			ret = ret * 10ULL + uint64_t(v);
		}
		return c == start ? d : int64_t(ret) * sign;
	}

	bool char_matches_str(char c, const std::string& cs)
	{
		for (const auto i : cs)
		{
			if (i == c) return true;
		}
		return false;
	}

	int64_t str_view::as(int64_t d) const
	{
		auto s = data();
		return parse_gen(s, s + size(), d);
	}

	uint64_t str_view::as(uint64_t d) const
	{
		auto s = data();
		return parse_gen(s, s + size(), d);
	}

	float str_view::as(float d) const
	{
		auto s = data();
		return float(parse_gen(s, s + size(), d));
	}

	size_t str_view::find_first_of(const std::string& cs, const size_t index) const noexcept
	{
		for (auto i = index; i < length_; ++i)
		{
			if (char_matches_str(data_[i], cs)) return i;
		}
		return std::string::npos;
	}

	size_t str_view::find_cstrl(const char* c, size_t c_len, size_t index) const
	{
		if (index >= length_) return std::string::npos;
		if (index + c_len > length_) return std::string::npos;
		if (index + c_len == length_) return memcmp(&data_[index], c, c_len) == 0;
		const auto s = std::search(&data_[index], end(), c, c + c_len);
		return s == end() ? std::string::npos : s - begin();
	}

	size_t str_view::find(const char* c, size_t index) const
	{
		if (index >= length_) return std::string::npos;
		return find_cstrl(c, strlen(c), index);
	}

	size_t str_view::find(const str_view& c, size_t index) const
	{
		if (index >= length_) return std::string::npos;
		return find_cstrl(c.data(), c.size(), index);
	}

	size_t str_view::find(const std::string& c, size_t index) const
	{
		if (index >= length_) return std::string::npos;
		return find_cstrl(c.data(), c.size(), index);
	}

	void str_view::split_to(std::vector<str_view>& result, char separator, bool skip_empty, bool trim_result, bool clear_target, size_t limit) const
	{
		if (clear_target) result.clear();
		auto index = 0U;
		const auto size = this->size();
		while (index <= size)
		{
			auto next = result.size() + 1 == limit ? std::string::npos : find_first_of(separator, index);
			if (next == std::string::npos) next = uint32_t(size);
			auto piece = substr(index, uint32_t(next) - index);
			if (trim_result) piece.trim();
			if (!skip_empty || !piece.empty()) result.push_back(piece);
			index = uint32_t(next) + 1;
		}
	}

	void str_view::split_to(std::vector<str_view>& result, const std::string& separator, bool skip_empty, bool trim_result, bool clear_target, size_t limit) const
	{
		if (clear_target) result.clear();
		auto index = 0U;
		const auto size = this->size();
		while (index <= size)
		{
			auto next = result.size() + 1 == limit ? std::string::npos : find_first_of(separator, index);
			if (next == std::string::npos) next = uint32_t(size);
			auto piece = substr(index, uint32_t(next) - index);
			if (trim_result) piece.trim();
			if (!skip_empty || !piece.empty()) result.push_back(piece);
			index = uint32_t(next) + 1;
		}
	}

	std::string& operator+=(std::string& l, const str_view& r)
	{
		if (!r.empty())
		{
			const auto l_size = l.size();
			l.resize(l_size + r.size());
			memcpy(&l[l_size], r.data(), r.size());
		}
		return l;
	}

	std::string operator+(std::string l, const str_view& r)
	{
		return l += r;
	}

	std::ostream& operator<<(std::ostream& os, const str_view& self)
	{
		os << self.str();
		return os;
	}

	std::string utf8_r(const wchar_t* s, size_t len)
	{
		std::string result;
		if (s && len > 0)
		{
			result.resize(len);
			auto r = WideCharToMultiByte(CP_UTF8, 0, s, int(len), result.data(), int(result.size()), nullptr, nullptr);
			if (r == 0)
			{
				result.resize(WideCharToMultiByte(CP_UTF8, 0, s, int(len), nullptr, 0, nullptr, nullptr));
				r = WideCharToMultiByte(CP_UTF8, 0, s, int(len), result.data(), int(result.size()), nullptr, nullptr);
			}
			result.resize(r);
		}
		return result;
	}

	std::wstring utf16_r(const char* s, size_t len)
	{
		std::wstring result;
		if (s && len > 0)
		{
			result.resize(len);
			auto r = MultiByteToWideChar(CP_UTF8, 0, s, int(len), result.data(), int(result.size()));
			if (r == 0)
			{
				result.resize(MultiByteToWideChar(CP_UTF8, 0, s, int(len), nullptr, 0));
				r = MultiByteToWideChar(CP_UTF8, 0, s, int(len), result.data(), int(result.size()));
			}
			result.resize(r);
		}
		return result;
	}
}

accsp_mapped::accsp_mapped(const std::wstring& filename, size_t size, bool existing_only) : size(size)
{
	if (existing_only)
	{
		entry_handle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, filename.c_str());
	}
	else
	{
		entry_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			0, DWORD(size), filename.c_str());
	}
	if (!entry_handle || entry_handle == INVALID_HANDLE_VALUE)
	{
		auto err = GetLastError();
		log_message("Failed to open: %s (size: %llu, error: %x)", utils::utf8(filename).c_str(), size, err);
		throw std::runtime_error("Failed to open a file mapping: " + std::to_string(err));
	}

	entry = MapViewOfFile(entry_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
	if (!entry)
	{
		throw std::exception("Failed to map a file mapping");
	}
}

accsp_mapped::~accsp_mapped()
{
	if (entry) UnmapViewOfFile(entry);
	if (entry_handle && entry_handle != INVALID_HANDLE_VALUE) CloseHandle(entry_handle);
}