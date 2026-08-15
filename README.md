# lite-tty-ime

轻量级终端中文拼音输入法。运行在 Linux 纯 TTY / 终端环境中，自带虚拟终端（tmux-like）架构：forkpty 启动子 shell，解析其 ANSI 输出到虚拟屏幕，再渲染到真实终端，底部提供状态栏显示输入法状态与候选词。

## 功能特性

- **拼音输入**：完整音节切分（激进/保守多套方案）、字词匹配、候选分页、连续输入
- **词库学习**：选中候选自动计数 + 时间戳，最近使用的词排序靠前；新词自动写入用户词库
- **虚拟终端**：ANSI 解析 + 虚拟屏幕 + 全量渲染，兼容 vim/btop/tmux/opencode 等 TUI 程序
- **混合输入**：大小写混合（`Hao`）、数字/符号混排（`2hao`）输入自动拼接字面量前缀
- **延迟写盘**：词库变更先去抖合并，空闲时统一落盘，百万词库不卡输入
- **mmap 词库缓存**：百万行词库匹配毫秒级响应
- **常用中文标点**：中文模式下标点键自动转换为全角符号（`，、。；：？！（）《》…` 等）

## 编译

```bash
make
```

环境要求：Linux、g++（C++17）、`-lutil`。产物为 `lite-tty-ime`。

## 使用

```bash
./lite-tty-ime
# 或指定命令：
./lite-tty-ime -- htop
```

### 按键

| 按键 | 功能 |
|---|---|
| Ctrl+0 | 切换中/英文模式 |
| a-z | 拼音输入 |
| A-Z | 大写字母直接进入缓冲（混合输入） |
| 空格 | 选择第 1 个候选 |
| 1-9, 0 | 选择候选（0 = 第 10 个） |
| `=` / `-` | 下一页 / 上一页 |
| Delete | 切换删除模式（删除词库词条） |
| Ctrl+← / Ctrl+→ | 移动拼音缓冲光标 |
| Backspace | 删除拼音字符 |
| Enter | 直接提交拼音原文 |
| Ctrl+] | 紧急退出 |

### 词库

| 文件 | 说明 |
|---|---|
| `pinyin_map.txt` | 拼音 → 汉字映射 |
| `char_freq.txt` | 单字频率（`拼音 字 计数`） |
| `user_dict.txt` | 用户词库（`拼音,拼音 文本 时间戳 计数`） |

默认搜索顺序：`/usr/share/lite-tty-ime/data` → `~/.lite-tty-ime/data` → `./data`。

## 配置

```bash
./lite-tty-ime --config ~/.lite-tty-ime/config
```

配置文件（`key=value`）：

```
data_dir=/path/to/data
status_fg_ansi=38
status_bg_ansi=39
```

| 选项 | 说明 |
|---|---|
| `--data-dir DIR` | 指定词库目录 |
| `--status-fg N` / `--status-bg N` | 状态栏 ANSI 颜色（0-255） |
| `--no-config` | 不加载配置文件 |
| `-- command...` | 启动指定程序而非默认 shell |

环境变量：

| 变量 | 说明 |
|---|---|
| `LITE_TTY_IME_ACTIVE` | 防嵌套运行标记 |
| `LITE_TTY_IME_NO_ALTSCR` | 不使用备用屏幕（TERM=linux/vt100 时自动关闭） |
| `LITE_TTY_IME_DEBUG_OUT=<path>` | 记录子程序原始输出（排查渲染问题） |

## 项目结构

```
├── Makefile
├── data/                  # 词库
└── src/
    ├── vterm/             # 虚拟终端（核心）
    │   ├── virtual_shell.cpp  # 主入口：forkpty + 事件循环
    │   ├── ansi_parser.h      # ANSI 解析状态机
    │   ├── screen.h           # 虚拟屏幕
    │   ├── rendering.h        # 全量渲染 + 状态栏
    │   └── ...
    ├── ime_controller.cpp # 输入法状态机（模式/缓冲/候选/删除模式）
    ├── pinyin_split.cpp   # 拼音拆分（激进/保守多套方案）
    ├── pinyin_matcher.cpp # 字词匹配（区间扫描 + 快照复用）
    ├── pinyin_composition.cpp # 候选聚合 + 去重 + 消耗字母数排序
    ├── pinyin_file_io.cpp # 词库读写（mmap 缓存 / 延迟写盘 / 原子写）
    └── status_bar.cpp     # 状态栏内容管理
```

## 致谢

- 由 Opencode（AI）辅助编程完成
- 词库格式与引擎设计参考 ClassicABC（Windows 拼音输入法）项目
