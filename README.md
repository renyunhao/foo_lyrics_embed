# foo_lyrics_embed

一个 foobar2000 歌词插件，包含两项功能：

1. **播放时自动嵌入**：在播放歌曲时，自动将同目录下与歌曲同名的歌词文件（`.lrc` / `.txt`）以 Tag 形式嵌入到音频文件中，并在嵌入成功后删除歌词文件。
2. **批量整理内嵌歌词**：扫描选中曲目（或整个播放列表）的 `LYRICS` Tag，按统一规范校验，缺失或不合规时联网搜索歌词并写回。

## 核心功能

### 播放时自动嵌入

- 监听 foobar2000 的"新曲目开始播放"事件（`play_callback_static`）
- 在音频文件同目录下查找同名歌词文件（优先 `.lrc`，其次 `.txt`）
- 读取歌词内容并自动检测文件编码（UTF-8 / UTF-16 / GBK / Big5 / Shift-JIS / EUC-KR / KOI8-R / Windows-125x 等），统一转换为 **无 BOM 的 UTF-8** 后写入 `LYRICS` Tag，避免乱码
- 若曲目已存在 `LYRICS` Tag 则跳过，不覆盖已有歌词
- Tag 写入成功后自动删除源歌词文件；删除失败时在控制台输出错误信息
- 处理过程与结果均输出到 foobar2000 控制台（View → Console），便于排查

### 批量整理内嵌歌词

菜单入口：`Library → 内嵌歌词`，包含五个命令：

| 命令 | 行为 |
| --- | --- |
| 扫描并整理内嵌歌词 | 按规范校验，必要时本地修复或联网搜索，并写回 `LYRICS` |
| 仅检测内嵌歌词（不写入） | 只校验并输出报告，不修改任何文件（dry-run） |
| 根据文件名推断标签 | 独立功能：只按模板补全 `TITLE` / `ARTIST`，不读取或修改歌词 |
| 删除备份的歌词 | 删除 `ORIGINAL_LYRICS` 备份 Tag，保留 `LYRICS`；执行前需确认 |
| 编辑配置 | 创建/打开统一的 `foo_lyrics_embed_config.txt`，内含运行参数、忽略目录与文件名模板 |

- 扫描范围：播放列表中被选中的曲目；未选中任何曲目时扫描整个当前播放列表
- 后台线程执行，使用插件自带的日志窗口：显示进度条、计数（修改 / 无候选 / 跳过 / 失败）、当前曲目和逐首处理详情，包括每个歌词源的搜索、候选读取、校验与采用结果
- 窗口内的“取消”会立即中止正在进行的 HTTP 请求；中途中止时已处理的曲目改动保留
- **并发搜索**：默认同时处理 5 首（`concurrency=5`，范围 1~16），`1` 表示串行；同一站点的并发请求默认上限 3（`per_host=3`），遇到 429/503 会自动退避重试一次
- **边扫描边写入**：每处理完一首且需要修改，就立即写回该曲目；写回串行执行，避免多个标签更新同时进行；中途中止时，已处理的曲目改动会保留
- 忽略目录：读取统一配置的 `[skip]` 段，每行一个目录，匹配不区分大小写，支持文件夹名（`Tune`）、路径片段（`Music\Tune`）或完整路径（`D:\Music\Tune`）；`#` 开头为注释。命中的曲目在报告中记为“跳过（忽略目录）”
- 写回前若已有 `LYRICS`，其原始内容会备份到 `ORIGINAL_LYRICS` Tag（仅在该 Tag 尚不存在时备份）
- 报告写入 `profile\foo_lyrics_embed_report.txt`（UTF-8 BOM），最终汇总同时输出到控制台和日志窗口

#### 删除备份的歌词

- 菜单：`Library → 内嵌歌词 → 删除备份的歌词`
- 删除选中曲目（未选中时为整个播放列表）的 `ORIGINAL_LYRICS` Tag，`LYRICS` 歌词本身不受影响
- 执行前弹出确认框；此操作不可撤销
- 无备份的曲目记为“无备份”，不会写入文件
- 不联网、不读取歌词正文，也不套用忽略目录
- 报告写入 `profile\foo_lyrics_embed_cleanup_report.txt`，汇总同时输出到控制台和日志窗口

