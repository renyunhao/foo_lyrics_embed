#include "stdafx.h"

#include "lyrics_sources.h"
#include "lyrics_quality.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>

namespace lyrics {

namespace {

const char* kUserAgent = "foo_lyrics_embed/2.0 (foobar2000)";

void log_msg(const log_fn& log, const std::string& text) {
	if (log) log(text);
}

// ---------------------------------------------------------------- JSON

namespace jsonx {

struct value {
	enum class type { nullv, boolean, number, string, array, object };
	type t = type::nullv;
	bool b = false;
	double num = 0;
	std::string str;
	std::vector<value> arr;
	std::vector<std::pair<std::string, value>> obj;

	const value* find(const char* key) const {
		if (t != type::object) return nullptr;
		for (auto& kv : obj)
			if (kv.first == key) return &kv.second;
		return nullptr;
	}
	const value* at(size_t i) const {
		return (t == type::array && i < arr.size()) ? &arr[i] : nullptr;
	}
	std::string s() const { return t == type::string ? str : std::string(); }
	double n() const { return t == type::number ? num : 0.0; }
	bool flag() const { return t == type::boolean ? b : false; }
};

class parser {
public:
	parser(const std::string& s) : m_s(s) {}

	bool run(value& out) {
		skip_ws();
		if (!parse_value(out)) return false;
		skip_ws();
		return m_ok;
	}

private:
	const std::string& m_s;
	size_t m_i = 0;
	bool m_ok = true;

	void skip_ws() {
		while (m_i < m_s.size()) {
			char c = m_s[m_i];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') m_i++;
			else break;
		}
	}

	bool consume(char c) {
		if (m_i < m_s.size() && m_s[m_i] == c) { m_i++; return true; }
		return false;
	}

	bool parse_value(value& out) {
		if (!m_ok || m_i >= m_s.size()) { m_ok = false; return false; }
		char c = m_s[m_i];
		switch (c) {
		case '{': return parse_object(out);
		case '[': return parse_array(out);
		case '"': out.t = value::type::string; return parse_string(out.str);
		case 't':
			if (m_s.compare(m_i, 4, "true") == 0) { m_i += 4; out.t = value::type::boolean; out.b = true; return true; }
			m_ok = false; return false;
		case 'f':
			if (m_s.compare(m_i, 5, "false") == 0) { m_i += 5; out.t = value::type::boolean; out.b = false; return true; }
			m_ok = false; return false;
		case 'n':
			if (m_s.compare(m_i, 4, "null") == 0) { m_i += 4; out.t = value::type::nullv; return true; }
			m_ok = false; return false;
		default:
			return parse_number(out);
		}
	}

	bool parse_number(value& out) {
		size_t start = m_i;
		if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) m_i++;
		bool any = false;
		while (m_i < m_s.size() && m_s[m_i] >= '0' && m_s[m_i] <= '9') { m_i++; any = true; }
		if (m_i < m_s.size() && m_s[m_i] == '.') {
			m_i++;
			while (m_i < m_s.size() && m_s[m_i] >= '0' && m_s[m_i] <= '9') { m_i++; any = true; }
		}
		if (m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E')) {
			m_i++;
			if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) m_i++;
			while (m_i < m_s.size() && m_s[m_i] >= '0' && m_s[m_i] <= '9') m_i++;
		}
		if (!any) { m_ok = false; return false; }
		out.t = value::type::number;
		out.num = atof(m_s.substr(start, m_i - start).c_str());
		return true;
	}

