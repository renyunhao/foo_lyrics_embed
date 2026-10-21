#include "stdafx.h"

#include "lyrics_quality.h"

#include <cstdio>
#include <cmath>
#include <map>

namespace lyrics {

namespace {

struct parsed_line {
	bool timed = false;
	int64_t time_ms = 0;
	std::string text;
	bool credit = false;
};

struct parsed_lyric {
	std::vector<parsed_line> lines;
};

bool parse_time_tag(const std::string& inside, int64_t& out_ms) {
	size_t colon1 = inside.find(':');
	if (colon1 == std::string::npos || colon1 == 0) return false;

	size_t colon2 = inside.find(':', colon1 + 1);
	size_t dot = inside.find('.', colon1 + 1);

	std::string mm = inside.substr(0, colon1);
	std::string ss, frac;

	if (colon2 != std::string::npos && (dot == std::string::npos || colon2 < dot)) {
		ss = inside.substr(colon1 + 1, colon2 - colon1 - 1);
		frac = inside.substr(colon2 + 1);
	} else if (dot != std::string::npos) {
		ss = inside.substr(colon1 + 1, dot - colon1 - 1);
		frac = inside.substr(dot + 1);
	} else {
		ss = inside.substr(colon1 + 1);
	}

	if (!all_digits(mm) || !all_digits(ss)) return false;
	if (mm.size() > 3 || ss.size() > 2) return false;

	int minutes = atoi(mm.c_str());
	int seconds = atoi(ss.c_str());
	if (seconds >= 60) return false;

	int64_t ms = (int64_t)minutes * 60000 + (int64_t)seconds * 1000;
	if (!frac.empty()) {
		if (!all_digits(frac) || frac.size() > 3) return false;
		int f = atoi(frac.c_str());
		if (frac.size() == 1) f *= 100;
		else if (frac.size() == 2) f *= 10;
		ms += f;
	}
	out_ms = ms;
	return true;
}

// [ti:标题] / [ar:艺人] / [al:专辑] / [by:xxx] / [offset:0] 等元数据标签
bool is_meta_tag(const std::string& inside) {
	size_t colon = inside.find(':');
	if (colon == std::string::npos || colon == 0 || colon > 10) return false;
	for (size_t i = 0; i < colon; i++) {
		char c = inside[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
			return false;
	}
	return true;
}

parsed_lyric parse_lrc(const std::string& content) {
	parsed_lyric out;
	auto raw_lines = split_lines(strip_bom(content));

	for (auto& raw : raw_lines) {
		std::string s = trim_copy(raw);
		if (s.empty()) {
			out.lines.push_back(parsed_line());
			continue;
		}

		size_t pos = 0;
		std::vector<int64_t> times;
		bool only_meta = false;
		bool unknown_tag = false;

		while (pos < s.size() && s[pos] == '[') {
			size_t close = s.find(']', pos);
			if (close == std::string::npos) break;
			std::string inside = s.substr(pos + 1, close - pos - 1);
			int64_t ms = 0;
			if (parse_time_tag(inside, ms)) {
				times.push_back(ms);
			} else if (is_meta_tag(inside)) {
				only_meta = true;
			} else {
				unknown_tag = true;
				break;
			}
			pos = close + 1;
		}

		std::string rest = trim_copy(s.substr(pos));

		if (times.empty()) {
			if (only_meta && rest.empty()) {
				out.lines.push_back(parsed_line());
			} else {
				parsed_line ln;
				ln.timed = false;
				ln.text = unknown_tag ? s : rest.empty() ? s : rest;
				out.lines.push_back(ln);
			}
			continue;
		}

		for (int64_t t : times) {
			parsed_line ln;
			ln.timed = true;
			ln.time_ms = t;
			ln.text = rest;
			out.lines.push_back(ln);
		}
	}
	return out;
}

std::string normalize_key(const std::string& key) {
	std::string out;
	for (char c : key) {
		if ((unsigned char)c <= ' ') continue;
		if (c == '.' || c == '_') continue;
		out.push_back(lower_ascii(c));
	}
	return out;
}

bool key_is_zuoci(const std::string& k) {
	static const char* keys[] = {
		"作词", "词", "词作者", "词作", "歌词", "中文词", "英文词",
		"lyricsby", "lyricby", "lyricist", "lyricists", "lyrics", "wordsby", "writtenby", "lyric", "textby"
	};
	for (auto key : keys)
		if (k == key) return true;
	return false;
}

bool key_is_zuoqu(const std::string& k) {
	static const char* keys[] = {
		"作曲", "曲", "作曲者", "曲作", "曲作者",
		"composedby", "composer", "composers", "musicby", "music", "composed", "songby"
	};
	for (auto key : keys)
		if (k == key) return true;
	return false;
}

bool key_is_other_credit(const std::string& k) {
	static const char* keys[] = {
		"编曲", "制作人", "监制", "出品", "出品人", "发行", "录音", "录音师", "混音", "母带",
		"吉他", "贝斯", "鼓", "和声", "和声编写", "弦乐", "钢琴", "键盘", "programming",
		"arranger", "arrangedby", "producer", "producedby", "mixing", "mix", "mixedby",
		"mastering", "masteredby", "guitar", "bass", "drums", "backingvocals", "engineer",
		"recordedby", "op", "sp", "publisher", "digitalrelease", "label", "executiveproducer"
	};
	for (auto key : keys)
		if (k == key) return true;
	return false;
}

bool is_credit_text(const std::string& text, std::string& key, std::string& value) {
	// 注意：不能用 find_first_of(":：")，它按字节逐个匹配，会把全角冒号截断。
	size_t half = text.find(':');
	size_t full = text.find("\xEF\xBC\x9A"); // 全角冒号
	size_t pos = 0;
	size_t delim_len = 1;
	if (half == std::string::npos && full == std::string::npos) return false;
	if (half == std::string::npos || (full != std::string::npos && full < half)) {
		pos = full;
		delim_len = 3;
	} else {
		pos = half;
		delim_len = 1;
	}
	if (pos == 0) return false;

	std::string raw_key = trim_copy(text.substr(0, pos));
	if (raw_key.empty() || raw_key.size() > 24) return false;

	key = normalize_key(raw_key);
	if (key.empty()) return false;

	bool recognized = key_is_zuoci(key) || key_is_zuoqu(key) || key_is_other_credit(key);
	if (!recognized) {
		// “词曲：xxx”“作词/作曲：xxx”这类合并写法
		if ((key.find("作词") != std::string::npos || key.find("词曲") != std::string::npos) &&
			(key.find("作曲") != std::string::npos || key.find("词曲") != std::string::npos)) {
			recognized = true;
		}
	}
	if (!recognized) return false;

	value = trim_copy(text.substr(pos + delim_len));
	return !value.empty();
}

void collect_credits(const parsed_lyric& p, std::string& zuoci, std::string& zuoqu) {
	for (size_t i = 0; i < p.lines.size() && i < 40; i++) {
		const auto& ln = p.lines[i];
		if (ln.text.empty()) continue;

		std::string key, value;
		if (!is_credit_text(ln.text, key, value)) continue;

		bool both = (key.find("词曲") != std::string::npos) ||
			(key.find("作词") != std::string::npos && key.find("作曲") != std::string::npos) ||
			(key.find("词") != std::string::npos && key.find("曲") != std::string::npos && key.find('/') != std::string::npos);

		if (both) {
			if (zuoci.empty()) zuoci = value;
			if (zuoqu.empty()) zuoqu = value;
			continue;
		}
		if (key_is_zuoci(key) && zuoci.empty()) zuoci = value;
		if (key_is_zuoqu(key) && zuoqu.empty()) zuoqu = value;
	}
}

bool replace_all_ci(std::string& s, const std::string& needle, const std::string& repl) {
	if (needle.empty()) return false;
	std::string low = lowercase_ascii(s);
	std::string nlow = lowercase_ascii(needle);
	bool changed = false;
	size_t pos = 0;
	while ((pos = low.find(nlow, pos)) != std::string::npos) {
		s.replace(pos, needle.size(), repl);
		low.replace(pos, needle.size(), repl);
		pos += repl.size();
		changed = true;
	}
	return changed;
}

void decode_entities(std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();) {
		if (s[i] != '&') {
			out.push_back(s[i++]);
			continue;
		}
		size_t semi = s.find(';', i + 1);
		if (semi == std::string::npos || semi - i > 12) {
			out.push_back(s[i++]);
			continue;
		}
		std::string ent = s.substr(i + 1, semi - i - 1);
		std::string low = lowercase_ascii(ent);
		bool handled = true;
		if (low == "amp") out += '&';
		else if (low == "lt") out += '<';
		else if (low == "gt") out += '>';
		else if (low == "quot") out += '"';
		else if (low == "apos") out += '\'';
		else if (low == "nbsp") out += ' ';
		else if (low.size() > 1 && low[0] == '#') {
			uint32_t cp = 0;
			bool ok = true;
			if (low[1] == 'x') {
				if (ent.size() <= 2) ok = false;
				for (size_t k = 2; ok && k < ent.size(); k++) {
					char c = lower_ascii(ent[k]);
					if (c >= '0' && c <= '9') cp = cp * 16 + (c - '0');
					else if (c >= 'a' && c <= 'f') cp = cp * 16 + (c - 'a' + 10);
					else ok = false;
				}
			} else {
				for (size_t k = 1; ok && k < ent.size(); k++) {
					char c = ent[k];
					if (c >= '0' && c <= '9') cp = cp * 10 + (c - '0');
					else ok = false;
				}
			}
			if (ok && cp > 0 && cp <= 0x10FFFF) append_utf8(out, cp);
			else handled = false;
		} else {
			handled = false;
		}

		if (handled) {
			i = semi + 1;
		} else {
			out.push_back(s[i++]);
		}
	}
	s.swap(out);
}

bool has_ad_line(const std::string& text) {
	static const char* ads[] = {
		"www.", "http://", "https://", "更多歌词", "歌词下载", "下载歌词", "本歌词由",
		"歌词制作", "欢迎访问", "爱歌词", "好听音乐网", "提供歌词", "歌词网", "lrc歌词",
		"欢迎光临", "点击下载", "扫码", "公众号", "qq群"
	};
	std::string low = lowercase_ascii(text);
	for (auto ad : ads)
		if (low.find(ad) != std::string::npos) return true;
	return false;
}

bool detect_dirty(const std::string& content, std::string& why) {
	static const char* marks[] = { "<br", "&amp;", "&lt;", "&gt;", "&quot;", "&nbsp;", "&#" };
	for (auto m : marks) {
		if (contains_ci(content, m)) {
			why = std::string("包含网页残留：") + m;
			return true;
		}
	}
	if (content.find("\xEF\xBF\xBD") != std::string::npos) {
		why = "包含乱码替换符";
		return true;
	}
	for (unsigned char c : content) {
		if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
			why = "包含控制字符";
			return true;
		}
	}

