#include "tui_shell.h"
#include "util.h"
#include "status_bar.h"
#include "ime_controller.h"
#include <iostream>
#include <string>
#include <unistd.h>
#include <pty.h>
#include <utmp.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cstdlib>
#include <errno.h>
#include <chrono>

/**
 * tui_shell.cpp
 *
 * 这是 lite-tty-ime 的主程序入口与“终端包装器”实现：
 * - 通过 forkpty 启动一个子 shell，并把它的输入输出接到当前终端；
 * - 程序自身使用备用屏幕（alternate screen buffer）在底部绘制状态栏；
 * - 在事件循环中：
 *   - 读取用户键盘输入，并在中文模式下拦截/解析为拼音缓冲与候选；
 *   - 把未拦截的输入透传给子 shell；
 *   - 读取子 shell 输出，过滤会破坏界面的控制序列，并重绘状态栏。
 *
 * 重要约束：
 * - 本文件里“状态栏绘制”使用 ANSI 控制序列，必须保证不会污染主终端输出区域；
 * - 键盘输入既可能是单字节普通键，也可能是多字节转义序列（例如 Ctrl+Left/Delete 等），
 *   所以需要在按字节遍历时额外检测并“消费掉”多字节序列。
 */
int master_fd; 
struct termios orig_termios; 

/**
 * master_fd: forkpty 创建的 PTY master，用于与子 shell 双向通信：
 * - 写 master_fd：等同于把内容输入到子 shell；
 * - 读 master_fd：获取子 shell 输出。
 *
 * orig_termios: 主进程启动前的终端属性快照，用于退出时恢复。
 */

static int g_rows = 0;
static int g_cols = 0;
static bool g_use_alternate_screen = true;
static bool g_child_fullscreen = false;
static volatile sig_atomic_t g_winch_pending = 0;
static std::chrono::steady_clock::time_point g_fullscreen_hint_until = std::chrono::steady_clock::time_point{};
static std::chrono::steady_clock::time_point g_last_status_redraw = std::chrono::steady_clock::time_point{};

void update_status_bar(int rows, int cols);

static bool should_use_alternate_screen() {
    const char* no_altscr = getenv("LITE_TTY_IME_NO_ALTSCR");
    if (no_altscr && *no_altscr) return false;

    const char* term = getenv("TERM");
    if (!term) return true;
    std::string t(term);
    if (t == "linux" || t == "vt100" || t == "dumb") return false;
    return true;
}

static void sync_child_pty_size(bool fullscreen) {
    if (g_rows <= 0 || g_cols <= 0) return;
    struct winsize ws;
    ws.ws_row = (unsigned short)(fullscreen ? g_rows : (g_rows > 1 ? (g_rows - 1) : 1));
    ws.ws_col = (unsigned short)g_cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(master_fd, TIOCSWINSZ, &ws);
}

static bool effective_fullscreen_mode() {
    if (g_child_fullscreen) return true;
    auto now = std::chrono::steady_clock::now();
    return now < g_fullscreen_hint_until;
}

static bool output_suggests_fullscreen(const std::string& output) {
    if (output.find("\033[2J") != std::string::npos) return true;
    if (output.find("\033[H") != std::string::npos) return true;
    if (output.find("\033[r") != std::string::npos) return true;
    if (output.find("\033[?25l") != std::string::npos) return true;
    if (output.find("\033[?25h") != std::string::npos) return true;
    if (output.find("\033[?1049h") != std::string::npos) return true;
    if (output.find("\033[?1049l") != std::string::npos) return true;

    if (output.find("\033Ptmux;") != std::string::npos) {
        if (output.find("\033\033[2J") != std::string::npos) return true;
        if (output.find("\033\033[H") != std::string::npos) return true;
        if (output.find("\033\033[r") != std::string::npos) return true;
        if (output.find("\033\033[?25l") != std::string::npos) return true;
        if (output.find("\033\033[?25h") != std::string::npos) return true;
        if (output.find("\033\033[?1049h") != std::string::npos) return true;
        if (output.find("\033\033[?1049l") != std::string::npos) return true;
    }

    return false;
}