	bool parse_string(std::string& out) {
		if (!consume('"')) { m_ok = false; return false; }
		out.clear();
		while (m_i < m_s.size()) {
			char c = m_s[m_i++];
			if (c == '"') return true;
			if (static_cast<unsigned char>(c) < 0x20) { m_ok = false; return false; }
			if (c != '\\') { out.push_back(c); continue; }

			if (m_i >= m_s.size()) { m_ok = false; return false; }
			char e = m_s[m_i++];
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				uint32_t cp = 0;
				if (!read_hex4(cp)) { m_ok = false; return false; }
				if (cp >= 0xD800 && cp <= 0xDBFF && m_i + 5 < m_s.size() &&
					m_s[m_i] == '\\' && m_s[m_i + 1] == 'u') {
					uint32_t low = 0;
					size_t save = m_i;
					m_i += 2;
					if (read_hex4(low) && low >= 0xDC00 && low <= 0xDFFF)
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					else
						m_i = save;
				}
				append_utf8(out, cp);
				break;
			}
			default:
				m_ok = false;
				return false;
			}
		}
		m_ok = false;
		return false;
	}

	bool read_hex4(uint32_t& out) {
		if (m_i + 4 > m_s.size()) return false;
		out = 0;
		for (int k = 0; k < 4; k++) {
			char c = lower_ascii(m_s[m_i++]);
			out <<= 4;
			if (c >= '0' && c <= '9') out |= (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f') out |= (uint32_t)(c - 'a' + 10);
			else return false;
		}
		return true;
	}

	bool parse_array(value& out) {
		consume('[');
		out.t = value::type::array;
		skip_ws();
		if (consume(']')) return true;
		while (m_i < m_s.size()) {
			value item;
			skip_ws();
			if (!parse_value(item)) { m_ok = false; return false; }
			out.arr.push_back(std::move(item));
			skip_ws();
			if (consume(']')) return true;
			if (!consume(',')) { m_ok = false; return false; }
		}
		m_ok = false;
		return false;
	}

	bool parse_object(value& out) {
		consume('{');
		out.t = value::type::object;
		skip_ws();
		if (consume('}')) return true;
		while (m_i < m_s.size()) {
			skip_ws();
			std::string key;
			if (!parse_string(key)) { m_ok = false; return false; }
			skip_ws();
			if (!consume(':')) { m_ok = false; return false; }
			skip_ws();
			value item;
			if (!parse_value(item)) { m_ok = false; return false; }
			out.obj.emplace_back(std::move(key), std::move(item));
			skip_ws();
			if (consume('}')) return true;
			if (!consume(',')) { m_ok = false; return false; }
		}
		m_ok = false;
		return false;
	}
};

} // namespace jsonx

bool parse_json(const std::string& raw, jsonx::value& out) {
	std::string s = trim_copy(raw);
	// 去掉可能的 JSONP 包装
	size_t first = s.find('{');
	size_t first_arr = s.find('[');
	if (first == std::string::npos || (first_arr != std::string::npos && first_arr < first)) first = first_arr;
	if (first == std::string::npos) return false;
	s = s.substr(first);
	jsonx::parser p(s);
	return p.run(out);
}

// ---------------------------------------------------------------- 基础工具

std::string base64_decode(const std::string& in) {
	auto value_of = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+' || c == '-') return 62;
		if (c == '/' || c == '_') return 63;
		return -1;
	};
	std::string out;
	int buf = 0, bits = 0;
	for (char c : in) {
		int v = value_of(c);
		if (v < 0) continue;
		buf = (buf << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((char)((buf >> bits) & 0xFF));
		}
	}
	return out;
}

// ---------------------------------------------------------------- 站点并发限制

std::mutex g_host_mutex;
std::condition_variable g_host_cv;
int g_host_limit = 3;
std::map<std::string, int> g_host_active;