	auto lines = split_lines(content);
	for (auto& l : lines) {
		std::string t = trim_copy(strip_bom(l));
		if (t.empty()) continue;
		if (has_ad_line(t)) {
			why = "包含广告行";
			return true;
		}
	}
	return false;
}

int64_t max_time_of(const parsed_lyric& p) {
	int64_t max_t = 0;
	for (auto& ln : p.lines)
		if (ln.timed && !ln.text.empty() && ln.time_ms > max_t) max_t = ln.time_ms;
	return max_t;
}

bool is_instrumental_texts(const std::vector<std::string>& texts) {
	if (texts.size() > 2) return false;
	std::string joined;
	for (auto& t : texts) joined += lowercase_ascii(t);
	static const char* marks[] = { "纯音乐", "请欣赏", "instrumental", "无人声", "演奏版" };
	for (auto m : marks)
		if (joined.find(m) != std::string::npos) return true;
	return false;
}

bool parse_srt_time(const std::string& s, int64_t& out_ms) {
	// HH:MM:SS,mmm 或 HH:MM:SS.mmm
	if (s.size() < 8) return false;
	int hh = 0, mm = 0, ss = 0, ms = 0;
	int n = sscanf(s.c_str(), "%d:%d:%d,%d", &hh, &mm, &ss, &ms);
	if (n < 3) n = sscanf(s.c_str(), "%d:%d:%d.%d", &hh, &mm, &ss, &ms);
	if (n < 3) return false;
	out_ms = ((int64_t)hh * 3600 + (int64_t)mm * 60 + ss) * 1000 + ms;
	return true;
}

std::string format_lrc_time(int64_t ms) {
	if (ms < 0) ms = 0;
	char buf[32];
	snprintf(buf, sizeof(buf), "[%02d:%02d.%03d]",
		(int)(ms / 60000), (int)((ms / 1000) % 60), (int)(ms % 1000));
	return buf;
}

std::string srt_to_lrc(const std::string& in) {
	auto lines = split_lines(in);
	std::string out;
	size_t i = 0;
	while (i < lines.size()) {
		std::string t = trim_copy(lines[i]);
		if (t.empty()) { i++; continue; }
		if (all_digits(t)) { i++; continue; }
		size_t arrow = t.find("-->");
		if (arrow == std::string::npos) { i++; continue; }

		int64_t start = 0;
		if (!parse_srt_time(trim_copy(t.substr(0, arrow)), start)) { i++; continue; }
		i++;

		while (i < lines.size()) {
			std::string body = trim_copy(lines[i]);
			if (body.empty()) { i++; break; }
			if (body.find("-->") != std::string::npos) break;
			out += format_lrc_time(start);
			out += body;
			out += '\n';
			i++;
		}
	}
	return out;
}

} // namespace

