#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <locale.h>
#include <wchar.h>
#include "vterm/screen.h"
#include "vterm/rendering.h"
#include "ime_controller.h"
#include "pinyin_file_io.h"
#include "status_bar.h"
#include "util.h"

#define LITE_TTY_IME_VERSION "0.1.0"

static int g_master_fd = -1;
static termios g_orig_termios{};
static volatile sig_atomic_t g_winch_pending = 0;
static int g_rows = 0;
static int g_cols = 0;
static bool g_app_cursor_mode = false;
static bool g_app_keypad_mode = false;
static int g_debug_out_fd = -1;
 
static std::string trim_ascii_ws(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

static std::string expand_home(const std::string& path) {
    if (path.rfind("~/", 0) != 0) return path;
    const char* home = getenv("HOME");
    if (!home || !*home) return path;
    return std::string(home) + path.substr(1);
}

static bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = (int)v;
    return true;
}

struct AppConfig {
    std::string data_dir;
    int status_fg_ansi = 38;
    int status_bg_ansi = 39;
};

static void load_config_file(const std::string& config_path, AppConfig& cfg) {
    std::ifstream f(config_path);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        line = trim_ascii_ws(line);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_ascii_ws(line.substr(0, eq));
        std::string val = trim_ascii_ws(line.substr(eq + 1));
        if (key == "data_dir") {
            cfg.data_dir = expand_home(val);
            continue;
        }
        if (key == "status_fg" || key == "status_fg_ansi") {
            int v = 0;
            if (parse_int(val, v)) cfg.status_fg_ansi = v;
            continue;
        }
        if (key == "status_bg" || key == "status_bg_ansi") {
            int v = 0;
            if (parse_int(val, v)) cfg.status_bg_ansi = v;
            continue;
        }
    }
}

static void print_help(const char* argv0) {
    std::cout
        << "lite-tty-ime " << LITE_TTY_IME_VERSION << "\n"
        << "\n"
        << "Usage: " << argv0 << " [options] [command...]\n"
        << "\n"
        << "Options:\n"
        << "  -h, --help            Show this help and exit\n"
        << "  -v, --version         Show version and exit\n"
        << "  --config PATH         Config file path (default: ~/.lite-tty-ime/config)\n"
        << "  --no-config           Do not load config file\n"
        << "  --data-dir DIR        Dictionary directory (must contain: pinyin_map.txt, user_dict.txt, char_freq.txt)\n"
        << "  --status-fg N         Status bar foreground ANSI color (0-255)\n"
        << "  --status-bg N         Status bar background ANSI color (0-255)\n"
        << "\n"
        << "Default dictionary search order (when --data-dir is not set):\n"
        << "  1) /usr/share/lite-tty-ime/data\n"
        << "  2) ~/.lite-tty-ime/data\n"
        << "  3) ./data\n"
        << "\n"
        << "Config file format (key=value):\n"
        << "  data_dir=/path/to/data\n"
        << "  status_fg_ansi=38\n"
        << "  status_bg_ansi=39\n";
}

// 将用户键盘输入转换为子程序更期望的序列（当前只处理方向键在 normal/app cursor 模式间的差异）。
static std::string translate_input_for_child(const char* in, ssize_t n) {
    std::string out;
    if (n <= 0) return out;
    out.reserve((size_t)n + 8);

    ssize_t i = 0;
    while (i < n) {
        unsigned char b0 = (unsigned char)in[i];
        if (i + 2 < n && b0 == 0x1B) {
            unsigned char b1 = (unsigned char)in[i + 1];
            unsigned char b2 = (unsigned char)in[i + 2];
            bool is_arrow = (b2 == 'A' || b2 == 'B' || b2 == 'C' || b2 == 'D');
            bool is_cursor_seq = (b1 == '[' || b1 == 'O');
            if (is_arrow && is_cursor_seq) {
                out.push_back('\x1B');
                out.push_back(g_app_cursor_mode ? 'O' : '[');
                out.push_back((char)b2);
                i += 3;
                continue;
            }
        }
        out.push_back((char)b0);
        i++;
    }
    return out;
}
#include "vterm/ansi_parser.h"
 
