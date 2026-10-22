#include "stdafx.h"

#include "lyrics_common.h"
#include "lyrics_log_window.h"
#include "lyrics_quality.h"
#include "lyrics_sources.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <SDK/messageBox.h>
#include <shellapi.h>

using namespace lyrics;

namespace {

const char* kReportFileName = "foo_lyrics_embed_report.txt";
const char* kTagsReportFileName = "foo_lyrics_embed_tags_report.txt";
const char* kCleanupReportFileName = "foo_lyrics_embed_cleanup_report.txt";
const char* kConfigFileName = "foo_lyrics_embed_config.txt";

enum class batch_mode { lyrics, tags, cleanup };

// ---------------------------------------------------------------- 忽略目录

bool profile_file_path(const char* name, pfc::string8& out) {
	try {
		pfc::string8 native = filesystem::g_get_native_path(core_api::get_profile_path(), fb2k::noAbort);
		if (strstr(native.get_ptr(), "://")) return false;
		out = native;
		if (!out.is_empty()) out += "\\";
		out += name;
		return true;
	} catch (...) {
		return false;
	}
}

std::string normalize_path_for_match(const std::string& in) {
	std::string s = lowercase_ascii(in);
	for (auto& c : s) {
		if (c == '/') c = '\\';
	}
	return s;
}

void add_skip_entry(const std::string& raw, std::vector<std::string>& out) {
	std::string s = trim_copy(raw);
	if (s.empty() || s[0] == '#') return;
	while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
	if (!s.empty()) out.push_back(normalize_path_for_match(s));
}

bool matches_skip_dir(const std::string& path_lc, const std::string& dir_lc) {
	if (dir_lc.empty()) return false;

	if (dir_lc.find('\\') == std::string::npos) {
		std::string needle = "\\" + dir_lc + "\\";
		if (path_lc.find(needle) != std::string::npos) return true;
		if (path_lc.size() > dir_lc.size() &&
			path_lc.compare(path_lc.size() - dir_lc.size() - 1, dir_lc.size() + 1, "\\" + dir_lc) == 0)
			return true;
		return false;
	}

	if (path_lc.compare(0, dir_lc.size(), dir_lc) == 0 &&
		(path_lc.size() == dir_lc.size() || path_lc[dir_lc.size()] == '\\'))
		return true;
	return path_lc.find("\\" + dir_lc + "\\") != std::string::npos;
}

bool is_skipped_path(const char* raw_path, const std::vector<std::string>& dirs, abort_callback& aborter) {
	if (dirs.empty()) return false;
	std::string raw_lc = normalize_path_for_match(raw_path ? raw_path : "");
	std::string native_lc;
	try {
		native_lc = normalize_path_for_match(filesystem::g_get_native_path(raw_path, aborter).c_str());
	} catch (...) {
	}
	for (auto& d : dirs) {
		if (matches_skip_dir(raw_lc, d)) return true;
		if (!native_lc.empty() && matches_skip_dir(native_lc, d)) return true;
	}
	return false;
}

const char* kConfigTemplate =
	"# foo_lyrics_embed 配置（UTF-8，修改后下次运行生效）\r\n"
	"\r\n"
	"[options]\r\n"
	"# 同时处理的曲目数（1-16），1 表示串行\r\n"
	"concurrency=5\r\n"
	"# 同一歌词站点的最大并发请求数（1-16）\r\n"
	"per_host=3\r\n"
	"\r\n"
	"[skip]\r\n"
	"# 每行一个要跳过扫描的目录，匹配不区分大小写\r\n"
	"# 支持文件夹名（Tune）、路径片段（Music\\Tune）或完整路径（D:\\Music\\Tune）\r\n"
	"# Tune\r\n"
	"\r\n"
	"[filename]\r\n"
	"# 文件名推断模板（不含扩展名），必须包含 {title}，可选 {artist}\r\n"
	"# 示例：文件名 \"周杰伦 - 稻香.mp3\" 对应 \"{artist} - {title}\"\r\n"
	"{artist} - {title}\r\n";

bool read_profile_text(const char* name, std::string& text) {
	text.clear();
	pfc::string8 full;
	if (!profile_file_path(name, full)) return false;
	try {
		file::ptr f;
		filesystem::g_open_read(f, full, fb2k::noAbort);
		std::vector<char> buf(8192);
		for (;;) {
			t_size got = f->read(buf.data(), buf.size(), fb2k::noAbort);
			if (got == 0) break;
			text.append(buf.data(), got);
		}
		return true;
	} catch (...) {
		return false;
	}
}

struct run_options {
	int concurrency = 5;
	int per_host = 3;
};

struct app_config {
	run_options options;
	std::vector<std::string> skip_dirs;
	std::string filename_template;
};

int parse_option_int(const std::string& value, int lo, int hi, int fallback) {
	char* end = nullptr;
	long v = strtol(value.c_str(), &end, 10);
	if (end == value.c_str()) return fallback;
	if (v < lo) v = lo;
	if (v > hi) v = hi;
	return (int)v;
}

void apply_option(const std::string& key, const std::string& value, run_options& o) {
	if (key == "concurrency") o.concurrency = parse_option_int(value, 1, 16, o.concurrency);
	else if (key == "per_host") o.per_host = parse_option_int(value, 1, 16, o.per_host);
}

// ---- 统一配置文件

app_config parse_config_text(const std::string& raw) {
	app_config cfg;
	std::string section = "options";

	for (auto& line : split_lines(strip_bom(raw))) {
		std::string s = trim_copy(line);
		if (s.empty() || s[0] == '#') continue;

		if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
			std::string name = lowercase_ascii(trim_copy(s.substr(1, s.size() - 2)));
			if (name == "options" || name == "skip" || name == "filename") {
				section = name;
				continue;
			}
		}
		if (section == "skip") {
			add_skip_entry(s, cfg.skip_dirs);
			continue;
		}
		if (section == "filename") {
			if (cfg.filename_template.empty()) cfg.filename_template = s;
			continue;
		}

		size_t eq = s.find('=');
		if (eq != std::string::npos) {
			apply_option(lowercase_ascii(trim_copy(s.substr(0, eq))),
				trim_copy(s.substr(eq + 1)), cfg.options);
		}
	}
	return cfg;
}