#### 文件名推断与标签回填

- 当 `TITLE` 或 `ARTIST` 为空时，按模板解析文件名（不含扩展名）并**写回对应的标签**
- 模板位置：统一配置的 `[filename]` 段，只读取第一条非注释行，默认值为 `{artist} - {title}`
- 支持的占位符：`{artist}`（艺人）、`{title}`（标题，必须出现）；其余字符按字面匹配，例如 `[MV] {artist} - {title}`
- 需要用户保证文件名符合模板格式；如果模板匹配失败，则整个文件名作为 `TITLE`，不推断 `ARTIST`
- 只补空缺字段：已有 `TITLE` 或 `ARTIST` 不会被覆盖
- dry-run 模式只会在报告中列出“拟补标签”，不会实际写入

#### 独立功能：根据文件名推断标签

- 菜单：`Library → 内嵌歌词 → 根据文件名推断标签`
- 只读取标签并按模板补全 `TITLE` / `ARTIST`，不检测、不搜索歌词，也不修改 `LYRICS`
- 与整理功能共用忽略目录列表和文件名模板
- 文件名不符合模板时不会猜测：该曲目记为“跳过”，原因“文件名不符合模板”
- 写入后报告保存到 `profile\foo_lyrics_embed_tags_report.txt`，汇总同时输出到控制台和日志窗口

#### 统一配置文件

所有配置集中在 `profile\foo_lyrics_embed_config.txt`，菜单「编辑配置」可直接打开：

```ini
# foo_lyrics_embed 配置（UTF-8，修改后下次运行生效）

[options]
# 同时处理的曲目数（1-16），1 表示串行
concurrency=5
# 同一歌词站点的最大并发请求数（1-16）
per_host=3

[skip]
# 每行一个要跳过扫描的目录，匹配不区分大小写
# 支持文件夹名（Tune）、路径片段（Music\Tune）或完整路径（D:\Music\Tune）
Tune

[filename]
# 文件名推断模板（不含扩展名），必须包含 {title}，可选 {artist}
{artist} - {title}
```

| 段 | 内容 |
| --- | --- |
| `[options]` | `concurrency`：同时处理的曲目数（1~16，1 表示串行，默认 5）；`per_host`：同一歌词站点最大并发请求数（1~16，默认 3） |
| `[skip]` | 每行一个要跳过扫描的目录 |
| `[filename]` | 文件名推断模板（第一条非注释行） |

- 超出范围的数值会被钳制到边界；缺失的键使用默认值
- 遇到 HTTP 429/503 会自动等待约 0.8 秒后重试一次
- 文件不存在时使用默认值；打开「编辑配置」会生成带注释的完整模板
- 仅支持这一个配置文件，不再读取旧版的 `foo_lyrics_embed_options.txt` / `foo_lyrics_embed_skip.txt` / `foo_lyrics_embed_filename.txt`

#### 规范标准（v1）

一首"规范"的内嵌歌词须同时满足：

| 编号 | 规则 |
| --- | --- |
| H1 | 存在 `[mm:ss]` / `[mm:ss.xx]` 时间轴 |
| H2 | 时间轴覆盖率 ≥ 80%（带时间行 / 非空行） |
| H3 | 有效歌词行 ≥ 8 行（不含词曲信息行） |
| H4 | 时间轴倒退 ≤ 2 处 |
| H5 | 最大时间不超过歌曲时长 + 10 秒，且不短于时长的 50% |
| H6 | 内容干净：无 HTML 实体、无 SRT 序号行、无广告/下载站行 |
| H7 | 含"作词"与"作曲"信息头 |

补充规则：

- 纯音乐 / 无人声曲目直接视为合格并跳过
- 缺少 H1–H5 任一项 → 整篇替换
- 仅缺 H7 → 只补头部，保留原有歌词正文
- 仅 H6 不合格（或为 SRT 格式）→ 本地清洗修复
- 搜索不到合格候选 → 保留原歌词，报告中标记"无候选"

