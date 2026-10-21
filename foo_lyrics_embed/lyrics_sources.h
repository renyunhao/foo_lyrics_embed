#pragma once

#include <functional>

#include "lyrics_common.h"

namespace lyrics {

using log_fn = std::function<void(const std::string&)>;

struct fetch_result {
	bool found = false;
	std::string content;
	std::string source;
	std::string detail; // 未找到时的原因
};

// 依次查询网易云、QQ音乐、LRCLIB，返回第一个通过匹配与规范校验的歌词。
// 候选必须满足 H1-H6，且能取得作词与作曲（H7）。
// log 可选，用于输出检索过程。
fetch_result find_lyrics(const track_meta& meta, abort_callback& aborter, const log_fn& log = {});

// 只查询作词/作曲，用于给已有歌词补头部。
bool lookup_credits(const track_meta& meta, std::string& zuoci, std::string& zuoqu,
	abort_callback& aborter, const log_fn& log = {});

// 同一站点的最大并发 HTTP 请求数（1-16），默认 3。
void set_http_host_limit(int limit);

} // namespace lyrics