app_config load_config() {
	app_config cfg;
	std::string text;
	if (read_profile_text(kConfigFileName, text)) return parse_config_text(text);
	return cfg;
}

void open_config_file(const char* name, const char* default_content, const char* title) {
	pfc::string8 full;
	if (!profile_file_path(name, full)) {
		popup_message::g_show("无法确定配置目录路径。", "内嵌歌词");
		return;
	}

	if (filesystem::g_exists(full, fb2k::noAbort) == false) {
		try {
			file::ptr f;
			filesystem::g_open_write_new(f, full, fb2k::noAbort);
			f->write(default_content, strlen(default_content), fb2k::noAbort);
		} catch (std::exception const& e) {
			popup_message::g_complain(title, e);
			return;
		}
	}

	pfc::stringcvt::string_wide_from_utf8 wide(full);
	ShellExecuteW(nullptr, L"open", L"notepad.exe", wide, nullptr, SW_SHOWNORMAL);
}

struct item_result {
	std::string path;
	std::string title;
	std::string artist;
	std::string verdict;
	std::string action;
	std::string source;
	std::string reason;
	std::string content;
	std::string patch_title;
	std::string patch_artist;
	int score = 0;
	bool changed = false;
	bool dry_run = false;
	bool failed = false;
	bool written = false;
	bool processed = false;
	bool remove_original = false;
};

std::string meta_join(const file_info& info, const char* name) {
	std::string out;
	t_size count = info.meta_get_count_by_name(name);
	for (t_size i = 0; i < count; i++) {
		const char* v = info.meta_get(name, i);
		if (!v || !*v) continue;
		if (!out.empty()) out += "/";
		out += v;
	}
	return out;
}

std::string join_reasons(const std::vector<std::string>& reasons) {
	std::string out;
	for (auto& r : reasons) {
		if (!out.empty()) out += "；";
		out += r;
	}
	return out;
}

std::string now_string() {
	time_t t = time(nullptr);
	tm local;
	localtime_s(&local, &t);
	char buf[64];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
	return buf;
}

bool same_content(const std::string& a, const std::string& b) {
	return trim_copy(strip_bom(a)) == trim_copy(strip_bom(b));
}

// ---------------------------------------------------------------- 文件名模板

std::string file_stem(const std::string& path) {
	size_t slash = path.find_last_of("\\/");
	std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	size_t dot = name.find_last_of('.');
	if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
	return name;
}

struct filename_template {
	std::vector<std::string> literals; // literals.size() == fields.size() + 1
	std::vector<int> fields;           // 0 = artist, 1 = title
	bool valid() const { return !fields.empty(); }
};

filename_template parse_filename_template(const std::string& text) {
	filename_template t;
	std::string lit;
	size_t i = 0;
	while (i < text.size()) {
		if (text.compare(i, 8, "{artist}") == 0) {
			t.literals.push_back(lit);
			lit.clear();
			t.fields.push_back(0);
			i += 8;
		} else if (text.compare(i, 7, "{title}") == 0) {
			t.literals.push_back(lit);
			lit.clear();
			t.fields.push_back(1);
			i += 7;
		} else {
			lit += text[i++];
		}
	}
	t.literals.push_back(lit);

	bool has_title = false;
	for (int f : t.fields) if (f == 1) has_title = true;
	if (!has_title) return parse_filename_template("{artist} - {title}");
	return t;
}

bool match_filename_template(const filename_template& t, const std::string& stem,
	std::string& artist, std::string& title) {
	if (!t.valid()) return false;

	size_t pos = 0;
	const std::string& prefix = t.literals[0];
	if (!prefix.empty()) {
		if (stem.compare(0, prefix.size(), prefix) != 0) return false;
		pos = prefix.size();
	}

	for (size_t i = 0; i < t.fields.size(); i++) {
		const std::string& next = t.literals[i + 1];
		std::string value;

		if (i + 1 == t.fields.size()) {
			if (next.empty()) {
				value = stem.substr(pos);
			} else {
				if (stem.size() < pos + next.size()) return false;
				if (stem.compare(stem.size() - next.size(), next.size(), next) != 0) return false;
				value = stem.substr(pos, stem.size() - next.size() - pos);
			}
		} else {
			size_t found = stem.find(next, pos);
			if (found == std::string::npos) return false;
			value = stem.substr(pos, found - pos);
			pos = found + next.size();
		}

		value = trim_copy(value);
		if (value.empty()) return false;
		if (t.fields[i] == 0) artist = value;
		else title = value;
	}
	return true;
}