#### 歌词来源与优先级

按以下顺序检索，命中合格候选后采用；若候选缺头部则尝试补全：

| 来源 | 优先级 | 说明 |
| --- | --- | --- |
| 网易云音乐 | 6 | 含翻译/时间轴，通常无词曲信息 |
| QQ 音乐 | 4 | LRC 为 Base64，常带"词：/曲："信息 |
| LRCLIB | 2 | 无鉴权，通常无词曲信息 |

候选需通过标题相似度、艺人匹配、时长差（≤ 5 秒）校验后才会采用。

## 目录结构

```
Foobar2000Lyrics/
├── README.md                  # 本文档
├── foo_lyrics_embed/          # 插件工程（独立于 SDK）
│   ├── foo_lyrics_embed.sln   # Visual Studio 解决方案
│   ├── foo_lyrics_embed.vcxproj
│   ├── main.cpp               # 组件版本声明与入口
│   ├── lyrics_embed.cpp       # 播放时自动嵌入：播放回调 / 歌词查找 / 编码检测转换 / Tag 写入
│   ├── lyrics_common.h        # 公共类型与文本工具（匹配、编码、相似度）
│   ├── lyrics_quality.h/.cpp  # 规范判定（H1–H7）、本地修复、补头部
│   ├── lyrics_sources.h/.cpp  # 在线歌词源（网易云 / QQ / LRCLIB）、HTTP、JSON、Base64
│   ├── lyrics_batch.cpp       # 批量任务：菜单命令、扫描、写回、备份、报告
│   ├── lyrics_log_window.h/.cpp # 插件自带的实时日志窗口（进度、逐首详情、取消）
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

### 使用方法一：播放时自动嵌入

1. 将歌词文件放到与音频文件相同的目录，且与音频文件同名（扩展名为 `.lrc` 或 `.txt`），例如：
   ```
   D:\Music\
   ├── Track 01.flac
   └── Track 01.lrc
   ```
2. 在 foobar2000 中播放该曲目
3. 插件自动将歌词（转换为带 BOM 的 UTF-8）写入音频文件的 `LYRICS` Tag，成功后删除歌词文件
4. 打开 `View → Console` 可查看处理日志：找到的歌词文件路径、检测到的编码、写入与删除结果

### 使用方法二：批量整理内嵌歌词

1. 在播放列表中选中要处理的曲目（不选则处理整个当前播放列表）
2. 如需调整并发、跳过纯音乐等目录或文件名模板，先执行 `Library → 内嵌歌词 → 编辑配置`，在对应段中修改
3. 先执行 `Library → 内嵌歌词 → 仅检测内嵌歌词（不写入）`，确认判定结果符合预期
4. 再执行 `Library → 内嵌歌词 → 扫描并整理内嵌歌词` 写入
5. 查看 `View → Console` 或 `profile\foo_lyrics_embed_report.txt` 获取逐曲目明细
6. 需要回滚时，可从 `ORIGINAL_LYRICS` Tag 取回被覆盖前的歌词
7. 确认不再需要回滚后，可执行 `Library → 内嵌歌词 → 删除备份的歌词` 清理备份 Tag

### 注意事项

- 音频文件须可写（只读文件、受保护格式或正在被独占占用时写入会失败）
- 播放时自动嵌入对已有 `LYRICS` Tag 的曲目会跳过；批量整理则会按规范校验并可能覆盖
- 歌词文件大小限制为 1MB 以内
- 写入的 Tag 类型由 foobar2000 按文件格式自动选择（如 ID3v2 / Vorbis Comment / APEv2 等）
- 批量整理依赖网络访问网易云音乐 / QQ 音乐 / LRCLIB，网络不可用时仅进行本地判定
- 两个写入路径统一为**无 BOM 的 UTF-8**：标签容器本身已声明编码，BOM 会被部分 LRC 解析器当作首行内容，可能破坏第一行时间轴
- 本版本基于 `FOOBAR2000_TARGET_VERSION=81` 构建，需 foobar2000 v2.0 及以上版本加载