// 恢复真实终端状态（退出 raw 模式、退出备用屏、恢复滚动区域与光标显示）。
static void reset_terminal() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    const char* show = "\033[0m\033[r\033[?25h\033[?1049l";
    write(STDOUT_FILENO, show, (size_t)strlen(show));
    wait_pending_persists();  // 退出前确保词库落盘（atexit / 正常退出路径）。
}

// 信号退出处理：恢复终端后立即退出进程。
static void handle_exit_signal(int) {
    reset_terminal();
    _exit(0);
}
 
// SIGWINCH 回调：标记窗口大小变更，延迟到主循环里处理。
static void handle_winch(int) {
    g_winch_pending = 1;
}
 
// 如窗口大小变更，则调整虚拟屏幕尺寸并同步子程序 PTY 的窗口大小。
static void apply_winch_if_needed(VirtualScreen& screen) {
    if (!g_winch_pending) return;
    g_winch_pending = 0;
    int rows = 0, cols = 0;
    get_terminal_size(rows, cols);
    g_rows = rows;
    g_cols = cols;
    int child_rows = rows > 1 ? (rows - 1) : 1;
    screen.resize(child_rows, cols);
    if (g_master_fd >= 0) {
        winsize ws{};
        ws.ws_row = (unsigned short)child_rows;
        ws.ws_col = (unsigned short)cols;
        ioctl(g_master_fd, TIOCSWINSZ, &ws);
    }
}
 