bool infer_from_filename(const std::string& path, const filename_template& tpl,
	std::string& artist, std::string& title, bool stem_fallback) {
	std::string stem = file_stem(path);
	artist.clear();
	title.clear();
	if (match_filename_template(tpl, stem, artist, title)) return true;
	if (stem_fallback && !stem.empty()) title = stem;
	return false;
}

bool fill_header(const track_meta& meta, const std::string& content,
	std::string& zuoci, std::string& zuoqu, std::string& out,
	const log_fn& log, abort_callback& aborter) {
	std::string zc = zuoci;
	std::string zq = zuoqu;
	lookup_credits(meta, zc, zq, aborter, log);
	if (zc.empty() || zq.empty()) return false;

	out = inject_credits(content, zc, zq);
	return evaluate(out, meta.duration).v == verdict::ok;
}

void process_one(metadb_handle_ptr item, item_result& r,
	const filename_template& tpl, const log_fn& log, abort_callback& aborter) {
	std::string path = item->get_path();

	if (filesystem::g_is_remote_safe(path.c_str())) {
		r.verdict = "跳过";
		r.action = "跳过";
		r.reason = "网络流或非本地文件";
		return;
	}

	file_info_impl info;
	{
		auto lock = file_lock_manager::get()->acquire_read(path.c_str(), aborter);
		service_ptr_t<input_info_reader> reader;
		input_entry::g_open_for_info_read(reader, nullptr, path.c_str(), aborter);
		reader->get_info(item->get_subsong_index(), info, aborter);
	}

	track_meta meta;
	meta.path = path;
	meta.subsong = item->get_subsong_index();
	meta.title = meta_join(info, "TITLE");
	meta.artist = meta_join(info, "ARTIST");
	meta.album = meta_join(info, "ALBUM");
	meta.duration = info.get_length();

	if (meta.title.empty() || meta.artist.empty()) {
		std::string inferred_artist, inferred_title;
		infer_from_filename(path, tpl, inferred_artist, inferred_title, true);
		if (meta.title.empty() && !inferred_title.empty()) {
			meta.title = inferred_title;
			r.patch_title = inferred_title;
		}
		if (meta.artist.empty() && !inferred_artist.empty()) {
			meta.artist = inferred_artist;
			r.patch_artist = inferred_artist;
		}
	}

	r.title = meta.title;
	r.artist = meta.artist;

	if (meta.title.empty()) {
		r.verdict = "跳过";
		r.action = "跳过";
		r.reason = "文件缺少标题标签且无法从文件名推断";
		return;
	}

	const char* existing_raw = info.meta_get("LYRICS", 0);
	std::string existing = existing_raw ? existing_raw : std::string();

	quality_result q = evaluate(existing, meta.duration);
	r.verdict = verdict_name(q.v);
	r.score = q.score;
	r.reason = join_reasons(q.reasons);

	if (q.v == verdict::ok) {
		r.action = "保持";
		return;
	}

	std::string candidate = existing;
	bool need_fetch = false;

	// 1) 先尝试本地修复（SRT 转换、清洗网页残留）
	if (q.v == verdict::needs_repair) {
		std::string fixed;
		std::vector<std::string> notes;
		if (repair_local(existing, fixed, notes)) {
			quality_result qfixed = evaluate(fixed, meta.duration);
			if (qfixed.v == verdict::ok || qfixed.v == verdict::missing_header) {
				candidate = fixed;
				q = qfixed;
			}
		}
	}

	// 本地修复后已合格
	if (q.v == verdict::ok) {
		if (same_content(candidate, existing)) {
			r.action = "保持";
			r.reason += "；清理后内容无变化";
		} else {
			r.action = "本地修复";
			r.source = "本地";
			r.content = candidate;
			r.changed = true;
		}
		return;
	}

	// 2) 只缺作词/作曲时，只补头部，保留原时间轴
	if (q.v == verdict::missing_header) {
		std::string merged;
		std::string zc = q.zuoci;
		std::string zq = q.zuoqu;
		if (fill_header(meta, candidate, zc, zq, merged, log, aborter)) {
			if (same_content(merged, existing)) {
				r.action = "保持";
				r.reason += "；补头后内容无变化";
			} else {
				r.action = "补头部";
				r.source = "已有歌词 + 在线头部";
				r.reason = join_reasons(q.reasons);
				r.content = merged;
				r.changed = true;
			}
			return;
		}
		need_fetch = true;
	}

	if (q.v == verdict::needs_replace || q.v == verdict::needs_repair || need_fetch) {
		fetch_result fr = find_lyrics(meta, aborter, log);
		if (fr.found) {
			if (same_content(fr.content, existing)) {
				r.action = "保持";
				r.reason += "；搜索结果与现有歌词相同";
			} else {
				r.action = q.v == verdict::needs_repair ? "本地修复失败，整篇替换" : "整篇替换";
				r.source = fr.source;
				r.content = fr.content;
				r.changed = true;
			}
			return;
		}
		r.action = "无候选";
		r.reason = fr.detail;
		return;
	}
}

