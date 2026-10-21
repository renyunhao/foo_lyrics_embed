# foo_lyrics_embed

一个 foobar2000 歌词嵌入插件：在播放歌曲时，自动将同目录下与歌曲同名的歌词文件（`.lrc` / `.txt`）以 Tag 形式嵌入到音频文件中，并在嵌入成功后删除歌词文件。

## 核心功能

- 监听 foobar2000 的"新曲目开始播放"事件（`play_callback_static`）
- 在音频文件同目录下查找同名歌词文件（优先 `.lrc`，其次 `.txt`）
- 读取歌词内容并自动检测文件编码（UTF-8 / UTF-16 / GBK / Big5 / Shift-JIS / EUC-KR / KOI8-R / Windows-125x 等），统一转换为 **带 BOM 的 UTF-8** 后写入 `LYRICS` Tag，避免乱码
- 若曲目已存在 `LYRICS` Tag 则跳过，不覆盖已有歌词
- Tag 写入成功后自动删除源歌词文件；删除失败时在控制台输出错误信息
- 处理过程与结果均输出到 foobar2000 控制台（View → Console），便于排查

## 目录结构

```
Foobar2000Lyrics/
├── README.md                  # 本文档
├── foo_lyrics_embed/          # 插件工程（独立于 SDK）
│   ├── foo_lyrics_embed.sln   # Visual Studio 解决方案
│   ├── foo_lyrics_embed.vcxproj
│   ├── main.cpp               # 组件版本声明与入口
│   ├── lyrics_embed.cpp       # 核心逻辑：播放回调 / 歌词查找 / 编码检测转换 / Tag 写入
│   ├── stdafx.h / PCH.cpp     # 预编译头
│   ├── compact_enc_det/       # 编码检测库（Google CompactEncDet，源码直接编译）
│   └── x64/Release/           # 构建输出（foo_lyrics_embed.dll）
└── SDK-2026-10-01/            # foobar2000 官方 SDK（2026-10-01 版）
    ├── pfc/                   # PFC 基础库
    └── foobar2000/
        ├── SDK/               # SDK 核心（service / metadb / play_callback 等）
        ├── foobar2000_component_client/  # 组件运行时入口
        ├── shared/            # shared-x64.lib 等链接库
        ├── helpers/           # SDK 辅助库（本工程未引用）
        └── foo_sample/        # 官方示例组件
```

## 环境依赖与构建指南

### 环境要求

| 依赖 | 要求 |
| --- | --- |
| 操作系统 | Windows 10/11 x64 |
| IDE / 工具链 | Visual Studio 2022（MSVC v143；2026-10-01 版 SDK 强制要求 C++20，仅支持 VS2022 及以上，不再支持更旧版本） |
| SDK | foobar2000 SDK 2026-10-01 |
| 运行时 | foobar2000 v2.0+ x64（`FOOBAR2000_TARGET_VERSION=81`） |

> **重要：foobar2000 SDK 并不包含在本仓库中。** 上表中的 `SDK-2026-10-01/` 目录需由使用者自行从 foobar2000 官方网站（https://www.foobar2000.org/ ）下载对应版本的 SDK 并解压到仓库根目录，否则项目无法完成编译。SDK 下载后需保持其原始目录结构（根目录下含 `foobar2000/`、`pfc/` 等子目录）。

### 命令行构建

```powershell
msbuild foo_lyrics_embed\foo_lyrics_embed.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

或使用完整路径（未配置 VS 开发者环境时）：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "foo_lyrics_embed\foo_lyrics_embed.sln" `
  /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

构建产物：`foo_lyrics_embed\x64\Release\foo_lyrics_embed.dll`

### Visual Studio 构建

1. 用 VS2022 打开 `foo_lyrics_embed\foo_lyrics_embed.sln`
2. 选择 `Release | x64` 配置，生成解决方案

> 首次构建会同时编译 SDK 依赖的静态库（pfc、foobar2000_SDK、foobar2000_component_client），耗时略长；后续增量构建很快。

## 使用说明

### 安装

1. 构建（或获取）`foo_lyrics_embed.dll`
2. 将其复制到 foobar2000 安装目录下的 `components\` 文件夹（或通过 foobar2000 的 `File → Preferences → Components → Install` 安装）
3. 重启 foobar2000，在 `Preferences → Components` 中应能看到 "Lyrics Embed"

### 使用方法

1. 将歌词文件放到与音频文件相同的目录，且与音频文件同名（扩展名为 `.lrc` 或 `.txt`），例如：
   ```
   D:\Music\
   ├── Track 01.flac
   └── Track 01.lrc
   ```
2. 在 foobar2000 中播放该曲目
3. 插件自动将歌词（转换为带 BOM 的 UTF-8）写入音频文件的 `LYRICS` Tag，成功后删除歌词文件
4. 打开 `View → Console` 可查看处理日志：找到的歌词文件路径、检测到的编码、写入与删除结果

### 注意事项

- 音频文件须可写（只读文件、受保护格式或正在被独占占用时写入会失败）
- 已有 `LYRICS` Tag 的曲目会被跳过，不会重复嵌入
- 歌词文件大小限制为 1MB 以内
- 写入的 Tag 类型由 foobar2000 按文件格式自动选择（如 ID3v2 / Vorbis Comment / APEv2 等）
- 本版本基于 `FOOBAR2000_TARGET_VERSION=81` 构建，需 foobar2000 v2.0 及以上版本加载
