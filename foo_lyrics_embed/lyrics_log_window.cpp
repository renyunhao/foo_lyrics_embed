#include "stdafx.h"

#include "lyrics_log_window.h"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <vector>

#include <commctrl.h>

namespace lyrics {

namespace {

const wchar_t* kWindowClass = L"foo_lyrics_embed_log";
const UINT WM_APP_FINISHED = WM_APP + 0x51;

enum {
	IDC_STATUS = 1001,
	IDC_PROGRESS,
	IDC_LOG,
	IDC_ACTION
};

class log_window_impl : public batch_log {
public:
	log_window_impl(const std::string& title, std::shared_ptr<abort_callback_impl> aborter,
		std::wstring changed_label, std::wstring no_candidate_label)
		: m_title(title), m_aborter(std::move(aborter)),
		  m_changed_label(std::move(changed_label)),
		  m_no_candidate_label(std::move(no_candidate_label)) {}

	~log_window_impl() override {
		HWND hwnd = m_hwnd.load();
		if (hwnd) DestroyWindow(hwnd);
	}

	HWND create() {
		static std::once_flag once;
		std::call_once(once, []() {
			WNDCLASSEXW wc = {};
			wc.cbSize = sizeof(wc);
			wc.style = CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = &log_window_impl::wnd_proc;
			wc.hInstance = core_api::get_my_instance();
			wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
			wc.lpszClassName = kWindowClass;
			RegisterClassExW(&wc);
		});

		pfc::stringcvt::string_wide_from_utf8 wide_title(m_title.c_str());
		HWND hwnd = CreateWindowExW(0, kWindowClass, wide_title,
			WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
			CW_USEDEFAULT, CW_USEDEFAULT, 820, 560,
			core_api::get_main_window(), nullptr, core_api::get_my_instance(), this);
		if (!hwnd) return nullptr;
		m_hwnd.store(hwnd);
		ShowWindow(hwnd, SW_SHOWNORMAL);
		UpdateWindow(hwnd);
		return hwnd;
	}

	void log_line(const std::string& line) override {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_pending.size() < 200000) m_pending.push_back(line);
	}

	void set_progress(int done, int total, int changed, int no_candidate, int skipped, int failed) override {
		m_done.store(done);
		m_total.store(total);
		m_changed.store(changed);
		m_no_candidate.store(no_candidate);
		m_skipped.store(skipped);
		m_failed.store(failed);
	}

	void finish(bool aborted) override {
		m_aborted.store(aborted);
		m_finished.store(true);
		HWND hwnd = m_hwnd.load();
		if (hwnd) PostMessageW(hwnd, WM_APP_FINISHED, 0, 0);
	}

	bool is_finished() const override { return m_finished.load(); }

private:
	std::string m_title;
	std::shared_ptr<abort_callback_impl> m_aborter;
	std::wstring m_changed_label;
	std::wstring m_no_candidate_label;

	std::atomic<bool> m_cancel{false};
	std::atomic<bool> m_finished{false};
	std::atomic<bool> m_aborted{false};
	std::atomic<HWND> m_hwnd{nullptr};

	std::atomic<int> m_done{0};
	std::atomic<int> m_total{0};
	std::atomic<int> m_changed{0};
	std::atomic<int> m_no_candidate{0};
	std::atomic<int> m_skipped{0};
	std::atomic<int> m_failed{0};

	std::mutex m_mutex;
	std::vector<std::string> m_pending;

	HWND m_status = nullptr;
	HWND m_progress = nullptr;
	HWND m_edit = nullptr;
	HWND m_button = nullptr;

	static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
		log_window_impl* self = nullptr;
		if (msg == WM_NCCREATE) {
			self = (log_window_impl*)((CREATESTRUCTW*)lp)->lpCreateParams;
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
		} else {
			self = (log_window_impl*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
		}
		if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
		return self->handle(hwnd, msg, wp, lp);
	}

	LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
		switch (msg) {
		case WM_CREATE: {
			m_hwnd.store(hwnd);

			m_status = CreateWindowExW(0, L"STATIC", L"",
				WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
				0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, core_api::get_my_instance(), nullptr);
			m_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
				WS_CHILD | WS_VISIBLE,
				0, 0, 0, 0, hwnd, (HMENU)IDC_PROGRESS, core_api::get_my_instance(), nullptr);
			SendMessageW(m_progress, PBM_SETRANGE32, 0, 1);
			m_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
				WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
				ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
				0, 0, 0, 0, hwnd, (HMENU)IDC_LOG, core_api::get_my_instance(), nullptr);
			// 多行 EDIT 默认只允许约 32K 字符，必须解除限制，否则日志会在中途停止追加
			SendMessageW(m_edit, EM_SETLIMITTEXT, 0, 0);
			m_button = CreateWindowExW(0, L"BUTTON", L"取消",
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
				0, 0, 0, 0, hwnd, (HMENU)IDC_ACTION, core_api::get_my_instance(), nullptr);

			HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
			SendMessageW(m_status, WM_SETFONT, (WPARAM)font, TRUE);
			SendMessageW(m_edit, WM_SETFONT, (WPARAM)font, TRUE);
			SendMessageW(m_button, WM_SETFONT, (WPARAM)font, TRUE);
			SendMessageW(m_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));

			SetTimer(hwnd, 1, 200, nullptr);
			layout(hwnd, 820, 560);
			return 0;
		}
		case WM_SIZE:
			layout(hwnd, LOWORD(lp), HIWORD(lp));
			return 0;
		case WM_TIMER:
			drain_log();
			update_status();
			return 0;
		case WM_COMMAND:
			if (LOWORD(wp) == IDC_ACTION) {
				on_action();
				return 0;
			}
			break;
		case WM_CLOSE:
			on_action();
			return 0;
		case WM_APP_FINISHED:
			drain_log();
			update_status();
			KillTimer(hwnd, 1);
			set_finished_ui();
			return 0;
		case WM_GETMINMAXINFO: {
			MINMAXINFO* mmi = (MINMAXINFO*)lp;
			mmi->ptMinTrackSize.x = 560;
			mmi->ptMinTrackSize.y = 320;
			return 0;
		}
		case WM_NCDESTROY:
			KillTimer(hwnd, 1);
			m_hwnd.store(nullptr);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	void on_action() {
		if (m_finished.load()) {
			HWND hwnd = m_hwnd.load();
			if (hwnd) DestroyWindow(hwnd);
			return;
		}
		if (!m_cancel.exchange(true)) {
			SetWindowTextW(m_button, L"正在停止...");
			EnableWindow(m_button, FALSE);
			if (m_aborter) m_aborter->abort();
			log_line("正在取消...");
		}
	}

	void set_finished_ui() {
		HWND hwnd = m_hwnd.load();
		if (!hwnd) return;
		std::wstring title = (const wchar_t*)pfc::stringcvt::string_wide_from_utf8(m_title.c_str());
		title += m_aborted.load() ? L"（已中止）" : L"（已完成）";
		SetWindowTextW(hwnd, title.c_str());
		SetWindowTextW(m_button, L"关闭");
		EnableWindow(m_button, TRUE);
	}

	void layout(HWND hwnd, int w, int h) {
		const int margin = 12;
		const int label_h = 22;
		const int prog_h = 16;
		const int button_w = 100;
		const int button_h = 28;

		int top = margin;
		MoveWindow(m_status, margin, top, (std::max)(0, w - margin * 2), label_h, TRUE);
		top += label_h;
		MoveWindow(m_progress, margin, top, (std::max)(0, w - margin * 2), prog_h, TRUE);
		top += prog_h + 8;

		int edit_h = h - top - margin - button_h - 8;
		if (edit_h < 0) edit_h = 0;
		MoveWindow(m_edit, margin, top, (std::max)(0, w - margin * 2), edit_h, TRUE);
		MoveWindow(m_button, w - margin - button_w, h - margin - button_h, button_w, button_h, TRUE);
	}

	void drain_log() {
		std::vector<std::string> batch;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_pending.empty()) return;
			batch.swap(m_pending);
		}

		std::string text;
		for (auto& line : batch) {
			text += line;
			text += "\r\n";
		}
		pfc::stringcvt::string_wide_from_utf8 wide(text.c_str());
		SendMessageW(m_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
		SendMessageW(m_edit, EM_REPLACESEL, FALSE, (LPARAM)wide.get_ptr());
		SendMessageW(m_edit, EM_SCROLLCARET, 0, 0);
	}

	void update_status() {
		int done = m_done.load();
		int total = m_total.load();
		if (total > 0) {
			SendMessageW(m_progress, PBM_SETRANGE32, 0, total);
			SendMessageW(m_progress, PBM_SETPOS, done, 0);
		}

		wchar_t buf[256];
		swprintf_s(buf, L"已完成 %d / %d      %s %d      %s %d      跳过 %d      失败 %d",
			done, total, m_changed_label.c_str(), m_changed.load(),
			m_no_candidate_label.c_str(), m_no_candidate.load(), m_skipped.load(), m_failed.load());
		SetWindowTextW(m_status, buf);
	}
};

} // namespace

std::shared_ptr<batch_log> create_log_window(const char* title, std::shared_ptr<abort_callback_impl> aborter,
	const char* changed_label, const char* no_candidate_label) {
	pfc::stringcvt::string_wide_from_utf8 changed(changed_label ? changed_label : "修改");
	pfc::stringcvt::string_wide_from_utf8 no_candidate(no_candidate_label ? no_candidate_label : "无候选");
	auto w = std::make_shared<log_window_impl>(title ? title : "", std::move(aborter),
		std::wstring(changed.get_ptr()), std::wstring(no_candidate.get_ptr()));
	if (!w->create()) return nullptr;
	return w;
}

} // namespace lyrics