// 独立功能：只按文件名模板补全 TITLE / ARTIST，不读取或修改歌词
void process_tags_one(metadb_handle_ptr item, item_result& r,
	const filename_template& tpl, abort_callback& aborter) {
	std::string path = item->get_path();

	if (filesystem::g_is_remote_safe(path.c_str())) {
		r.verdict = "跳过";
		r.action = "跳过";
		r.reason = "网络流或非本地文件";
		return;
	}

	file_info_impl info;
	{
		auto lock = file_lock_manager::get()->acquire_read(path.c_str(), aborter);
		service_ptr_t<input_info_reader> reader;
		input_entry::g_open_for_info_read(reader, nullptr, path.c_str(), aborter);
		reader->get_info(item->get_subsong_index(), info, aborter);
	}

	std::string title = meta_join(info, "TITLE");
	std::string artist = meta_join(info, "ARTIST");
	std::string inferred_artist, inferred_title;
	bool matched = infer_from_filename(path, tpl, inferred_artist, inferred_title, false);

	if (title.empty()) {
		if (!matched || inferred_title.empty()) {
			r.title = file_stem(path);
			r.verdict = "跳过";
			r.action = "跳过";
			r.reason = "文件名不符合模板";
			return;
		}
		r.patch_title = inferred_title;
		title = inferred_title;
	}
	if (artist.empty() && matched && !inferred_artist.empty()) {
		r.patch_artist = inferred_artist;
		artist = inferred_artist;
	}

	r.title = title;
	r.artist = artist;
	r.verdict = (r.patch_title.empty() && r.patch_artist.empty()) ? "保持" : "补标签";
	r.action = r.verdict;
	if (r.verdict == "补标签") {
		r.reason = "按文件名模板补全";
		if (!r.patch_title.empty() && !r.patch_artist.empty()) r.reason += " TITLE、ARTIST";
		else if (!r.patch_title.empty()) r.reason += " TITLE";
		else r.reason += " ARTIST";
	}
}

// 独立功能：只删除 LYRICS 的备份 Tag，不读取或修改歌词正文
void process_cleanup_one(metadb_handle_ptr item, item_result& r, abort_callback& aborter) {
	std::string path = item->get_path();

	if (filesystem::g_is_remote_safe(path.c_str())) {
		r.verdict = "跳过";
		r.action = "跳过";
		r.reason = "网络流或非本地文件";
		return;
	}

	file_info_impl info;
	{
		auto lock = file_lock_manager::get()->acquire_read(path.c_str(), aborter);
		service_ptr_t<input_info_reader> reader;
		input_entry::g_open_for_info_read(reader, nullptr, path.c_str(), aborter);
		reader->get_info(item->get_subsong_index(), info, aborter);
	}

	r.title = meta_join(info, "TITLE");
	r.artist = meta_join(info, "ARTIST");

	if (info.meta_find("ORIGINAL_LYRICS") == pfc::infinite_size) {
		r.verdict = "无备份";
		r.action = "无备份";
		r.reason = "没有 ORIGINAL_LYRICS 备份";
		return;
	}

	r.verdict = "有备份";
	r.action = "删除备份";
	r.reason = "删除 ORIGINAL_LYRICS，保留 LYRICS";
	r.remove_original = true;
}

struct tag_patch {
	bool has_lyrics = false;
	bool has_title = false;
	bool has_artist = false;
	bool remove_original = false;
	std::string lyrics;
	std::string title;
	std::string artist;
};

tag_patch make_patch(const item_result& r) {
	tag_patch p;
	if (r.changed && !r.content.empty()) {
		p.has_lyrics = true;
		p.lyrics = r.content;
	}
	if (!r.patch_title.empty()) {
		p.has_title = true;
		p.title = r.patch_title;
	}
	if (!r.patch_artist.empty()) {
		p.has_artist = true;
		p.artist = r.patch_artist;
	}
	p.remove_original = r.remove_original;
	return p;
}

class tag_update_filter : public file_info_filter {
public:
	explicit tag_update_filter(std::map<std::string, tag_patch> changes)
		: m_changes(std::move(changes)) {}

	bool apply_filter(trackRef loc, t_filestats, file_info& info) override {
		auto it = m_changes.find(std::string(loc->get_path()));
		if (it == m_changes.end()) return false;
		const tag_patch& p = it->second;
		bool changed = false;

		if (p.remove_original && info.meta_find("ORIGINAL_LYRICS") != pfc::infinite_size) {
			info.meta_remove_field("ORIGINAL_LYRICS");
			changed = true;
		}
		if (p.has_lyrics) {
			const char* current = info.meta_get("LYRICS", 0);
			if (current && *current && info.meta_find("ORIGINAL_LYRICS") == pfc::infinite_size)
				info.meta_set("ORIGINAL_LYRICS", current);
			info.meta_set("LYRICS", strip_bom(p.lyrics).c_str());
			changed = true;
		}
		if (p.has_title) {
			info.meta_set("TITLE", p.title.c_str());
			changed = true;
		}
		if (p.has_artist) {
			info.meta_set("ARTIST", p.artist.c_str());
			changed = true;
		}
		return changed;
	}

private:
	std::map<std::string, tag_patch> m_changes;
};