quality_result evaluate(const std::string& raw, double duration_sec) {
	quality_result r;
	std::string content = trim_copy(strip_bom(raw));
	if (content.empty()) {
		r.v = verdict::needs_replace;
		r.reasons.push_back("无内嵌歌词");
		return r;
	}

	parsed_lyric p = parse_lrc(content);
	collect_credits(p, r.zuoci, r.zuoqu);
	r.has_zuoci = !r.zuoci.empty();
	r.has_zuoqu = !r.zuoqu.empty();

	// 标记 credit 行
	for (size_t i = 0; i < p.lines.size() && i < 40; i++) {
		std::string key, value;
		if (!p.lines[i].text.empty() && is_credit_text(p.lines[i].text, key, value))
			p.lines[i].credit = true;
	}

	int timed_nonempty = 0;
	int lyric_lines = 0;
	int total_nonempty = 0;
	size_t backwards = 0;
	int64_t prev_time = -1;
	std::vector<std::string> plain_texts;

	for (auto& ln : p.lines) {
		if (ln.text.empty()) continue;
		total_nonempty++;
		if (ln.timed) {
			timed_nonempty++;
			if (!ln.credit) {
				lyric_lines++;
				plain_texts.push_back(ln.text);
			}
			if (prev_time >= 0 && ln.time_ms < prev_time) backwards++;
			prev_time = ln.time_ms;
		} else if (!ln.credit) {
			plain_texts.push_back(ln.text);
		}
	}

	r.timed_lines = timed_nonempty;
	r.lyric_lines = lyric_lines;
	r.total_lines = total_nonempty;
	r.max_time_ms = max_time_of(p);
	r.has_timeline = timed_nonempty > 0;

	if (is_instrumental_texts(plain_texts)) {
		r.instrumental = true;
		r.v = verdict::ok;
		r.score = 100;
		r.reasons.push_back("纯音乐/无人声");
		return r;
	}

	double coverage = total_nonempty > 0 ? (double)timed_nonempty / (double)total_nonempty : 0.0;
	bool h1 = timed_nonempty > 0;
	bool h2 = coverage >= 0.80;
	bool h3 = lyric_lines >= 8;
	bool h4 = backwards <= 2;
	bool h5 = true;
	if (duration_sec > 1.0) {
		int64_t dur_ms = (int64_t)(duration_sec * 1000.0);
		if (r.max_time_ms > dur_ms + 10000) h5 = false;
		else if (dur_ms > 20000 && r.max_time_ms < (int64_t)(dur_ms * 0.5)) h5 = false;
	}
	std::string dirty_why;
	bool h6 = !detect_dirty(content, dirty_why);
	bool h7 = r.has_zuoci && r.has_zuoqu;

	r.dirty = !h6;

	int score = 100;
	if (!h1) score -= 40;
	if (!h2) score -= 25;
	if (!h3) score -= 20;
	if (!h4) score -= 15;
	if (!h5) score -= 15;
	if (!h6) score -= 20;
	if (!h7) score -= 10;
	if (score < 0) score = 0;
	r.score = score;

	char buf[160];
	if (!h1) r.reasons.push_back("H1 缺少可解析的时间轴");
	if (!h2) {
		snprintf(buf, sizeof(buf), "H2 时间轴覆盖率不足（%.0f%%）", coverage * 100.0);
		r.reasons.push_back(buf);
	}
	if (!h3) {
		snprintf(buf, sizeof(buf), "H3 有效歌词行不足（%d 行）", lyric_lines);
		r.reasons.push_back(buf);
	}
	if (!h4) {
		snprintf(buf, sizeof(buf), "H4 时间轴倒序 %u 处", (unsigned)backwards);
		r.reasons.push_back(buf);
	}
	if (!h5) r.reasons.push_back("H5 时间轴与歌曲时长不符");
	if (!h6) r.reasons.push_back("H6 " + dirty_why);
	if (!h7) {
		std::string missing;
		if (!r.has_zuoci) missing += "作词";
		if (!r.has_zuoqu) {
			if (!missing.empty()) missing += "/";
			missing += "作曲";
		}
		r.reasons.push_back("H7 缺少" + missing);
	}

	if (looks_like_srt(content)) {
		r.v = verdict::needs_repair;
		r.reasons.push_back("SRT 格式，可本地转换");
	} else if (!h1 || !h2 || !h3 || !h4 || !h5) {
		r.v = verdict::needs_replace;
	} else if (!h6) {
		r.v = verdict::needs_repair;
	} else if (!h7) {
		r.v = verdict::missing_header;
	} else {
		r.v = verdict::ok;
	}

	return r;
}

