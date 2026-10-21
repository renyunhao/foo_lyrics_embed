#pragma once

// 歌词整理功能的公共类型与文本工具，供判定模块与在线源模块共用。
// 内部统一使用 UTF-8 的 std::string，只在调用 foobar2000 API 时转换。

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lyrics {

struct track_meta {
	std::string path;
	uint32_t subsong = 0;
	std::string title;
	std::string artist;
	std::string album;
	double duration = 0.0;
};

inline std::string trim_copy(const std::string& s) {
	size_t b = 0, e = s.size();
	while (b < e && (unsigned char)s[b] <= ' ') b++;
	while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
	return s.substr(b, e - b);
}

inline bool starts_with(const std::string& s, const char* prefix) {
	size_t n = strlen(prefix);
	return s.size() >= n && memcmp(s.data(), prefix, n) == 0;
}

inline char lower_ascii(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

inline std::string lowercase_ascii(const std::string& s) {
	std::string out(s);
	for (char& c : out) c = lower_ascii(c);
	return out;
}

inline bool contains_ci(const std::string& hay, const char* needle) {
	std::string h = lowercase_ascii(hay);
	std::string n = lowercase_ascii(needle);
	return h.find(n) != std::string::npos;
}

inline std::string strip_bom(std::string s) {
	if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
		return s.substr(3);
	return s;
}

inline std::vector<std::string> split_lines(const std::string& s) {
	std::vector<std::string> out;
	std::string cur;
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (c == '\r') {
			if (i + 1 < s.size() && s[i + 1] == '\n') i++;
			out.push_back(cur);
			cur.clear();
		} else if (c == '\n') {
			out.push_back(cur);
			cur.clear();
		} else {
			cur.push_back(c);
		}
	}
	out.push_back(cur);
	return out;
}

inline std::string join_lines(const std::vector<std::string>& lines) {
	std::string out;
	for (size_t i = 0; i < lines.size(); i++) {
		if (i) out += '\n';
		out += lines[i];
	}
	return out;
}

inline void append_utf8(std::string& out, uint32_t cp) {
	if (cp < 0x80) {
		out.push_back((char)cp);
	} else if (cp < 0x800) {
		out.push_back((char)(0xC0 | (cp >> 6)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back((char)(0xE0 | (cp >> 12)));
		out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	} else {
		out.push_back((char)(0xF0 | (cp >> 18)));
		out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	}
}

inline std::vector<uint32_t> utf8_codepoints(const std::string& s) {
	std::vector<uint32_t> out;
	size_t i = 0;
	while (i < s.size()) {
		unsigned char c = (unsigned char)s[i];
		uint32_t cp = 0xFFFD;
		size_t n = 1;
		if (c < 0x80) {
			cp = c;
		} else if ((c >> 5) == 0x6) {
			cp = c & 0x1F; n = 2;
		} else if ((c >> 4) == 0xE) {
			cp = c & 0x0F; n = 3;
		} else if ((c >> 3) == 0x1E) {
			cp = c & 0x07; n = 4;
		}
		if (i + n > s.size()) {
			cp = 0xFFFD; n = 1;
		} else if (n > 1) {
			for (size_t k = 1; k < n; k++)
				cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
		}
		out.push_back(cp);
		i += n;
	}
	return out;
}

inline bool is_ignored_codepoint(uint32_t cp) {
	if (cp == 0x00B7 || cp == 0x30FB) return true; // 间隔号
	if (cp >= 0x2000 && cp <= 0x206F) return true; // 通用标点
	if (cp >= 0x3000 && cp <= 0x303F) return true; // CJK 标点
	if (cp >= 0xFE30 && cp <= 0xFE4F) return true; // CJK 兼容形式
	if (cp >= 0xFF01 && cp <= 0xFF0F) return true;
	if (cp >= 0xFF1A && cp <= 0xFF20) return true;
	if (cp >= 0xFF3B && cp <= 0xFF40) return true;
	if (cp >= 0xFF5B && cp <= 0xFF65) return true;
	return false;
}

// 用于标题/艺人比较的归一化：全角转半角、小写、去除括号内容与标点、压缩空格。
inline std::string normalize_for_match(const std::string& s) {
	auto cps = utf8_codepoints(s);
	std::string out;
	int bracket = 0;
	for (uint32_t cp : cps) {
		if (cp == '(' || cp == '[' || cp == 0xFF08 || cp == 0x3010 || cp == 0x300A ||
			cp == 0x3008 || cp == 0x3014 || cp == 0xFF3B || cp == 0xFE5D) {
			bracket++;
			continue;
		}
		if (cp == ')' || cp == ']' || cp == 0xFF09 || cp == 0x3011 || cp == 0x300B ||
			cp == 0x3009 || cp == 0x3015 || cp == 0xFF3D || cp == 0xFE5E) {
			if (bracket > 0) bracket--;
			continue;
		}
		if (bracket > 0) continue;

		if (cp >= 0xFF01 && cp <= 0xFF5E) cp = cp - 0xFF01 + 0x21;
		if (cp == 0x3000) cp = ' ';
		if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';

		if (cp < 0x80) {
			bool alnum = (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9');
			if (alnum) {
				out.push_back((char)cp);
			} else if (cp == ' ') {
				if (!out.empty() && out.back() != ' ') out.push_back(' ');
			}
			continue;
		}
		if (is_ignored_codepoint(cp)) continue;
		append_utf8(out, cp);
	}
	return trim_copy(out);
}

inline double text_similarity(const std::string& a, const std::string& b) {
	if (a.empty() && b.empty()) return 1.0;
	if (a.empty() || b.empty()) return 0.0;
	auto x = utf8_codepoints(a);
	auto y = utf8_codepoints(b);
	size_t n = x.size(), m = y.size();
	std::vector<size_t> prev(m + 1), cur(m + 1);
	for (size_t j = 0; j <= m; j++) prev[j] = j;
	for (size_t i = 1; i <= n; i++) {
		cur[0] = i;
		for (size_t j = 1; j <= m; j++) {
			size_t cost = (x[i - 1] == y[j - 1]) ? 0 : 1;
			cur[j] = (std::min)((std::min)(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
		}
		prev.swap(cur);
	}
	double ratio = 1.0 - (double)prev[m] / (double)(std::max)(n, m);
	return ratio < 0.0 ? 0.0 : ratio;
}

inline std::vector<std::string> split_artists(const std::string& s) {
	std::vector<std::string> out;
	std::string cur;
	const std::string separators = "/,;&、，；";
	for (size_t i = 0; i < s.size();) {
		if (separators.find(s[i]) != std::string::npos) {
			out.push_back(cur);
			cur.clear();
			i++;
			continue;
		}
		cur.push_back(s[i]);
		i++;
	}
	out.push_back(cur);

	std::vector<std::string> result;
	for (auto& item : out) {
		std::string t = trim_copy(item);
		// 处理 feat. / ft. 这类合作写法
		std::string low = lowercase_ascii(t);
		size_t pos = low.find(" feat. ");
		if (pos == std::string::npos) pos = low.find(" ft. ");
		if (pos != std::string::npos) {
			result.push_back(trim_copy(t.substr(0, pos)));
			result.push_back(trim_copy(t.substr(pos + 6)));
			continue;
		}
		if (!t.empty()) result.push_back(t);
	}
	return result;
}

inline bool artist_matches(const std::string& fileArtist, const std::string& sourceArtist) {
	std::string a = normalize_for_match(fileArtist);
	std::string b = normalize_for_match(sourceArtist);
	if (a.empty() || b.empty()) return false;
	if (a == b) return true;
	if (a.find(b) != std::string::npos || b.find(a) != std::string::npos) return true;

	auto ta = split_artists(fileArtist);
	auto tb = split_artists(sourceArtist);
	for (auto& x : ta) {
		std::string nx = normalize_for_match(x);
		if (nx.empty()) continue;
		for (auto& y : tb) {
			std::string ny = normalize_for_match(y);
			if (ny.empty()) continue;
			if (nx == ny) return true;
			if (ny.find(nx) != std::string::npos || nx.find(ny) != std::string::npos) return true;
		}
	}
	return false;
}

inline std::string url_encode(const std::string& s) {
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back((char)c);
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0F]);
		}
	}
	return out;
}

inline bool all_digits(const std::string& s) {
	if (s.empty()) return false;
	for (char c : s)
		if (c < '0' || c > '9') return false;
	return true;
}

} // namespace lyrics