class batch_task : public threaded_process_callback {
public:
	batch_task(metadb_handle_list items, bool dry_run, std::vector<std::string> skip_dirs,
		filename_template tpl, batch_mode mode,
		std::shared_ptr<batch_log> log, std::shared_ptr<abort_callback_impl> aborter,
		run_options opts)
		: m_items(std::move(items)), m_dry_run(dry_run), m_skip_dirs(std::move(skip_dirs)),
		  m_tpl(std::move(tpl)), m_mode(mode), m_log(std::move(log)),
		  m_aborter(std::move(aborter)), m_concurrency(opts.concurrency) {}

	void run(threaded_process_status& status, abort_callback& tp_aborter) override {
		abort_callback& aborter = *m_aborter;
		const size_t total = m_items.get_size();
		m_results.clear();
		m_results.resize(total);

		std::mutex write_mutex;
		std::atomic<size_t> next{0};
		std::atomic<size_t> done{0};
		std::atomic<int> kept{0}, changed{0}, no_candidate{0}, skipped{0}, failed{0};
		std::atomic<int> write_status{(int)metadb_io::update_info_success};

		if (m_log) {
			const char* mode_name = m_mode == batch_mode::tags ? "根据文件名推断标签"
				: m_mode == batch_mode::cleanup ? "删除歌词备份" : "整理内嵌歌词";
			std::string head = "开始扫描：" + std::to_string(total) + " 首（" +
				mode_name + "，并发 " + std::to_string(m_concurrency) + "）";
			m_log->log_line(head);
			m_log->set_progress(0, (int)total, 0, 0, 0, 0);
		}

		auto process_index = [&](size_t i, std::mutex& write_guard,
			std::atomic<int>& wstatus, std::atomic<int>& a_kept, std::atomic<int>& a_changed,
			std::atomic<int>& a_no_candidate, std::atomic<int>& a_skipped, std::atomic<int>& a_failed) {
			metadb_handle_ptr item = m_items[i];
			std::string index_prefix = "[" + std::to_string(i + 1) + "/" + std::to_string(total) + "] ";
			std::string display = file_stem(item->get_path());

			item_result r;
			r.path = item->get_path();
			r.dry_run = m_dry_run;
			r.processed = true;

			if (m_mode != batch_mode::cleanup && is_skipped_path(item->get_path(), m_skip_dirs, aborter)) {
				r.verdict = "跳过";
				r.action = "跳过";
				r.reason = "忽略目录（过滤列表）";
				if (m_log) m_log->log_line(index_prefix + "跳过（忽略目录）：" + display);
				m_results[i] = std::move(r);
				a_skipped.fetch_add(1);
				return;
			}

			if (m_log) m_log->log_line(index_prefix + "处理：" + display);

			log_fn item_log = [this, index_prefix](const std::string& s) {
				if (m_log) m_log->log_line(index_prefix + s);
			};

			try {
				if (m_mode == batch_mode::tags)
					process_tags_one(item, r, m_tpl, aborter);
				else if (m_mode == batch_mode::cleanup)
					process_cleanup_one(item, r, aborter);
				else
					process_one(item, r, m_tpl, item_log, aborter);
			} catch (exception_aborted const&) {
				throw;
			} catch (std::exception const& e) {
				r.action = "失败";
				r.failed = true;
				r.reason = e.what();
			}

			if (r.failed) a_failed.fetch_add(1);
			else if (r.action == "跳过") a_skipped.fetch_add(1);
			else if (m_mode == batch_mode::cleanup) {
				if (r.remove_original) a_changed.fetch_add(1);
				else a_no_candidate.fetch_add(1);
			}
			else if (r.action == "无候选") a_no_candidate.fetch_add(1);
			else if (r.changed || !r.patch_title.empty() || !r.patch_artist.empty()) a_changed.fetch_add(1);
			else a_kept.fetch_add(1);

			if (m_log) {
				std::string line = "    → " + (r.action.empty() ? std::string("?") : r.action);
				if (!r.verdict.empty() && r.verdict != r.action) line += "  [" + r.verdict + "]";
				if (!r.source.empty()) line += "  来源: " + r.source;
				if (!r.reason.empty()) line += "  " + r.reason;
				if (!r.patch_title.empty() || !r.patch_artist.empty()) line += "  补标签";
				m_log->log_line(index_prefix + line);
			}

			tag_patch p = m_dry_run ? tag_patch() : make_patch(r);
			bool has_patch = p.has_lyrics || p.has_title || p.has_artist || p.remove_original;

			m_results[i] = std::move(r);

			if (has_patch) {
				std::lock_guard<std::mutex> lock(write_guard);
				int code = write_patch(item, p);
				if (code == (int)metadb_io::update_info_success)
					m_results[i].written = true;
				else if (wstatus.load() == (int)metadb_io::update_info_success)
					wstatus.store(code);
			}
		};

		auto worker = [&]() {
			for (;;) {
				if (aborter.is_aborting()) break;
				size_t i = next.fetch_add(1);
				if (i >= total) break;
				try {
					process_index(i, write_mutex, write_status, kept, changed,
						no_candidate, skipped, failed);
				} catch (exception_aborted const&) {
					m_aborter->abort();
					break;
				} catch (...) {
					m_aborter->abort();
					break;
				}
				done.fetch_add(1);
				if (m_log) {
					m_log->set_progress((int)done.load(), (int)total,
						changed.load(), no_candidate.load(), skipped.load(), failed.load());
				}
			}
		};

		int workers = m_concurrency;
		if ((size_t)workers > total) workers = (int)total;
		if (workers < 1) workers = 1;

		std::vector<std::jthread> threads;
		threads.reserve(workers);
		for (int t = 0; t < workers; t++) threads.emplace_back(worker);

		while (done.load() < total && !aborter.is_aborting()) {
			if (tp_aborter.is_aborting()) m_aborter->abort();
			status.set_progress(done.load(), total);
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}

		threads.clear();
		m_write_status = write_status.load();
		status.set_progress(done.load(), total);

		if (m_log) {
			m_log->set_progress((int)done.load(), (int)total,
				changed.load(), no_candidate.load(), skipped.load(), failed.load());
		}
	}