bool looks_like_srt(const std::string& content) {
	auto lines = split_lines(content);
	for (auto& l : lines) {
		size_t pos = l.find("-->");
		if (pos == std::string::npos) continue;
		std::string left = trim_copy(l.substr(0, pos));
		int colons = 0;
		for (char c : left)
			if (c == ':') colons++;
		if (colons >= 2) return true;
	}
	return false;
}

bool repair_local(const std::string& in, std::string& out, std::vector<std::string>& notes) {
	std::string work = strip_bom(in);

	if (looks_like_srt(work)) {
		out = srt_to_lrc(work);
		notes.push_back("SRT 已转换为 LRC");
		return !out.empty();
	}

	bool changed = false;
	if (replace_all_ci(work, "<br />", "\n")) changed = true;
	if (replace_all_ci(work, "<br/>", "\n")) changed = true;
	if (replace_all_ci(work, "<br>", "\n")) changed = true;

	std::string before_entities = work;
	decode_entities(work);
	if (work != before_entities) changed = true;

	auto lines = split_lines(work);
	std::vector<std::string> kept;
	kept.reserve(lines.size());
	bool removed_ad = false;
	for (auto& l : lines) {
		std::string t = trim_copy(l);
		if (t.empty()) {
			kept.push_back(std::string());
			continue;
		}
		if (has_ad_line(t)) {
			removed_ad = true;
			changed = true;
			continue;
		}
		kept.push_back(t);
	}

	// 压缩连续空行
	std::vector<std::string> compact;
	for (auto& l : kept) {
		if (l.empty() && !compact.empty() && compact.back().empty()) continue;
		compact.push_back(l);
	}
	while (!compact.empty() && compact.back().empty()) compact.pop_back();

	out = join_lines(compact);
	if (out != trim_copy(strip_bom(in))) changed = true;
	if (removed_ad) notes.push_back("已移除广告行");
	if (changed && notes.empty()) notes.push_back("已清理网页残留");
	return changed;
}

