#include "stdafx.h"
#include "compact_enc_det/compact_enc_det.h"

namespace {

static const char* g_lyricsExtensions[] = { ".lrc", ".txt", nullptr };
static const char* g_lyricsTagName = "LYRICS";

static UINT EncodingToCodePage(Encoding enc) {
    switch (enc) {
    case ISO_8859_1:      return 28591;
    case ISO_8859_2:      return 28592;
    case ISO_8859_3:      return 28593;
    case ISO_8859_4:      return 28594;
    case ISO_8859_5:      return 28595;
    case ISO_8859_6:      return 28596;
    case ISO_8859_7:      return 28597;
    case ISO_8859_8:      return 28598;
    case ISO_8859_9:      return 28599;
    case ISO_8859_10:     return 28600;
    case ISO_8859_11:     return 28601;
    case ISO_8859_13:     return 28603;
    case ISO_8859_15:     return 28605;
    case JAPANESE_EUC_JP: return 51932;
    case JAPANESE_SHIFT_JIS: return 932;
    case JAPANESE_JIS:    return 50220;
    case CHINESE_BIG5:    return 950;
    case CHINESE_GB:      return 936;
    case KOREAN_EUC_KR:   return 51949;
    case UTF8:            return 65001;
    case ASCII_7BIT:      return 28591;
    case RUSSIAN_KOI8_R:  return 20866;
    case RUSSIAN_CP1251:  return 1251;
    case MSFT_CP1252:     return 1252;
    case RUSSIAN_KOI8_RU: return 21866;
    case MSFT_CP1250:     return 1250;
    case MSFT_CP1254:     return 1254;
    case MSFT_CP1257:     return 1257;
    case MSFT_CP874:      return 874;
    case MSFT_CP1256:     return 1256;
    case MSFT_CP1255:     return 1255;
    case MSFT_CP1253:     return 1253;
    case RUSSIAN_CP866:   return 866;
    case GBK:             return 936;
    case GB18030:         return 54936;
    case BIG5_HKSCS:      return 950;
    case CHINESE_EUC_CN:  return 51936;
    default:              return 0;
    }
}

static const char g_utf8BOM[] = { (char)0xEF, (char)0xBB, (char)0xBF };

static bool ConvertToUTF8(const char* raw, int rawLen, UINT srcCodePage, pfc::string8& out) {
    pfc::string8 converted;
    if (srcCodePage == 65001) {
        converted.set_string(raw, rawLen);
    } else {
        int wideLen = MultiByteToWideChar(srcCodePage, 0, raw, rawLen, nullptr, 0);
        if (wideLen <= 0) return false;
        pfc::array_t<WCHAR> wideBuf;
        wideBuf.set_size(wideLen);
        MultiByteToWideChar(srcCodePage, 0, raw, rawLen, wideBuf.get_ptr(), wideLen);
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideBuf.get_ptr(), wideLen, nullptr, 0, nullptr, nullptr);
        if (utf8Len <= 0) return false;
        pfc::array_t<char> utf8Buf;
        utf8Buf.set_size(utf8Len);
        WideCharToMultiByte(CP_UTF8, 0, wideBuf.get_ptr(), wideLen, utf8Buf.get_ptr(), utf8Len, nullptr, nullptr);
        converted.set_string(utf8Buf.get_ptr(), utf8Len);
    }
    out.set_string(g_utf8BOM, 3);
    out += converted;
    return true;
}


class lyrics_embed_callback : public play_callback_static {
public:
    unsigned get_flags() override {
        return flag_on_playback_new_track;
    }

    void on_playback_starting(play_control::t_track_command p_command, bool p_paused) override {}
    void on_playback_new_track(metadb_handle_ptr p_track) override;
    void on_playback_stop(play_control::t_stop_reason p_reason) override {}
    void on_playback_seek(double p_time) override {}
    void on_playback_pause(bool p_state) override {}
    void on_playback_edited(metadb_handle_ptr p_track) override {}
    void on_playback_dynamic_info(const file_info& p_info) override {}
    void on_playback_dynamic_info_track(const file_info& p_info) override {}
    void on_playback_time(double p_time) override {}
    void on_volume_change(float p_new_val) override {}
};

static bool FindLyricsFile(const char* audioPath, pfc::string8& lyricsPath) {
    pfc::string8 basePath(audioPath);
    t_size dotPos = basePath.find_last('.');
    if (dotPos == pfc::infinite_size) return false;

    pfc::string8 noExtPath;
    noExtPath.set_string(basePath.get_ptr(), dotPos);

    for (t_size i = 0; g_lyricsExtensions[i] != nullptr; i++) {
        lyricsPath = noExtPath;
        lyricsPath += g_lyricsExtensions[i];
        try {
            abort_callback_dummy aborter;
            filesystem::g_exists(lyricsPath, aborter);
            return true;
        } catch (...) {
            continue;
        }
    }
    return false;
}