	static int write_patch(const metadb_handle_ptr& item, const tag_patch& patch) {
		auto done = std::make_shared<pfc::event>(false);
		auto code = std::make_shared<int>((int)metadb_io::update_info_errors);

		fb2k::inMainThread([item, patch, done, code]() {
			try {
				std::map<std::string, tag_patch> changes;
				changes[item->get_path()] = patch;
				file_info_filter::ptr filter = new service_impl_t<tag_update_filter>(std::move(changes));
				metadb_handle_list handles;
				handles.add_item(item);
				metadb_io_v2::get()->update_info_async(handles, filter, core_api::get_main_window(),
					metadb_io_v2::op_flag_silent,
					fb2k::makeCompletionNotify([done, code](unsigned s) {
						*code = (int)s;
						done->set_state(true);
					}));
			} catch (...) {
				*code = (int)metadb_io::update_info_errors;
				done->set_state(true);
			}
		});

		done->wait();
		return *code;
	}

	void on_done(ctx_t, bool aborted) override {
		finalize(m_results, aborted, m_write_status, m_mode, m_log.get());
	}

private:
	metadb_handle_list m_items;
	bool m_dry_run = false;
	std::vector<std::string> m_skip_dirs;
	filename_template m_tpl;
	batch_mode m_mode = batch_mode::lyrics;
	std::shared_ptr<batch_log> m_log;
	std::shared_ptr<abort_callback_impl> m_aborter;
	int m_concurrency = 5;
	std::vector<item_result> m_results;
	int m_write_status = (int)metadb_io::update_info_success;

	static void write_report(const std::string& text, const char* name) {
		try {
			pfc::string8 full;
			if (!profile_file_path(name, full)) full = name;

			file::ptr f;
			filesystem::g_open_write_new(f, full, fb2k::noAbort);
			static const char bom[] = { (char)0xEF, (char)0xBB, (char)0xBF };
			f->write(bom, 3, fb2k::noAbort);
			f->write(text.data(), text.size(), fb2k::noAbort);
		} catch (...) {
		}
	}

