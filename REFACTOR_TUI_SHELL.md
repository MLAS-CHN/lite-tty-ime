## 范围

本文只覆盖 [tui_shell.cpp](file:///home/mlas_chn/WorkSpaces/pinyin/src/tui_shell.cpp)（当前版本）在被舍弃/替换前的“对外接口 + 项目内依赖”梳理，用于后续把终端包装器替换为 tmux-like 虚拟终端结构时，避免遗漏稳定接口与关键耦合点。

## 对外接口（本文件提供给其他模块调用）

这些函数在 [tui_shell.h](file:///home/mlas_chn/WorkSpaces/pinyin/src/tui_shell.h) 中声明，可视为稳定接口：

- `void set_status_bar_color(int ansi_color)`
  - 参数：`ansi_color`（ANSI 背景色编号，例如 40 黑底、44 蓝底、47 白底）
  - 作用：更新状态栏背景色缓存，并触发一次状态栏重绘（用当前终端尺寸）。
- `void set_status_bar_fg_color(int ansi_color)`
  - 参数：`ansi_color`（ANSI 前景色编号，例如 37 灰、30 黑、31 红）
  - 作用：更新状态栏前景色缓存，并触发一次状态栏重绘（用当前终端尺寸）。
- `void write_status_bar(const std::string& text)`
  - 参数：`text`（状态栏要显示的完整字符串）
  - 作用：写入状态栏文本缓存，并触发一次状态栏重绘（用当前终端尺寸）。
- `void write_status_bar(const char* text)`
  - 参数：`text`（C 字符串，可为 `nullptr`）
  - 作用：便捷重载；当 `text != nullptr` 时转发到 `write_status_bar(std::string)`.
- `std::string get_status_bar_text()`
  - 返回：当前缓存的状态栏字符串
  - 作用：读取状态栏文本缓存，便于调试或外部模块查询。

补充：`int main()` 为可执行入口，不作为“库接口”被其他模块调用。

## 依赖方（哪些项目内模块在调用上述对外接口）

### status_bar.h / status_bar.cpp

- [status_bar.cpp](file:///home/mlas_chn/WorkSpaces/pinyin/src/status_bar.cpp)
  - 依赖接口：`set_status_bar_color`、`set_status_bar_fg_color`、`write_status_bar`
  - 含义：状态栏模块本身只拼接文本与做宽度截断，真正“往终端画一行”的落点目前在 `tui_shell.h` 这组接口上。

## 外部模块依赖（本文件调用了哪些项目内函数）

### util.h / util.cpp

- `void init_logger()`
  - 调用点：`main()`
  - 作用：初始化日志系统（文件名/级别等由 util 实现决定）。
- `void write_log(const std::string& msg, LogLevel level)`
  - 调用点：状态栏接口、`reset_terminal()`、信号处理、尺寸变化处理、按键日志等
  - 参数：`msg` 日志文本；`level` 日志等级（例如 INFO/WARN/DEBUG）
  - 作用：记录运行时关键事件，辅助定位终端/PTY/状态栏问题。
- `void get_terminal_size(int& rows, int& cols)`
  - 调用点：`main()` 初始化、`reset_terminal()`、状态栏重绘、颜色切换时即时重绘
  - 参数：输出参数 `rows/cols`
  - 作用：获取真实终端尺寸；用于：子 PTY 尺寸同步、滚动区域设置、状态栏定位。
- `std::string get_key_name(const char* buf, ssize_t n)`
  - 调用点：`main()` 的 STDIN 输入分支
  - 参数：`buf` 原始按键字节序列；`n` 字节数
  - 作用：把按键字节序列转换成可读字符串用于日志。

### status_bar.h / status_bar.cpp

- `void init_status_bar()`
  - 调用点：`main()` 初始化阶段
  - 作用：初始化状态栏内容管理模块（例如默认模式标签、候选展示文本等由模块内部维护）。

### ime_controller.h / ime_controller.cpp

- `void ime_controller_init()`
  - 调用点：`main()` 初始化阶段
  - 作用：初始化输入法控制模块状态（模式、缓冲、候选等）。
- `void ime_controller_handle_input_bytes(const char* buf, ssize_t n, int master_fd)`
  - 调用点：`main()` 在收到 STDIN 输入后
  - 参数：`buf/n` 用户按键原始字节；`master_fd` 子 PTY master
  - 作用：输入法模块统一处理输入；内部会决定拦截/转发哪些字节到 `master_fd`。

## 重构关注点（为 tmux-like 虚拟终端做约束）

- 子程序 PTY 尺寸：当前逻辑是 `rows-1`，并在 `SIGWINCH` 时同步子 PTY 尺寸；tmux-like 结构需要保留同样的尺寸语义。
- 输出路径：当前是“读 master_fd 输出 -> 过滤部分控制序列 -> 直接 write 到真实 STDOUT”；tmux-like 需要改为“更新虚拟屏幕状态 -> 渲染到真实终端”。
- 退出收尾：`reset_terminal()` 恢复滚动区域、显示光标、退出备用屏幕、恢复 termios；新结构必须保留等价收尾路径（正常退出/信号退出）。
