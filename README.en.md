**English** | [简中](README.md)
# lite-tty-ime (English Guide, v0.3.0)

This project is a terminal-native (TTY) pinyin input method.  
It intercepts keyboard input and provides candidate selection, frequency learning, hot reload, and shell-friendly behavior.

---

## 1. Current Feature List (Item by Item)

### 1.1 Terminal runtime mode
- Shell wrapper mode: runs your shell through PTY (default `/bin/bash`).
- Bottom status bar: shows IME mode, current preedit, candidate info, and hints.
- In shell mode, committed bytes are forwarded to PTY to keep shell behavior natural.

### 1.2 Chinese/English mode switching
- A configurable toggle key (default `Ctrl+Space`) switches IME ON/OFF.
- In English mode, characters are passed/committed directly.
- State buffers are reset on mode switch to avoid stale context.

### 1.3 Pinyin parsing and candidate generation
- Supports `a-z` pinyin typing.
- Supports manual separator `'`.
- Internally uses multiple segmentation strategies:
  - Aggressive split
  - Conservative split
  - Merge-after-split
- Candidate sources include:
  - Base dictionary
  - User dictionary
  - Learned phrase hits from `learned_phrases.txt`
- Can promote a best full-string stitched candidate to the top.

### 1.4 Candidate selection and paging
- `1..9`: select candidate on current page.
- `+` / `=`: next page.
- `-`: previous page.
- Page size is configurable (`1..9`, default `5`).

### 1.5 Configurable action policy for Space/Enter
- `space_action` and `enter_action` support:
  - `select_first`
  - `commit_raw`
  - `send_line`
  - `none`

### 1.6 Full-width punctuation (guarded behavior)
- Works only when there is no active pinyin input and no candidate list.
- Examples: `,` -> `，`, `.` -> `。`, `?` -> `？`, `(` -> `（`.
- This avoids conflicts with candidate picking and editing.

### 1.7 ESC sequence and cursor behavior
- Parses common ESC sequences so IME does not steal TUI navigation keys.
- Esc/arrow sequences are forwarded to shell apps when needed.
- In Chinese preedit, parsed `Ctrl+Left` / `Ctrl+Right` can move pinyin cursor.

### 1.8 Frequency tracking and ranking
- After candidate commit, frequency stats are written to `<user_dict>.freq`.
- Candidate ordering uses frequency + recency weighting.
- Frequently selected words naturally move up.

### 1.9 Auto-learning to user dictionary / phrase dictionary
- After continuous selection, rules decide whether to store into:
  - user dictionary (`user_dict.txt`)
  - learned phrase dictionary (`learned_phrases.txt`)
- Decision logic is in `learn_rules.*`.

### 1.10 Dictionary hot reload
- Polls file mtimes and hot reloads without restart.
- Covers:
  - base dictionary
  - user dictionary
  - frequency file
  - learned phrase file

### 1.11 Startup command and logging
- `--startup-cmd`: run one shell command after startup.
- `--log-file`: append runtime logs.
- `--debug`: enable debug-oriented output.

---

## 2. CLI Parameters (currently supported)

- `-h`, `--help`
- `-V`, `--version`
- `-s`, `--startup-cmd "CMD"`
- `--debug`
- `--dict PATH`
- `--user-dict PATH`
- `--config PATH`
- `--log-file PATH`
- `--toggle-key KEY`
- `--exit-key KEY`

Note: some legacy flags are explicitly removed and will return errors.

---

## 3. Config Keys Recognized by Current Code

- `dict` / `dict_path`
- `user_dict` / `user_dict_path`
- `toggle_key`
- `exit_key`
- `candidate_page_size` / `page_size`
- `space_action`
- `enter_action`
- `continuous_backtrack`
- `debug`
- `show_con_stats` / `show_continuous_stats`
- `show_store_hint` / `show_store_message`
- `log_file`

---

## 4. Runtime Files Read/Written

- Base dictionary (read): path from `--dict`.
- User dictionary (read/write): path from `--user-dict`.
- Frequency file (read/write): `<user_dict>.freq`.
- Learned phrase file (read/write): `learned_phrases.txt` in same directory as user dict.

If default system path is not writable, code falls back to user-space directory (`$XDG_DATA_HOME` or `~/.local/share/...`).

---

## 5. Keymap Quick Reference

- `1..9`: select candidate
- `+` / `=` / `-`: page navigation
- `'`: manual pinyin separator
- `Esc` and arrow sequences: preserved/forwarded
- `Backspace`: delete preedit or rollback composed content
- `Enter` / `Space`: behavior depends on configured action

---

## 6. Version in Source

- Current source version: `0.3.0`
- Version constant location: `kVersion` in `src/main.cpp`
