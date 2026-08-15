#include "status_bar.h"
#include "vterm/shell_api.h"
#include "util.h"
#include <string>

/**
 * 状态栏逻辑实现
 */

static std::string current_mode = "EN";
static std::string current_buffer = "";
static int g_status_bar_fg_ansi = 38;
static int g_status_bar_bg_ansi = 39;

void set_status_bar_colors(int fg_ansi, int bg_ansi) {
    g_status_bar_fg_ansi = fg_ansi;
    g_status_bar_bg_ansi = bg_ansi;
}

/**
 * 初始化状态栏主题与初始文案。
 * 启动时设置默认颜色，并触发一次完整刷新。
 */
void init_status_bar() {
    write_log("Initializing Status Bar content...", INFO);
    
    // 设置初始颜色：黑色背景，灰色文字
    vterm::set_status_bar_bg_ansi(g_status_bar_bg_ansi);
    vterm::set_status_bar_fg_ansi(g_status_bar_fg_ansi);
    
    refresh_status_bar();
}

/**
 * 更新输入法状态上下文并刷新状态栏。
 *
 * @param mode_name 模式名（例如 EN / 中文）。
 * @param buffer 预编辑缓冲展示文本。
 */
void update_ime_status(const std::string& mode_name, const std::string& buffer) {
    current_mode = mode_name;
    current_buffer = buffer;
    refresh_status_bar();
}

/**
 * 根据当前状态拼接并绘制状态栏文案。
 * 关键行为：
 * - 中文模式标签从“中文”压缩为“中”；
 * - 文案超出终端宽度时截断，避免换行污染主区域。
 */
void refresh_status_bar() {
    // 紧凑显示：[模式]已拆分拼音
    std::string mode_tag = (current_mode == "中文") ? "中" : current_mode;
    std::string full_content = "[" + mode_tag + "]" + current_buffer;

    // 基于终端宽度做截断，避免状态栏文本溢出
    int rows = 0, cols = 0;
    get_terminal_size(rows, cols);
    if (cols > 0 && get_display_width(full_content) > cols) {
        full_content.resize(get_utf8_cut_index_by_width(full_content, cols));
    }
    
    // 写入底层的状态栏缓存并触发渲染
    vterm::write_status_bar(full_content);
}