std::string inject_credits(const std::string& lrc, const std::string& zuoci, const std::string& zuoqu) {
	std::string existing_zc, existing_zq;
	extract_credits(lrc, existing_zc, existing_zq);

	std::vector<std::string> add;
	if (!zuoci.empty() && existing_zc.empty())
		add.push_back("[00:00.00]作词：" + zuoci);
	if (!zuoqu.empty() && existing_zq.empty())
		add.push_back("[00:00.00]作曲：" + zuoqu);
	if (add.empty()) return lrc;

	std::string body = strip_bom(lrc);
	auto lines = split_lines(body);

	size_t pos = 0;
	while (pos < lines.size()) {
		std::string t = trim_copy(lines[pos]);
		if (t.empty()) { pos++; continue; }
		if (t[0] == '[') {
			size_t close = t.find(']');
			if (close != std::string::npos && is_meta_tag(t.substr(1, close - 1))) {
				pos++;
				continue;
			}
		}
		break;
	}

	std::vector<std::string> result;
	result.insert(result.end(), lines.begin(), lines.begin() + pos);
	result.insert(result.end(), add.begin(), add.end());
	result.insert(result.end(), lines.begin() + pos, lines.end());
	return join_lines(result);
}

bool extract_credits(const std::string& content, std::string& zuoci, std::string& zuoqu) {
	parsed_lyric p = parse_lrc(strip_bom(content));
	collect_credits(p, zuoci, zuoqu);
	return !zuoci.empty() && !zuoqu.empty();
}

const char* verdict_name(verdict v) {
	switch (v) {
	case verdict::ok: return "合格";
	case verdict::missing_header: return "缺头部";
	case verdict::needs_repair: return "可本地修复";
	case verdict::needs_replace: return "需替换";
	case verdict::no_candidate: return "无候选";
	}
	return "未知";
}

} // namespace lyrics