	static void finalize(const std::vector<item_result>& results, bool aborted, int write_status,
		batch_mode mode, batch_log* log) {
		int kept = 0, changed = 0, no_candidate = 0, failed = 0, skipped = 0, tags_filled = 0;
		int deleted = 0, no_backup = 0;
		bool dry_run = false;
		const bool tags_mode = mode == batch_mode::tags;
		const bool cleanup_mode = mode == batch_mode::cleanup;
		std::string report;
		report += tags_mode ? "根据文件名推断标签报告\n"
			: cleanup_mode ? "歌词备份清理报告\n" : "内嵌歌词整理报告\n";
		report += "时间: " + now_string() + "\n";

		int processed = 0;
		for (auto& r : results) {
			if (!r.processed) continue;
			processed++;
			if (r.dry_run) dry_run = true;
			if (!r.patch_title.empty() || !r.patch_artist.empty()) tags_filled++;
			if (r.failed) failed++;
			else if (r.action == "删除备份") deleted++;
			else if (r.action == "无备份") no_backup++;
			else if (r.action == "无候选") no_candidate++;
			else if (r.action == "保持") kept++;
			else if (r.action == "跳过") skipped++;
			else if (r.changed) changed++;
		}

		std::string mode_text;
		if (aborted) mode_text = dry_run ? "已中止（未写入）" : "已中止（已处理部分已写入）";
		else if (dry_run) mode_text = "仅检测（未写入）";
		else mode_text = tags_mode ? "补全标签" : cleanup_mode ? "删除备份" : "写入";
		report += "模式: " + mode_text + "\n";
		if (dry_run)
			report += "说明: 本次为 dry-run，所有“补标签/修改”均未写入文件。\n";
		else if (!aborted && write_status >= 0)
			report += std::string("写入状态: ") + (write_status == (int)metadb_io::update_info_success
				? "成功" : ("失败（" + std::to_string(write_status) + "）")) + "\n";
		if (cleanup_mode) {
			report += "曲目: " + std::to_string(processed) +
				"  " + (aborted ? "拟删除: " : "已删除: ") + std::to_string(deleted) +
				"  无备份: " + std::to_string(no_backup) +
				"  跳过: " + std::to_string(skipped) +
				"  失败: " + std::to_string(failed) + "\n\n";
		} else {
			report += "曲目: " + std::to_string(processed) +
				"  保持: " + std::to_string(kept) +
				"  " + ((aborted || dry_run) ? "拟补标签: " : "补标签: ") + std::to_string(tags_filled);
			if (!tags_mode) {
				report += std::string("  ") + ((aborted || dry_run) ? "拟修改: " : "修改: ") + std::to_string(changed) +
					"  无候选: " + std::to_string(no_candidate);
			}
			report += "  跳过: " + std::to_string(skipped) +
				"  失败: " + std::to_string(failed) + "\n\n";
		}

		for (size_t i = 0; i < results.size(); i++) {
			const auto& r = results[i];
			if (!r.processed) continue;
			report += "[" + std::to_string(i + 1) + "] " + r.action + "  " + r.title;
			if (!r.artist.empty()) report += " - " + r.artist;
			report += "\n";
			report += "    文件: " + r.path + "\n";
			report += "    判定: " + r.verdict;
			if (!cleanup_mode) report += " (" + std::to_string(r.score) + ")";
			if (!r.source.empty()) report += "  来源: " + r.source;
			report += "\n";
			if (!r.reason.empty()) report += "    说明: " + r.reason + "\n";
			if (!r.patch_title.empty() || !r.patch_artist.empty()) {
				report += r.written ? "    已补标签:" : "    拟补标签:";
				if (!r.patch_title.empty()) report += " TITLE=" + r.patch_title;
				if (!r.patch_artist.empty()) report += " ARTIST=" + r.patch_artist;
				report += "\n";
			}
		}

		const char* report_name = tags_mode ? kTagsReportFileName
			: cleanup_mode ? kCleanupReportFileName : kReportFileName;
		write_report(report, report_name);
		console::print(report.c_str());

		pfc::string8 summary;
		if (tags_mode) {
			summary << (aborted ? "标签补全已中止：" : "标签补全完成：") << "共 " << processed << " 首，"
				<< ((aborted) ? "拟补标签 " : "已补标签 ") << tags_filled << " 首，"
				<< "保持 " << kept << " 首，跳过 " << skipped << " 首，"
				<< "失败 " << failed << " 首。\n\n报告已保存到配置目录下的 " << report_name;
		} else if (cleanup_mode) {
			summary << (aborted ? "歌词备份清理已中止：" : "歌词备份清理完成：") << "共 " << processed << " 首，"
				<< (aborted ? "拟删除 " : "已删除 ") << deleted << " 首，"
				<< "无备份 " << no_backup << " 首，跳过 " << skipped << " 首，"
				<< "失败 " << failed << " 首。\n\n报告已保存到配置目录下的 " << report_name;
		} else {
			summary << (aborted ? "歌词整理已中止：" : "歌词整理完成：") << "共 " << processed << " 首，"
				<< ((aborted || dry_run) ? "拟修改 " : "已修改 ") << changed << " 首，"
				<< ((aborted || dry_run) ? "拟补标签 " : "已补标签 ") << tags_filled << " 首，"
				<< "保持 " << kept << " 首，无候选 " << no_candidate << " 首，"
				<< "失败 " << failed << " 首。\n\n报告已保存到配置目录下的 " << report_name;
		}

		if (log) {
			log->log_line("");
			log->log_line(summary.c_str());
			log->set_progress(processed, (int)results.size(), cleanup_mode ? deleted : changed,
				cleanup_mode ? no_backup : no_candidate, skipped, failed);
			log->finish(aborted);
		}

		if (!log)
			popup_message::g_show(summary, tags_mode ? "标签补全"
				: cleanup_mode ? "删除备份的歌词" : "内嵌歌词");
	}
};

std::shared_ptr<batch_log> g_active_log;

void start_batch(const metadb_handle_list& items, bool dry_run, batch_mode mode) {
	const char* popup_title = mode == batch_mode::cleanup ? "删除备份的歌词" : "内嵌歌词";
	if (items.get_size() == 0) {
		popup_message::g_show("当前播放列表没有可处理的曲目。", popup_title);
		return;
	}
	if (g_active_log && !g_active_log->is_finished()) {
		popup_message::g_show("已有整理任务正在运行，请等待其完成。", popup_title);
		return;
	}

	const char* title = mode == batch_mode::tags ? "根据文件名推断标签"
		: mode == batch_mode::cleanup ? "删除备份的歌词"
		: (dry_run ? "检测内嵌歌词" : "整理内嵌歌词");

	auto aborter = std::make_shared<abort_callback_impl>();
	auto log = mode == batch_mode::cleanup
		? create_log_window(title, aborter, "删除", "无备份")
		: create_log_window(title, aborter);
	if (!log) {
		popup_message::g_show("无法创建日志窗口。", popup_title);
		return;
	}
	g_active_log = log;

	app_config cfg = load_config();
	const run_options opts = cfg.options;
	set_http_host_limit(opts.per_host);

	filename_template tpl = parse_filename_template(cfg.filename_template);
	service_ptr_t<batch_task> task = new service_impl_t<batch_task>(
		items, dry_run, std::move(cfg.skip_dirs), std::move(tpl), mode, log, aborter, opts);
	try {
		threaded_process::get()->run_modeless(task,
			threaded_process::flag_silent, core_api::get_main_window(), title);
	} catch (std::exception const& e) {
		log->log_line(std::string("启动失败：") + e.what());
		log->finish(true);
	}
}