std::string url_host(const std::string& url) {
	size_t scheme = url.find("://");
	size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
	size_t end = url.find('/', start);
	return lowercase_ascii(url.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

class host_slot {
public:
	host_slot(const std::string& host, abort_callback& aborter) : m_host(host) {
		std::unique_lock<std::mutex> lock(g_host_mutex);
		for (;;) {
			if (aborter.is_aborting()) throw exception_aborted();
			int& active = g_host_active[m_host];
			if (active < g_host_limit) {
				active++;
				m_held = true;
				break;
			}
			g_host_cv.wait_for(lock, std::chrono::milliseconds(100));
		}
	}

	~host_slot() { release(); }

private:
	void release() {
		if (!m_held) return;
		m_held = false;
		{
			std::lock_guard<std::mutex> lock(g_host_mutex);
			auto it = g_host_active.find(m_host);
			if (it != g_host_active.end() && it->second > 0) it->second--;
		}
		g_host_cv.notify_one();
	}

	std::string m_host;
	bool m_held = false;
};

bool is_transient_http_error(const std::string& what) {
	return what.find("429") != std::string::npos ||
		what.find("503") != std::string::npos ||
		what.find("Too Many Requests") != std::string::npos ||
		what.find("Service Unavailable") != std::string::npos;
}

std::string http_get_once(const std::string& url, const std::vector<std::string>& headers,
	abort_callback& aborter) {
	host_slot slot(url_host(url), aborter);

	auto client = http_client::get();
	service_ptr_t<http_request> req = client->create_request("GET");
	req->add_header((std::string("User-Agent: ") + kUserAgent).c_str());
	req->add_header("Accept: */*");
	for (auto& h : headers) req->add_header(h.c_str());

	file::ptr f = req->run(url.c_str(), aborter);

	std::string out;
	std::vector<char> buf(64 * 1024);
	const size_t kMax = 4 * 1024 * 1024;
	while (out.size() < kMax) {
		t_size got = f->read(buf.data(), buf.size(), aborter);
		if (got == 0) break;
		out.append(buf.data(), got);
	}
	return out;
}

std::string http_get(const std::string& url, const std::vector<std::string>& headers, abort_callback& aborter) {
	for (int attempt = 0;; attempt++) {
		try {
			return http_get_once(url, headers, aborter);
		} catch (exception_aborted const&) {
			throw;
		} catch (std::exception const& e) {
			if (attempt == 0 && is_transient_http_error(e.what())) {
				aborter.sleep(0.8);
				continue;
			}
			throw;
		}
	}
}

struct source_song {
	std::string source;
	std::string title;
	std::string artist;
	std::string album;
	double duration = -1.0;
	std::string id;
	int priority = 0;
};

int score_song(const track_meta& track, const source_song& song, bool& ok) {
	ok = false;
	std::string nt = normalize_for_match(track.title);
	std::string ns = normalize_for_match(song.title);
	double title_sim = text_similarity(nt, ns);
	if (title_sim < 0.55) return 0;

	double diff = -1.0;
	if (track.duration > 0.5 && song.duration > 0.5)
		diff = fabs(track.duration - song.duration);
	if (diff >= 0.0 && diff > 5.0) return 0;

	bool artist_ok = artist_matches(track.artist, song.artist);
	if (!artist_ok && title_sim < 0.75) return 0;

	int score = (int)(title_sim * 60.0);
	if (artist_ok) score += 20;
	if (diff >= 0.0) score += (std::max)(0, 20 - (int)(diff * 4.0));
	else score += 10;
	ok = true;
	return score;
}

std::string join_artists_from_json(const jsonx::value* arr, const char* field) {
	std::string out;
	if (!arr || arr->t != jsonx::value::type::array) return out;
	for (auto& item : arr->arr) {
		const jsonx::value* name = item.find(field);
		if (!name) continue;
		if (!out.empty()) out += "/";
		out += name->s();
	}
	return out;
}

// ---------------------------------------------------------------- 网易云

std::vector<source_song> netease_search(const track_meta& meta, abort_callback& aborter) {
	std::vector<source_song> out;
	std::string q = meta.title;
	if (!meta.artist.empty()) q += " " + meta.artist;

	std::string url = "https://music.163.com/api/search/get/web?s=" + url_encode(q) +
		"&type=1&offset=0&total=true&limit=10";
	std::string body = http_get(url, { "Referer: https://music.163.com" }, aborter);

	jsonx::value root;
	if (!parse_json(body, root)) return out;

	const jsonx::value* result = root.find("result");
	if (!result) return out;
	const jsonx::value* songs = result->find("songs");
	if (!songs || songs->t != jsonx::value::type::array) return out;

	for (auto& item : songs->arr) {
		source_song s;
		s.source = "网易云音乐";
		s.priority = 6;
		const jsonx::value* id = item.find("id");
		const jsonx::value* name = item.find("name");
		if (!id || !name) continue;
		char idbuf[32];
		snprintf(idbuf, sizeof(idbuf), "%.0f", id->n());
		s.id = idbuf;
		s.title = name->s();
		s.artist = join_artists_from_json(item.find("artists"), "name");
		if (s.artist.empty()) s.artist = join_artists_from_json(item.find("ar"), "name");
		const jsonx::value* album = item.find("album");
		if (album) {
			const jsonx::value* an = album->find("name");
			if (an) s.album = an->s();
		}
		const jsonx::value* dur = item.find("duration");
		if (dur && dur->t == jsonx::value::type::number) s.duration = dur->n() / 1000.0;
		out.push_back(s);
	}
	return out;
}

std::string netease_lyric(const std::string& id, abort_callback& aborter) {
	if (id.empty()) return std::string();
	std::string url = "https://music.163.com/api/song/lyric?id=" + id + "&lv=1&kv=1&tv=-1";
	std::string body = http_get(url, { "Referer: https://music.163.com" }, aborter);

	jsonx::value root;
	if (!parse_json(body, root)) return std::string();
	const jsonx::value* no_lyric = root.find("nolyric");
	const jsonx::value* uncollected = root.find("uncollected");
	if ((no_lyric && no_lyric->flag()) || (uncollected && uncollected->flag())) return std::string();
	const jsonx::value* lrc = root.find("lrc");
	if (!lrc) return std::string();
	const jsonx::value* lyric = lrc->find("lyric");
	if (!lyric) return std::string();
	return lyric->s();
}

// ---------------------------------------------------------------- QQ 音乐

std::vector<source_song> qq_search(const track_meta& meta, abort_callback& aborter) {
	std::vector<source_song> out;
	std::string q = meta.title;
	if (!meta.artist.empty()) q += " " + meta.artist;

	std::string url = "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&n=10&p=1&w=" +
		url_encode(q);
	std::string body = http_get(url, { "Referer: https://y.qq.com" }, aborter);

	jsonx::value root;
	if (!parse_json(body, root)) return out;

	const jsonx::value* data = root.find("data");
	if (!data) return out;
	const jsonx::value* song = data->find("song");
	if (!song) return out;
	const jsonx::value* list = song->find("list");
	if (!list || list->t != jsonx::value::type::array) return out;

	for (auto& item : list->arr) {
		source_song s;
		s.source = "QQ音乐";
		s.priority = 4;
		const jsonx::value* mid = item.find("songmid");
		const jsonx::value* name = item.find("songname");
		if (!mid || !name) continue;
		s.id = mid->s();
		s.title = name->s();
		s.artist = join_artists_from_json(item.find("singer"), "name");
		const jsonx::value* album = item.find("albumname");
		if (album) s.album = album->s();
		const jsonx::value* interval = item.find("interval");
		if (interval && interval->t == jsonx::value::type::number) s.duration = interval->n();
		out.push_back(s);
	}
	return out;
}

std::string qq_lyric(const std::string& songmid, abort_callback& aborter) {
	if (songmid.empty()) return std::string();
	char cache[32];
	snprintf(cache, sizeof(cache), "%lld", (long long)time(nullptr) * 1000);
	std::string url = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=" +
		url_encode(songmid) + "&pcachetime=" + cache +
		"&g_tk=5381&loginUin=0&hostUin=0&inCharset=utf8&outCharset=utf-8&notice=0&platform=yqq&needNewCode=1&format=json";
	std::string body = http_get(url, { "Referer: https://y.qq.com" }, aborter);

	jsonx::value root;
	if (!parse_json(body, root)) return std::string();
	const jsonx::value* lyric = root.find("lyric");
	if (!lyric) return std::string();
	return base64_decode(lyric->s());
}

// ---------------------------------------------------------------- LRCLIB

struct lrclib_item {
	source_song song;
	std::string lyric;
};

std::vector<lrclib_item> lrclib_search(const track_meta& meta, abort_callback& aborter) {
	std::vector<lrclib_item> out;
	std::string url = "https://lrclib.net/api/search?track_name=" + url_encode(meta.title);
	if (!meta.artist.empty()) url += "&artist_name=" + url_encode(meta.artist);

	std::string body = http_get(url, {}, aborter);
	jsonx::value root;
	if (!parse_json(body, root)) return out;
	if (root.t != jsonx::value::type::array) return out;

	for (auto& item : root.arr) {
		lrclib_item it;
		it.song.source = "LRCLIB";
		it.song.priority = 2;
		const jsonx::value* name = item.find("trackName");
		const jsonx::value* artist = item.find("artistName");
		const jsonx::value* album = item.find("albumName");
		const jsonx::value* dur = item.find("duration");
		const jsonx::value* synced = item.find("syncedLyrics");
		if (!name || !synced) continue;
		it.song.title = name->s();
		if (artist) it.song.artist = artist->s();
		if (album) it.song.album = album->s();
		if (dur && dur->t == jsonx::value::type::number) it.song.duration = dur->n();
		it.lyric = synced->s();
		if (trim_copy(it.lyric).empty()) continue;
		out.push_back(std::move(it));
	}
	return out;
}

// ---------------------------------------------------------------- 组合逻辑

struct best_candidate {
	bool valid = false;
	int total = 0;
	std::string content;
	std::string source;
	quality_result quality;
};

bool accept_candidate(const track_meta& track, const source_song& song, std::string content,
	int match_score, best_candidate& best, bool& perfect) {
	perfect = false;

	quality_result q = evaluate(content, track.duration);
	if (q.instrumental) return false;

	if (q.v == verdict::needs_repair) {
		std::string fixed;
		std::vector<std::string> notes;
		if (!repair_local(content, fixed, notes)) return false;
		content = fixed;
		q = evaluate(content, track.duration);
	}
	if (!(q.v == verdict::ok || q.v == verdict::missing_header)) return false;

	int total = match_score + song.priority + (q.has_zuoci && q.has_zuoqu ? 3 : 0);
	if (!best.valid || total > best.total) {
		best.valid = true;
		best.total = total;
		best.content = content;
		best.source = song.source;
		best.quality = q;
	}
	perfect = (q.v == verdict::ok && total >= 95);
	return true;
}

} // namespace

bool lookup_credits(const track_meta& meta, std::string& zuoci, std::string& zuoqu,
	abort_callback& aborter, const log_fn& log) {
	if (!zuoci.empty() && !zuoqu.empty()) return true;

	auto try_lyric = [&](const std::string& lyric) {
		if (lyric.empty()) return false;
		std::string zc, zq;
		extract_credits(lyric, zc, zq);
		if (zuoci.empty() && !zc.empty()) zuoci = zc;
		if (zuoqu.empty() && !zq.empty()) zuoqu = zq;
		return !zuoci.empty() && !zuoqu.empty();
	};

	try {
		log_msg(log, "  补头部：查询网易云");
		auto songs = netease_search(meta, aborter);
		log_msg(log, songs.empty() ? "  补头部：网易云无结果"
			: ("  补头部：网易云 " + std::to_string(songs.size()) + " 个候选"));
		int tried = 0;
		for (auto& s : songs) {
			aborter.check();
			bool ok = false;
			score_song(meta, s, ok);
			if (!ok) continue;
			if (++tried > 2) break;
			if (try_lyric(netease_lyric(s.id, aborter))) {
				log_msg(log, "  补头部：网易云取得作词/作曲");
				return true;
			}
		}
	} catch (exception_aborted const&) {
		throw;
	} catch (...) {
	}

	try {
		log_msg(log, "  补头部：查询 QQ 音乐");
		auto songs = qq_search(meta, aborter);
		log_msg(log, songs.empty() ? "  补头部：QQ 音乐无结果"
			: ("  补头部：QQ 音乐 " + std::to_string(songs.size()) + " 个候选"));
		int tried = 0;
		for (auto& s : songs) {
			aborter.check();
			bool ok = false;
			score_song(meta, s, ok);
			if (!ok) continue;
			if (++tried > 2) break;
			if (try_lyric(qq_lyric(s.id, aborter))) {
				log_msg(log, "  补头部：QQ 音乐取得作词/作曲");
				return true;
			}
		}
	} catch (exception_aborted const&) {
		throw;
	} catch (...) {
	}

	return !zuoci.empty() && !zuoqu.empty();
}

fetch_result find_lyrics(const track_meta& meta, abort_callback& aborter, const log_fn& log) {
	fetch_result result;
	best_candidate best;
	std::string last_detail = "未找到匹配的歌词";
	bool stop_early = false;

	auto consider = [&](const source_song& song, const std::string& content, int match_score) {
		bool perfect = false;
		bool accepted = accept_candidate(meta, song, content, match_score, best, perfect);
		if (!accepted) last_detail = "候选歌词未通过规范校验";
		return accepted && perfect;
	};

	// 1) 网易云
	try {
		log_msg(log, "  网易云：搜索中");
		auto songs = netease_search(meta, aborter);
		if (songs.empty()) {
			last_detail = "网易云无搜索结果";
			log_msg(log, "  网易云：无结果");
		} else {
			log_msg(log, "  网易云：" + std::to_string(songs.size()) + " 个候选");
		}
		int tried = 0;
		for (auto& s : songs) {
			aborter.check();
			bool ok = false;
			int sc = score_song(meta, s, ok);
			if (!ok) continue;
			if (++tried > 2) break;
			log_msg(log, "  网易云：读取《" + s.title + "》");
			std::string lyric = netease_lyric(s.id, aborter);
			if (lyric.empty()) continue;
			if (consider(s, lyric, sc)) {
				log_msg(log, "  网易云：命中");
				stop_early = true;
				break;
			}
			log_msg(log, "  网易云：候选未通过校验，继续");
		}
	} catch (exception_aborted const&) {
		throw;
	} catch (std::exception const& e) {
		last_detail = std::string("网易云请求失败：") + e.what();
		log_msg(log, "  网易云：" + last_detail);
	}

	// 2) QQ 音乐
	if (!stop_early) {
		try {
			log_msg(log, "  QQ 音乐：搜索中");
			auto songs = qq_search(meta, aborter);
			if (songs.empty()) {
				last_detail = "QQ音乐无搜索结果";
				log_msg(log, "  QQ 音乐：无结果");
			} else {
				log_msg(log, "  QQ 音乐：" + std::to_string(songs.size()) + " 个候选");
			}
			int tried = 0;
			for (auto& s : songs) {
				aborter.check();
				bool ok = false;
				int sc = score_song(meta, s, ok);
				if (!ok) continue;
				if (++tried > 2) break;
				log_msg(log, "  QQ 音乐：读取《" + s.title + "》");
				std::string lyric = qq_lyric(s.id, aborter);
				if (lyric.empty()) continue;
				if (consider(s, lyric, sc)) {
					log_msg(log, "  QQ 音乐：命中");
					stop_early = true;
					break;
				}
				log_msg(log, "  QQ 音乐：候选未通过校验，继续");
			}
		} catch (exception_aborted const&) {
			throw;
		} catch (std::exception const& e) {
			last_detail = std::string("QQ音乐请求失败：") + e.what();
			log_msg(log, "  QQ 音乐：" + last_detail);
		}
	}

	// 3) LRCLIB
	if (!stop_early) {
		try {
			log_msg(log, "  LRCLIB：搜索中");
			auto items = lrclib_search(meta, aborter);
			if (items.empty()) {
				last_detail = "LRCLIB无搜索结果";
				log_msg(log, "  LRCLIB：无结果");
			} else {
				log_msg(log, "  LRCLIB：" + std::to_string(items.size()) + " 个候选");
			}
			int tried = 0;
			for (auto& it : items) {
				aborter.check();
				bool ok = false;
				int sc = score_song(meta, it.song, ok);
				if (!ok) continue;
				if (++tried > 2) break;
				log_msg(log, "  LRCLIB：检查《" + it.song.title + "》");
				if (consider(it.song, it.lyric, sc)) {
					log_msg(log, "  LRCLIB：命中");
					stop_early = true;
					break;
				}
			}
		} catch (exception_aborted const&) {
			throw;
		} catch (std::exception const& e) {
			last_detail = std::string("LRCLIB请求失败：") + e.what();
			log_msg(log, "  LRCLIB：" + last_detail);
		}
	}

	if (!best.valid) {
		result.detail = last_detail;
		return result;
	}

	// 采用所有候选中得分最高的一条，而不是第一个通过的
	std::string content = best.content;
	if (best.quality.v == verdict::missing_header) {
		log_msg(log, "  候选缺作词/作曲，尝试补齐");
		std::string zc = best.quality.zuoci;
		std::string zq = best.quality.zuoqu;
		lookup_credits(meta, zc, zq, aborter, log);
		if (zc.empty() || zq.empty()) {
			result.found = false;
			result.detail = "候选歌词缺少作词/作曲，且未能补齐";
			log_msg(log, "  " + result.detail);
			return result;
		}
		content = inject_credits(content, zc, zq);
		quality_result q = evaluate(content, meta.duration);
		if (q.v != verdict::ok) {
			result.found = false;
			result.detail = "补齐作词/作曲后仍未通过规范校验";
			log_msg(log, "  " + result.detail);
			return result;
		}
	}

	log_msg(log, "  采用：" + best.source);
	result.found = true;
	result.content = content;
	result.source = best.source;
	return result;
}

void set_http_host_limit(int limit) {
	if (limit < 1) limit = 1;
	if (limit > 16) limit = 16;
	std::lock_guard<std::mutex> lock(g_host_mutex);
	g_host_limit = limit;
	g_host_cv.notify_all();
}

} // namespace lyrics