// tmux_shell 入口：forkpty 启动子 shell，把输出喂给 ANSI 解析器，再将虚拟屏幕渲染到真实终端。
int main(int argc, char** argv) {
    setlocale(LC_CTYPE, "");
    std::string config_path;
    {
        const char* home = getenv("HOME");
        config_path = (home && *home) ? (std::string(home) + "/.lite-tty-ime/config") : ".lite-tty-ime.config";
    }
    bool no_config = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_help(argv[0]);
            return 0;
        }
        if (a == "-v" || a == "--version") {
            std::cout << "lite-tty-ime " << LITE_TTY_IME_VERSION << std::endl;
            return 0;
        }
        if (a == "--no-config") {
            no_config = true;
            continue;
        }
        if (a == "--config" && i + 1 < argc) {
            config_path = expand_home(argv[i + 1]);
            ++i;
            continue;
        }
    }

    AppConfig cfg;
    if (!no_config) load_config_file(config_path, cfg);

    std::vector<char*> child_argv;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") {
            for (int j = i + 1; j < argc; ++j) child_argv.push_back(argv[j]);
            break;
        }
        if (a.size() > 0 && a[0] == '-') {
            if ((a == "--config" || a == "--data-dir" || a == "--status-fg" || a == "--status-bg") && i + 1 < argc) {
                ++i;
                continue;
            }
            if (a == "--no-config" || a == "-h" || a == "--help") continue;
            std::cerr << "Unknown option: " << a << std::endl;
            std::cerr << "Use --help to see available options." << std::endl;
            return 2;
        }

        for (int j = i; j < argc; ++j) child_argv.push_back(argv[j]);
        break;
    }

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc) {
            cfg.data_dir = expand_home(argv[i + 1]);
            ++i;
            continue;
        }
        if (a == "--status-fg" && i + 1 < argc) {
            int v = 0;
            if (parse_int(argv[i + 1], v)) cfg.status_fg_ansi = v;
            ++i;
            continue;
        }
        if (a == "--status-bg" && i + 1 < argc) {
            int v = 0;
            if (parse_int(argv[i + 1], v)) cfg.status_bg_ansi = v;
            ++i;
            continue;
        }
    }

    if (!cfg.data_dir.empty()) {
        if (!set_pinyin_data_dir(cfg.data_dir)) {
            std::cerr << "Invalid --data-dir (missing required files): " << cfg.data_dir << std::endl;
            return 2;
        }
    }
    if (cfg.status_fg_ansi < 0) cfg.status_fg_ansi = 0;
    if (cfg.status_fg_ansi > 255) cfg.status_fg_ansi = 255;
    if (cfg.status_bg_ansi < 0) cfg.status_bg_ansi = 0;
    if (cfg.status_bg_ansi > 255) cfg.status_bg_ansi = 255;
    set_status_bar_colors(cfg.status_fg_ansi, cfg.status_bg_ansi);

    if (getenv("LITE_TTY_IME_ACTIVE")) {
        std::cerr << "Error: lite-tty-ime is already running in this terminal." << std::endl;
        return 1;
    }
    const char* debug_out = getenv("LITE_TTY_IME_DEBUG_OUT");
    if (debug_out && *debug_out) {
        g_debug_out_fd = open(debug_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_debug_out_fd < 0) {
            std::cerr << "Warning: cannot open debug output: " << debug_out << std::endl;
        }
    }
    init_logger();
    int rows = 0, cols = 0;
    get_terminal_size(rows, cols);
    g_rows = rows;
    g_cols = cols;
    int child_rows = rows > 1 ? (rows - 1) : 1;
 
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) {
        perror("tcgetattr");
        return 1;
    }
    atexit(reset_terminal);
    signal(SIGINT, handle_exit_signal);
    signal(SIGTERM, handle_exit_signal);
    signal(SIGWINCH, handle_winch);
 
    termios raw = g_orig_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
 
    const char* enter = "\033[H\033[2J\033[?1049h\033[H";
    write(STDOUT_FILENO, enter, (size_t)strlen(enter));
    std::string limit_scroll = "\033[1;" + std::to_string(rows - 1) + "r";
    write(STDOUT_FILENO, limit_scroll.c_str(), limit_scroll.size());

    init_status_bar();
    ime_controller_init();
    setenv("LITE_TTY_IME_ACTIVE", "1", 1);
 
    winsize ws{};
    ws.ws_row = (unsigned short)child_rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
 
    pid_t pid = forkpty(&g_master_fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        perror("forkpty");
        return 1;
    }
    if (pid == 0) {
        if (!child_argv.empty()) {
            child_argv.push_back(nullptr);
            execvp(child_argv[0], child_argv.data());
            _exit(1);
        }
        const char* shell = getenv("SHELL");
        if (!shell) shell = "/bin/bash";
        execl(shell, shell, (char*)nullptr);
        _exit(1);
    }
 
    VirtualScreen screen;
    screen.resize(child_rows, cols);
    AnsiParser parser(screen, g_master_fd);
 
    auto last_render = std::chrono::steady_clock::time_point{};
    char buf[8192];
    fd_set readfds;
 
    while (true) {
        apply_winch_if_needed(screen);

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(g_master_fd, &readfds);
        int nfds = g_master_fd > STDIN_FILENO ? (g_master_fd + 1) : (STDIN_FILENO + 1);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 20 * 1000;
        int rc = select(nfds, &readfds, nullptr, nullptr, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // 空闲点：延迟写盘（去抖 400ms），不阻塞按键/输出处理。
        flush_dirty_dicts_if_idle();

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                if ((unsigned char)buf[i] == 29) {
                    reset_terminal();
                    return 0;
                }
            }
            std::string in_to_child = translate_input_for_child(buf, n);
            ime_controller_handle_input_bytes(in_to_child.data(), (ssize_t)in_to_child.size(), g_master_fd);
        }
 
        if (FD_ISSET(g_master_fd, &readfds)) {
            ssize_t n = read(g_master_fd, buf, sizeof(buf));
            if (n <= 0) break;
            // 调试：LITE_TTY_IME_DEBUG_OUT=<path> 时把子程序原始输出落盘，
            // 用于定位 vterm 解析/渲染错位问题。
            if (g_debug_out_fd >= 0) {
                write(g_debug_out_fd, buf, (size_t)n);
            }
            parser.feed(buf, (size_t)n);
        }
 
        auto now = std::chrono::steady_clock::now();
        if (last_render == std::chrono::steady_clock::time_point{} ||
            now - last_render > std::chrono::milliseconds(33)) {
            render_to_real_terminal(screen, g_rows, g_cols);
            last_render = now;
        }
 
        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
    }
 
    return 0;
}