static void update_child_fullscreen_state_from_output(const std::string& output,
                                                      bool& did_enter_fullscreen,
                                                      bool& did_exit_fullscreen) {
    did_enter_fullscreen = false;
    did_exit_fullscreen = false;
    auto has = [&](const char* seq) { return output.find(seq) != std::string::npos; };

    bool saw_enter = has("\033[?1049h") || has("\033[?47h") || has("\033[?1047h") || has("\033\033[?1049h") ||
                     has("\033\033[?47h") || has("\033\033[?1047h");
    bool saw_exit = has("\033[?1049l") || has("\033[?47l") || has("\033[?1047l") || has("\033\033[?1049l") ||
                    has("\033\033[?47l") || has("\033\033[?1047l");

    if (saw_enter) {
        if (!g_child_fullscreen) did_enter_fullscreen = true;
        g_child_fullscreen = true;
    }
    if (saw_exit) {
        if (g_child_fullscreen) did_exit_fullscreen = true;
        g_child_fullscreen = false;
    }
}

/**
 * 退出前恢复终端显示与 termios 原始配置。
 * 用于正常退出、异常退出和信号退出的统一收尾。
 */
void reset_terminal() {
    write_log("Resetting terminal and exiting Alternate Screen Buffer", INFO);
    int rows = 0, cols = 0;
    get_terminal_size(rows, cols);
    if (rows <= 0) rows = g_rows;
    if (cols <= 0) cols = g_cols;

    const char* pre = "\033[0m\033[r\033[?25h";
    write(STDOUT_FILENO, pre, 12);
    if (g_use_alternate_screen) {
        const char* exit_alt = "\033[?1049l";
        write(STDOUT_FILENO, exit_alt, 8);
        write(STDOUT_FILENO, pre, 12);
    }

    if (rows > 0) {
        std::string tail = "\033[" + std::to_string(rows) + ";1H\033[2K";
        write(STDOUT_FILENO, tail.c_str(), tail.size());
    }
    
    // 恢复原始终端属性（termios），确保键盘输入和回显恢复正常
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

/**
 * 进程退出信号处理器。
 * 收到 SIGINT/SIGTERM 时恢复终端并结束进程。
 *
 * @param sig 信号编号。
 */
void handle_exit_signal(int sig) {
    write_log("Received exit signal: " + std::to_string(sig), WARN);
    reset_terminal();
    exit(0);
}

/**
 * 终端尺寸变化信号处理器（SIGWINCH）。
 * 负责同步子 PTY 尺寸、修正滚动区域并重绘状态栏。
 *
 * @param sig 信号编号。
 */
void handle_winch(int sig) {
    (void)sig;
    g_winch_pending = 1;
}

static void process_winch_if_needed() {
    if (!g_winch_pending) return;
    g_winch_pending = 0;

    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) return;

    int prev_rows = g_rows;
    int rows = ws.ws_row;
    int cols = ws.ws_col;
    g_rows = rows;
    g_cols = cols;
    write_log("Terminal resized to: " + std::to_string(rows) + "x" + std::to_string(cols), INFO);

    bool fullscreen = effective_fullscreen_mode();
    sync_child_pty_size(fullscreen);

    if (prev_rows > 0 && rows > prev_rows) {
        std::string clear_old = "\0337\033[" + std::to_string(prev_rows) + ";1H\033[2K\0338";
        write(STDOUT_FILENO, clear_old.c_str(), clear_old.size());
    }

    if (fullscreen) {
        const char* reset_scroll = "\033[r";
        write(STDOUT_FILENO, reset_scroll, 3);
    } else {
        std::string limit_scroll = "\033[1;" + std::to_string(rows - 1) + "r";
        write(STDOUT_FILENO, limit_scroll.c_str(), limit_scroll.size());
        update_status_bar(rows, cols);
    }
}

/**
 * 程序主入口。
 * 负责：
 * 1) 初始化终端与状态栏；
 * 2) 启动子 shell；
 * 3) 事件循环中处理键盘输入、输入法状态与子进程输出。
 */