// ---------------------------------------------------------------- 菜单

const GUID g_menu_group = { 0x8f6b1c2a, 0x4b7e, 0x4d21, { 0x9a, 0x3c, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
const GUID g_cmd_apply = { 0x8f6b1c2a, 0x4b7e, 0x4d21, { 0x9a, 0x3c, 0x11, 0x22, 0x33, 0x44, 0x55, 0x67 } };
const GUID g_cmd_check = { 0x8f6b1c2a, 0x4b7e, 0x4d21, { 0x9a, 0x3c, 0x11, 0x22, 0x33, 0x44, 0x55, 0x68 } };
const GUID g_cmd_tags = { 0x8f6b1c2a, 0x4b7e, 0x4d21, { 0x9a, 0x3c, 0x11, 0x22, 0x33, 0x44, 0x55, 0x6b } };
const GUID g_cmd_cleanup = { 0x8f6b1c2a, 0x4b7e, 0x4d21, { 0x9a, 0x3c, 0x11, 0x22, 0x33, 0x44, 0x55, 0x6d } };
const GUID g_cmd_config = { 0x8f6b1c2a, 0x4b7e, 0x4d21, { 0x9a, 0x3c, 0x11, 0x22, 0x33, 0x44, 0x55, 0x6c } };

class lyrics_menu_commands : public mainmenu_commands {
public:
	t_uint32 get_command_count() override { return 5; }

	GUID get_command(t_uint32 index) override {
		if (index == 0) return g_cmd_apply;
		if (index == 1) return g_cmd_check;
		if (index == 2) return g_cmd_tags;
		if (index == 3) return g_cmd_cleanup;
		return g_cmd_config;
	}

	void get_name(t_uint32 index, pfc::string_base& out) override {
		if (index == 0) out = "扫描并整理内嵌歌词";
		else if (index == 1) out = "仅检测内嵌歌词（不写入）";
		else if (index == 2) out = "根据文件名推断标签";
		else if (index == 3) out = "删除备份的歌词";
		else out = "编辑配置";
	}

	bool get_description(t_uint32 index, pfc::string_base& out) override {
		if (index == 0) out = "扫描选中曲目（未选中时为整个播放列表），按规范校验并联网补全内嵌歌词。";
		else if (index == 1) out = "只检测不写入，结束后在控制台和报告中查看判定结果。";
		else if (index == 2) out = "仅按文件名模板补全 TITLE/ARTIST，不检测或搜索歌词。";
		else if (index == 3) out = "删除选中曲目（未选中时为整个播放列表）的 ORIGINAL_LYRICS 备份 Tag，不影响 LYRICS；操作不可撤销。";
		else out = "打开 foo_lyrics_embed_config.txt，配置并发、忽略目录与文件名模板。";
		return true;
	}

	GUID get_parent() override { return g_menu_group; }

	void execute(t_uint32 index, ctx_t) override {
		if (index == 4) {
			open_config_file(kConfigFileName, kConfigTemplate, "创建配置文件失败");
			return;
		}
		auto pm = playlist_manager::get();
		if (pm->get_active_playlist() == SIZE_MAX) {
			popup_message::g_show("当前没有活动的播放列表。", "内嵌歌词");
			return;
		}
		metadb_handle_list items;
		pm->activeplaylist_get_selected_items(items);
		const bool from_selection = items.get_size() > 0;
		if (!from_selection)
			pm->activeplaylist_get_all_items(items);

		if (index == 3) {
			if (items.get_size() == 0) {
				start_batch(items, false, batch_mode::cleanup);
				return;
			}
			pfc::string8 msg;
			msg << "将从" << (from_selection ? "选中的 " : "整个播放列表的 ") << items.get_size()
				<< " 首曲目中删除 ORIGINAL_LYRICS 备份 Tag，此操作不可撤销。\n\n"
				<< "LYRICS 歌词本身不会被修改。\n\n是否继续？";
			if (fb2k::messageBox(core_api::get_main_window(), msg,
				"删除备份的歌词", MB_YESNO | MB_ICONQUESTION) != IDYES)
				return;
			start_batch(items, false, batch_mode::cleanup);
			return;
		}

		start_batch(items, index == 1,
			index == 2 ? batch_mode::tags : batch_mode::lyrics);
	}
};

mainmenu_group_popup_factory g_menu_group_factory(g_menu_group, mainmenu_groups::library,
	mainmenu_commands::sort_priority_dontcare, "内嵌歌词");
mainmenu_commands_factory_t<lyrics_menu_commands> g_menu_commands_factory;

} // namespace