static bool ReadLyricsContent(const char* lyricsPath, pfc::string8& content) {
    try {
        abort_callback_dummy aborter;
        service_ptr_t<file> filePtr;
        filesystem::g_open(filePtr, lyricsPath, filesystem::open_mode_read, aborter);
        t_filesize size = filePtr->get_size(aborter);
        if (size == filesize_invalid || size == 0 || size > 1024 * 1024) return false;

        pfc::array_t<char> buffer;
        buffer.set_size((t_size)size + 1);
        filePtr->read(buffer.get_ptr(), (t_size)size, aborter);
        buffer[(t_size)size] = '\0';

        const char* rawData = buffer.get_ptr();
        int rawLen = (int)size;

        int dataStart = 0;
        if (rawLen >= 3 && (unsigned char)rawData[0] == 0xEF &&
            (unsigned char)rawData[1] == 0xBB &&
            (unsigned char)rawData[2] == 0xBF) {
            content.set_string(g_utf8BOM, 3);
            content += (rawData + 3);
            return true;
        }

        if (rawLen >= 2 && (unsigned char)rawData[0] == 0xFF &&
            (unsigned char)rawData[1] == 0xFE) {
            int wideLen = (rawLen - 2) / 2;
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, (const WCHAR*)(rawData + 2), wideLen, nullptr, 0, nullptr, nullptr);
            if (utf8Len > 0) {
                pfc::array_t<char> utf8Buf;
                utf8Buf.set_size(utf8Len);
                WideCharToMultiByte(CP_UTF8, 0, (const WCHAR*)(rawData + 2), wideLen, utf8Buf.get_ptr(), utf8Len, nullptr, nullptr);
                content.set_string(g_utf8BOM, 3);
                content += pfc::string8(utf8Buf.get_ptr(), utf8Len);
                return true;
            }
        }

        if (rawLen >= 2 && (unsigned char)rawData[0] == 0xFE &&
            (unsigned char)rawData[1] == 0xFF) {
            pfc::array_t<WCHAR> swapped;
            int wideLen = (rawLen - 2) / 2;
            swapped.set_size(wideLen);
            const unsigned char* src = (const unsigned char*)(rawData + 2);
            for (int i = 0; i < wideLen; i++) {
                swapped[i] = (WCHAR)((src[i * 2] << 8) | src[i * 2 + 1]);
            }
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, swapped.get_ptr(), wideLen, nullptr, 0, nullptr, nullptr);
            if (utf8Len > 0) {
                pfc::array_t<char> utf8Buf;
                utf8Buf.set_size(utf8Len);
                WideCharToMultiByte(CP_UTF8, 0, swapped.get_ptr(), wideLen, utf8Buf.get_ptr(), utf8Len, nullptr, nullptr);
                content.set_string(g_utf8BOM, 3);
                content += pfc::string8(utf8Buf.get_ptr(), utf8Len);
                return true;
            }
        }

        int bytesConsumed = 0;
        bool isReliable = false;
        Encoding detectedEnc = CompactEncDet::DetectEncoding(
            rawData, rawLen, nullptr, nullptr, nullptr,
            UNKNOWN_ENCODING, UNKNOWN_LANGUAGE,
            CompactEncDet::QUERY_CORPUS, true,
            &bytesConsumed, &isReliable);

        UINT codePage = EncodingToCodePage(detectedEnc);
        if (codePage == 0) codePage = 65001;

        if (!ConvertToUTF8(rawData, rawLen, codePage, content)) {
            content = rawData;
        }

        console::printf("Lyrics Embed: detected encoding %s (codepage %u, reliable=%d)",
            EncodingName(detectedEnc), codePage, isReliable);
        return true;
    } catch (...) {
        return false;
    }
}

class lyrics_file_info_filter : public file_info_filter {
public:
    lyrics_file_info_filter(const char* lyricsContent, const char* lyricsPath) : m_lyricsContent(lyricsContent), m_lyricsPath(lyricsPath) {}

    bool apply_filter(trackRef p_location, t_filestats p_stats, file_info& p_info) override {
        const char* existing = p_info.meta_get(g_lyricsTagName, 0);
        if (existing != nullptr && strlen(existing) > 0) {
            console::printf("Lyrics Embed: track already has LYRICS tag, skipping");
            return false;
        }
        p_info.meta_set(g_lyricsTagName, m_lyricsContent);
        return true;
    }

    const char* GetLyricsPath() const { return m_lyricsPath; }

private:
    pfc::string8 m_lyricsContent;
    pfc::string8 m_lyricsPath;
};

void lyrics_embed_callback::on_playback_new_track(metadb_handle_ptr p_track) {
    if (p_track.is_empty()) return;

    const char* audioPath = p_track->get_path();
    if (audioPath == nullptr || strlen(audioPath) == 0) return;

    pfc::string8 lyricsPath;
    if (!FindLyricsFile(audioPath, lyricsPath)) return;

    pfc::string8 lyricsContent;
    if (!ReadLyricsContent(lyricsPath, lyricsContent)) return;

    console::printf("Lyrics Embed: found lyrics file: %s", lyricsPath.c_str());

    metadb_handle_list handles;
    handles += p_track;

    service_ptr_t<lyrics_file_info_filter> filter = new service_impl_t<lyrics_file_info_filter>(lyricsContent, lyricsPath);
    pfc::string8 lyricsPathCopy(lyricsPath);
    static_api_ptr_t<metadb_io_v2>()->update_info_async(
        handles,
        filter,
        core_api::get_main_window(),
        metadb_io_v2::op_flag_delay_ui | metadb_io_v2::op_flag_silent,
        fb2k::makeCompletionNotify([lyricsPathCopy](unsigned status) {
            if (status == metadb_io::update_info_success) {
                try {
                    abort_callback_dummy aborter;
                    filesystem::g_remove(lyricsPathCopy, aborter);
                    console::printf("Lyrics Embed: deleted lyrics file: %s", lyricsPathCopy.c_str());
                } catch (std::exception const& e) {
                    console::printf("Lyrics Embed: failed to delete lyrics file: %s", e.what());
                }
            }
        })
    );
}

play_callback_static_factory_t<lyrics_embed_callback> g_lyrics_embed_callback_factory;

}