int main() {
    // 禁止嵌套运行检测
    if (getenv("LITE_TTY_IME_ACTIVE")) {
        std::cerr << "Error: lite-tty-ime is already running in this terminal." << std::endl;
        return 1;
    }

    init_logger();

    int rows, cols;
    get_terminal_size(rows, cols);
    g_rows = rows;
    g_cols = cols;

    struct winsize ws;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("tcgetattr");
        return 1;
    }
    
    atexit(reset_terminal);
    signal(SIGINT, handle_exit_signal);
    signal(SIGTERM, handle_exit_signal);
    signal(SIGWINCH, handle_winch);

    struct termios raw = orig_termios;
    cfmakeraw(&raw);
    raw.c_iflag |= (IXON | IXOFF);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    g_use_alternate_screen = should_use_alternate_screen();

    /**
     * 进入备用屏幕模式
     * 1. \033[H\033[2J: 先清理主屏幕，防止子程序意外退出备用屏幕时看到旧内容
     * 2. \033[?1049h: 开启备用屏幕缓冲区
     * 3. \033[H: 将光标移动到备用屏幕左上角
     */
    if (g_use_alternate_screen) {
        std::cout << "\033[H\033[2J" << "\033[?1049h" << "\033[H" << std::flush;
    } else {
        std::cout << "\033[H\033[2J" << std::flush;
    }

    // 初始化界面：设置滚动区域
    std::cout << "\033[1;" << (rows - 1) << "r";
    
    // 初始化状态栏内容（调用抽离出的状态栏模块）
    init_status_bar();

    ime_controller_init();

    struct winsize sub_ws = ws;
    sub_ws.ws_row -= 1;
    
    // 设置环境变量，标记当前已经运行了一个实例，禁止嵌套
    setenv("LITE_TTY_IME_ACTIVE", "1", 1);
    
    write_log("Starting lite-tty-ime...", INFO);
    
    pid_t pid = forkpty(&master_fd, NULL, NULL, &sub_ws);

    if (pid < 0) {
        perror("forkpty");
        return 1;
    } else if (pid == 0) {
        const char* shell = getenv("SHELL");
        if (!shell) shell = "/bin/bash";
        execl(shell, shell, (char*)NULL);
        _exit(1);
    }

    char buf[8192];
    fd_set readfds;

    while (true) {
        /**
         * select 监听两路输入：
         * - STDIN_FILENO：用户键盘输入
         * - master_fd：子 shell 的输出
         *
         * 这里使用阻塞 select（timeout=NULL），以最小 CPU 占用运行。
         */
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(master_fd, &readfds);

        if (select(master_fd + 1, &readfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        process_winch_if_needed();

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;

            /**
             * 按键拦截与逻辑处理
             *
             * 注意：buf 可能包含多个字节：
             * - 普通按键是单字节；
             * - 功能键/组合键可能是多字节转义序列（例如 Delete/Ctrl+方向键等）。
             * 本循环以“逐字节”方式遍历，但遇到多字节序列时会通过 match_* 检测并一次性消费掉多个字节。
             */
            std::string key_name = get_key_name(buf, n);
            write_log(key_name + " press", DEBUG);

            for (ssize_t i = 0; i < n; ++i) {
                if ((unsigned char)buf[i] == 29) {
                    write_log("Emergency Exit Triggered via Ctrl+]", WARN);
                    reset_terminal();
                    exit(0);
                }
            }
            // 处理按键输入
            ime_controller_handle_input_bytes(buf, n, master_fd);
        }

        if (FD_ISSET(master_fd, &readfds)) {
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n <= 0) break;

            /**
             * 过滤子程序的输出，拦截破坏界面的指令
             * 1. 拦截 \033[?1049l (退出备用屏幕): 
             *    像 mc 这样的程序退出时会尝试回到主屏幕，这会导致我们的状态栏出现在错误的位置。
             *    我们通过删除这个指令，强制子程序留在我们的备用屏幕内。
             * 2. 拦截 \033[r (重置滚动区域):
             *    子程序可能会尝试恢复全屏滚动，我们会将其拦截并修正为受限滚动区域。
             */
            std::string output(buf, n);
            bool did_enter_fullscreen = false;
            bool did_exit_fullscreen = false;
            update_child_fullscreen_state_from_output(output, did_enter_fullscreen, did_exit_fullscreen);
            if (output_suggests_fullscreen(output)) {
                g_fullscreen_hint_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
            }
            bool modified = false;

            if (did_enter_fullscreen) {
                sync_child_pty_size(true);
                const char* reset_scroll = "\033[r";
                write(STDOUT_FILENO, reset_scroll, 3);
            }

            // 拦截退出备用屏幕
            size_t pos;
            while ((pos = output.find("\033[?1049l")) != std::string::npos) {
                output.erase(pos, 8);
                modified = true;
            }

            // 拦截重置滚动区域
            if (!effective_fullscreen_mode()) {
                std::string reset_scroll = "\033[r";
                while ((pos = output.find(reset_scroll)) != std::string::npos) {
                    char fix_scroll[32];
                    sprintf(fix_scroll, "\033[1;%dr", g_rows - 1);
                    output.replace(pos, reset_scroll.length(), fix_scroll);
                    modified = true;
                }
            }

            if (modified) {
                if (write(STDOUT_FILENO, output.data(), output.size()) != (ssize_t)output.size()) break;
            } else {
                if (write(STDOUT_FILENO, buf, n) != (ssize_t)n) break;
            }

            if (did_exit_fullscreen) {
                sync_child_pty_size(false);
                std::string fix_scroll = "\033[1;" + std::to_string(g_rows - 1) + "r";
                std::string fix_cursor = "\033[" + std::to_string(g_rows - 1) + ";1H";
                write(STDOUT_FILENO, fix_scroll.c_str(), fix_scroll.size());
                write(STDOUT_FILENO, fix_cursor.c_str(), fix_cursor.size());
            }

            /**
             * 响应式更新状态栏
             * 只要上方终端有输出更新，就立即重绘一次状态栏。
             */
            if (!effective_fullscreen_mode()) {
                auto now = std::chrono::steady_clock::now();
                if (g_last_status_redraw == std::chrono::steady_clock::time_point{} ||
                    now - g_last_status_redraw > std::chrono::milliseconds(50)) {
                    update_status_bar(g_rows, g_cols);
                    g_last_status_redraw = now;
                }
            }
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
// 底边栏逻辑（状态栏渲染与对外接口）
// -----------------------------------------------------------------------------

static std::string current_status_text = "";
static int current_bg_color = 40;
static int current_fg_color = 37;

void set_status_bar_color(int ansi_color) {
    write_log("Set status bar background color to: " + std::to_string(ansi_color), INFO);
    current_bg_color = ansi_color;
    int r, c;
    get_terminal_size(r, c);
    update_status_bar(r, c);
}

void set_status_bar_fg_color(int ansi_color) {
    write_log("Set status bar foreground color to: " + std::to_string(ansi_color), INFO);
    current_fg_color = ansi_color;
    int r, c;
    get_terminal_size(r, c);
    update_status_bar(r, c);
}

void write_status_bar(const std::string& text) {
    write_log("Write to status bar: " + text, DEBUG);
    current_status_text = text;
    int r, c;
    get_terminal_size(r, c);
    update_status_bar(r, c);
}

void write_status_bar(const char* text) {
    if (text) write_status_bar(std::string(text));
}

std::string get_status_bar_text() {
    write_log("Get status bar text (current: " + current_status_text + ")", DEBUG);
    return current_status_text;
}

void update_status_bar(int rows, int cols) {
    (void)cols;
    std::string out;
    out.reserve(current_status_text.size() + 64);
    out += "\0337";
    out += "\033[";
    out += std::to_string(rows);
    out += ";1H";
    out += "\033[";
    out += std::to_string(current_fg_color);
    out += ";";
    out += std::to_string(current_bg_color);
    out += "m\033[2K";
    out += current_status_text;
    out += "\033[m\0338";
    write(STDOUT_FILENO, out.c_str(), out.size());
}
