#pragma once

#include "lyrics_common.h"

namespace lyrics {

// 判定结果，对应“规范歌词 v1”中的五类修复动作。
enum class verdict {
	ok,             // 合格，保持不动
	missing_header, // 仅有作词/作曲缺失，只需补头部
	needs_repair,   // 可用本地规则修复（SRT 转换、去 HTML/广告行）
	needs_replace,  // 必须整篇替换
	no_candidate    // 需要替换但找不到合格候选
};

struct quality_result {
	verdict v = verdict::needs_replace;
	int score = 0;
	std::vector<std::string> reasons;

	bool instrumental = false;
	bool has_timeline = false;
	bool has_zuoci = false;
	bool has_zuoqu = false;
	std::string zuoci;
	std::string zuoqu;

	int timed_lines = 0;   // 带时间且正文非空的行
	int lyric_lines = 0;   // 带时间、非空、非 credit 的行
	int total_lines = 0;   // 正文非空的行
	int64_t max_time_ms = 0;
	bool dirty = false;
};

// 按“规范歌词 v1”的 H1-H7 判定。duration 为文件时长（秒），<=0 时跳过 H5。
quality_result evaluate(const std::string& content, double duration_sec);

bool looks_like_srt(const std::string& content);

// 本地修复：SRT -> LRC、解码 HTML 实体、去掉广告行与网页残留。返回是否有改动。
bool repair_local(const std::string& in, std::string& out, std::vector<std::string>& notes);

// 在 LRC 头部（元数据行之后、第一行歌词之前）插入作词/作曲。
std::string inject_credits(const std::string& lrc, const std::string& zuoci, const std::string& zuoqu);

// 从已有 LRC 中提取作词/作曲。
bool extract_credits(const std::string& content, std::string& zuoci, std::string& zuoqu);

const char* verdict_name(verdict v);

} // namespace lyrics
