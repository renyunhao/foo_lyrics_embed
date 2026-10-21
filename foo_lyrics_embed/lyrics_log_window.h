#pragma once

#include <memory>
#include <string>

#include <SDK/abort_callback_impl.h>

namespace lyrics {

// 批量任务的实时日志窗口接口，方法均可从工作线程调用。
class batch_log {
public:
	virtual ~batch_log() {}
	virtual void log_line(const std::string& line) = 0;
	virtual void set_progress(int done, int total, int changed, int no_candidate, int skipped, int failed) = 0;
	virtual void finish(bool aborted) = 0;
	virtual bool is_finished() const = 0;
};

// 必须在主线程调用。aborter 用于“取消”按钮中断网络请求。
std::shared_ptr<batch_log> create_log_window(const char* title,
	std::shared_ptr<abort_callback_impl> aborter);

} // namespace lyrics
