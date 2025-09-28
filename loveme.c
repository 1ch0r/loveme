#define _POSIX_C_SOURCE 200809L
// Testing capabilities. Incomplete.
// Build: gcc -std=c11 -O2 -Wall -Wextra -o devstral_agent devstral_agent.c -lncurses -ltinfo
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <ncurses.h>
#include <signal.h>
#include <libgen.h>

#define PATH_MAX_LEN 4096
#define BUF_SIZE 8192
#define DEFAULT_MAX_TOTAL (4*1024*1024)
#define DEFAULT_MAX_FILE  (512*1024)
#define MAX_PLAN_FILES 256
#define MAX_LINE 4096
#define MAX_HISTORY 100
#define INPUT_HEIGHT 5
#define MAX_FILES 1000

typedef struct {
    char workdir[PATH_MAX_LEN];
    char model[PATH_MAX_LEN];
    char cli[PATH_MAX_LEN];
    char mode[32];
    char focus_file[PATH_MAX_LEN];
    char test_cmd[PATH_MAX_LEN];
    size_t max_total;
    size_t max_file;
    size_t ctx_size;
    size_t n_predict;
    int apply_changes;
    int run_tests;
    int stream_output;
    int include_code;
} Config;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    char prompts[MAX_HISTORY][2048];
    char responses[MAX_HISTORY][8192];
    int count;
    int current;
} History;

typedef struct {
    char path[PATH_MAX_LEN];
    size_t size;
    int is_dir;
} FileEntry;

typedef struct {
    FileEntry files[MAX_FILES];
    int count;
    int selected;
} FileList;

typedef struct {
    char filepath[PATH_MAX_LEN];
    char *content;
    int applied;
} FileChange;

static WINDOW *config_win, *prompt_win, *output_win, *status_win, *file_win;
static Config global_cfg;
static History history = {0};
static FileList file_list = {0};
static int should_exit = 0;
static int ui_mode = 0; // 0=normal, 1=file_browser

// Colors
enum {
    COLOR_HEADER = 1,
    COLOR_BORDER = 2,
    COLOR_HIGHLIGHT = 3,
    COLOR_ERROR = 4,
    COLOR_SUCCESS = 5,
    COLOR_INPUT = 6,
    COLOR_CODE = 7,
    COLOR_SELECTED = 8
};

static void die(const char *msg) { 
    endwin(); 
    perror(msg); 
    exit(1); 
}

static void cleanup_ncurses(void) {
    if (config_win) delwin(config_win);
    if (prompt_win) delwin(prompt_win);
    if (output_win) delwin(output_win);
    if (status_win) delwin(status_win);
    if (file_win) delwin(file_win);
    endwin();
}

static void signal_handler(int sig) {
    cleanup_ncurses();
    exit(sig);
}

// Buffer helpers
static void buffer_init(Buffer *b) {
    b->cap = BUF_SIZE;
    b->data = malloc(b->cap);
    if (!b->data) die("malloc");
    b->len = 0;
    b->data[0] = '\0';
}

static void buffer_free(Buffer *b) { 
    free(b->data); 
    b->data = NULL; 
    b->len = b->cap = 0; 
}

static void buffer_clear(Buffer *b) { 
    b->len = 0; 
    b->data[0] = '\0'; 
}

static void buffer_ensure_capacity(Buffer *b, size_t needed) {
    if (b->len + needed + 1 > b->cap) {
        size_t newcap = (b->len + needed + 1) * 2;
        b->data = realloc(b->data, newcap);
        if (!b->data) die("realloc");
        b->cap = newcap;
    }
}

static void buffer_append(Buffer *b, const char *s) {
    size_t sl = strlen(s);
    buffer_ensure_capacity(b, sl);
    memcpy(b->data + b->len, s, sl);
    b->len += sl;
    b->data[b->len] = '\0';
}

static void buffer_append_fmt(Buffer *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[BUF_SIZE];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buffer_append(b, tmp);
}

// File helpers
static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return lf <= ls && strcmp(s + ls - lf, suf) == 0;
}

static int is_code_file(const char *path) {
    const char *exts[] = {".c", ".h", ".cpp", ".cc", ".cxx", ".py", ".js", ".ts", 
        ".jsx", ".tsx", ".java", ".cs", ".go", ".rs", ".rb", ".php", ".swift", 
        ".kt", ".m", ".mm", ".scala", ".lua", ".pl", ".sh", ".rs", ".toml", 
        ".yaml", ".yml", ".json", ".xml", ".md", ".txt", ".sql", ".r", ".R"};
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (ends_with(path, exts[i])) return 1;
    return 0;
}

static int should_ignore(const char *name) {
    // Ignore common non-source directories and files
    const char *ignore[] = {"node_modules", ".git", ".svn", "__pycache__", 
        ".pytest_cache", "dist", "build", "target", ".next", ".vscode",
        ".idea", "*.pyc", "*.o", "*.so", "*.dll", "*.exe", ".DS_Store"};
    
    for (size_t i = 0; i < sizeof(ignore) / sizeof(ignore[0]); i++) {
        if (strcmp(name, ignore[i]) == 0) return 1;
        // Handle wildcards
        if (ignore[i][0] == '*' && ends_with(name, ignore[i] + 1)) return 1;
    }
    return 0;
}

// Read file content
static char* read_file_content(const char *path, size_t max_size) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size == 0) {
        fclose(f);
        return NULL;
    }
    
    size_t file_size = (size_t)st.st_size;
    if (file_size > max_size) file_size = max_size;
    
    char *content = malloc(file_size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }
    
    size_t read = fread(content, 1, file_size, f);
    content[read] = '\0';
    fclose(f);
    
    return content;
}

// Write file content
static int write_file_content(const char *path, const char *content) {
    // Create backup
    char backup_path[PATH_MAX_LEN];
    snprintf(backup_path, sizeof(backup_path), "%s.bak", path);
    
    // Read original for backup
    char *original = read_file_content(path, DEFAULT_MAX_FILE * 10);
    if (original) {
        FILE *backup = fopen(backup_path, "w");
        if (backup) {
            fputs(original, backup);
            fclose(backup);
        }
        free(original);
    }
    
    // Write new content
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    
    fputs(content, f);
    fclose(f);
    return 0;
}

// Recursive directory scanning
static void scan_directory(const char *path, FileList *list, const char *base_path, int depth) {
    if (depth > 5) return; // Limit recursion depth
    if (list->count >= MAX_FILES) return;
    
    DIR *d = opendir(path);
    if (!d) return;
    
    struct dirent *ent;
    while ((ent = readdir(d)) && list->count < MAX_FILES) {
        if (ent->d_name[0] == '.') continue;
        if (should_ignore(ent->d_name)) continue;
        
        char full[PATH_MAX_LEN];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        
        struct stat st;
        if (stat(full, &st) != 0) continue;
        
        // Calculate relative path
        char rel[PATH_MAX_LEN];
        if (strlen(full) > strlen(base_path) + 1) {
            strcpy(rel, full + strlen(base_path) + 1);
        } else {
            strcpy(rel, ent->d_name);
        }
        
        if (S_ISDIR(st.st_mode)) {
            // Add directory entry
            strcpy(list->files[list->count].path, rel);
            list->files[list->count].size = 0;
            list->files[list->count].is_dir = 1;
            list->count++;
            
            // Recurse
            scan_directory(full, list, base_path, depth + 1);
        } else if (S_ISREG(st.st_mode) && is_code_file(ent->d_name)) {
            strcpy(list->files[list->count].path, rel);
            list->files[list->count].size = st.st_size;
            list->files[list->count].is_dir = 0;
            list->count++;
        }
    }
    closedir(d);
}

// Build repository context with actual code
static void build_repo_context(const Config *cfg, Buffer *ctx, int include_code) {
    buffer_append_fmt(ctx, "Repository root: %s\n\n", cfg->workdir);
    
    // Scan files
    file_list.count = 0;
    scan_directory(cfg->workdir, &file_list, cfg->workdir, 0);
    
    buffer_append(ctx, "## Repository Structure:\n");
    
    size_t total_added = 0;
    for (int i = 0; i < file_list.count && total_added < cfg->max_total; i++) {
        FileEntry *fe = &file_list.files[i];
        
        if (fe->is_dir) {
            buffer_append_fmt(ctx, "📁 %s/\n", fe->path);
        } else {
            buffer_append_fmt(ctx, "📄 %s (%zu bytes)\n", fe->path, fe->size);
            
            // Include actual code if requested and file is focused or small enough
            if (include_code && fe->size < cfg->max_file) {
                int include_this = 0;
                
                // Always include focused file
                if (strlen(cfg->focus_file) > 0 && strstr(fe->path, cfg->focus_file)) {
                    include_this = 1;
                }
                // Include small important files
                else if (fe->size < 10000 && (ends_with(fe->path, ".h") || 
                         ends_with(fe->path, "README.md") || 
                         ends_with(fe->path, "Makefile") ||
                         ends_with(fe->path, "CMakeLists.txt") ||
                         ends_with(fe->path, "package.json"))) {
                    include_this = 1;
                }
                
                if (include_this && total_added + fe->size < cfg->max_total) {
                    char full_path[PATH_MAX_LEN];
                    snprintf(full_path, sizeof(full_path), "%s/%s", cfg->workdir, fe->path);
                    
                    char *content = read_file_content(full_path, cfg->max_file);
                    if (content) {
                        buffer_append_fmt(ctx, "\n### File: %s\n```\n", fe->path);
                        buffer_append(ctx, content);
                        buffer_append(ctx, "\n```\n\n");
                        total_added += strlen(content);
                        free(content);
                    }
                }
            }
        }
    }
    
    buffer_append_fmt(ctx, "\nTotal files: %d\n", file_list.count);
}

// Enhanced prompt building
static void append_system_prompt(Buffer *out, const Config *cfg) {
    buffer_append(out,
        "<|system|>\n"
        "You are Devstral, an advanced repository agent specialized in code analysis and modification.\n\n"
        "Core Capabilities:\n"
        "- Comprehensive code analysis with architectural insights\n"
        "- Precise file modifications with context awareness\n"
        "- Test-driven development support\n"
        "- Refactoring and optimization suggestions\n\n");
    
    if (strcmp(cfg->mode, "edit") == 0) {
        buffer_append(out,
            "EDIT MODE - Output format for file changes:\n"
            "<<<FILE: path/to/file.ext>>>\n"
            "<<<REPLACEMENT_START>>>\n"
            "complete new file content here\n"
            "<<<REPLACEMENT_END>>>\n\n"
            "- Always output complete file content\n"
            "- Multiple files can be edited in one response\n"
            "- Include clear explanations before changes\n\n");
    } else if (strcmp(cfg->mode, "agent") == 0) {
        buffer_append(out,
            "AGENT MODE - Autonomous problem solving:\n"
            "1. Analyze the repository structure and understand the codebase\n"
            "2. Create a detailed plan with specific steps\n"
            "3. Implement changes systematically\n"
            "4. Suggest tests and validation steps\n"
            "5. Provide file modifications in EDIT format when needed\n\n");
    }
    
    buffer_append(out, "<|endofsystem|>\n\n");
}

static void build_conversation_context(Buffer *out) {
    if (history.count > 0) {
        buffer_append(out, "<|conversation_history|>\n");
        
        // Include last 3 exchanges for context
        int start = history.count > 3 ? history.count - 3 : 0;
        for (int i = start; i < history.count; i++) {
            buffer_append_fmt(out, "User: %s\n", history.prompts[i]);
            if (strlen(history.responses[i]) > 500) {
                // Truncate long responses
                char truncated[500];
                strncpy(truncated, history.responses[i], 497);
                truncated[497] = '\0';
                buffer_append_fmt(out, "Assistant: %s...\n\n", truncated);
            } else {
                buffer_append_fmt(out, "Assistant: %s\n\n", history.responses[i]);
            }
        }
        buffer_append(out, "<|endofhistory|>\n\n");
    }
}

static void build_enhanced_prompt(const Config *cfg, const char *task, Buffer *out) {
    append_system_prompt(out, cfg);
    
    // Add conversation history for context
    build_conversation_context(out);
    
    // Add repository context
    Buffer repo;
    buffer_init(&repo);
    build_repo_context(cfg, &repo, cfg->include_code);
    
    buffer_append(out, "<|repository_context|>\n");
    buffer_append(out, repo.data);
    buffer_append(out, "<|endofcontext|>\n\n");
    
    // Add user query
    buffer_append(out, "<|user|>\n");
    buffer_append_fmt(out, "%s\n", task);
    buffer_append(out, "<|endofuser|>\n\n");
    
    // Add assistant tag for response
    buffer_append(out, "<|assistant|>\n");
    
    buffer_free(&repo);
}

// Parse file changes from response
static int parse_file_changes(const char *response, FileChange **changes, int *num_changes) {
    *num_changes = 0;
    *changes = malloc(sizeof(FileChange) * MAX_PLAN_FILES);
    if (!*changes) return -1;
    
    const char *p = response;
    while ((p = strstr(p, "<<<FILE:")) != NULL) {
        p += 8; // Skip "<<<FILE:"
        while (*p == ' ') p++;
        
        // Extract filename
        const char *end = strstr(p, ">>>");
        if (!end) break;
        
        size_t len = end - p;
        if (len >= PATH_MAX_LEN) {
            p = end + 3;
            continue;
        }
        
        FileChange *change = &(*changes)[*num_changes];
        strncpy(change->filepath, p, len);
        change->filepath[len] = '\0';
        change->applied = 0;
        
        // Find replacement content
        p = strstr(end, "<<<REPLACEMENT_START>>>");
        if (!p) break;
        p += 23; // Skip marker
        if (*p == '\n') p++; // Skip newline
        
        const char *content_end = strstr(p, "<<<REPLACEMENT_END>>>");
        if (!content_end) break;
        
        len = content_end - p;
        change->content = malloc(len + 1);
        if (!change->content) break;
        
        strncpy(change->content, p, len);
        change->content[len] = '\0';
        
        (*num_changes)++;
        if (*num_changes >= MAX_PLAN_FILES) break;
        
        p = content_end + 20; // Skip end marker
    }
    
    return *num_changes > 0 ? 0 : -1;
}

// Apply file changes
static int apply_file_changes(const Config *cfg, FileChange *changes, int num_changes) {
    int success_count = 0;
    
    for (int i = 0; i < num_changes; i++) {
        char full_path[PATH_MAX_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", cfg->workdir, changes[i].filepath);
        
        // Create directories if needed
        char *path_copy = strdup(full_path);
        char *dir = dirname(path_copy);
        
        char mkdir_cmd[PATH_MAX_LEN];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dir);
        system(mkdir_cmd);
        free(path_copy);
        
        if (write_file_content(full_path, changes[i].content) == 0) {
            changes[i].applied = 1;
            success_count++;
        }
    }
    
    return success_count;
}

// Run tests
static int run_tests(const Config *cfg, Buffer *output) {
    if (strlen(cfg->test_cmd) == 0) {
        buffer_append(output, "No test command configured.\n");
        return -1;
    }
    
    buffer_append_fmt(output, "Running tests: %s\n", cfg->test_cmd);
    buffer_append(output, "─────────────────────────────\n");
    
    FILE *pipe = popen(cfg->test_cmd, "r");
    if (!pipe) {
        buffer_append(output, "Failed to run test command.\n");
        return -1;
    }
    
    char line[1024];
    while (fgets(line, sizeof(line), pipe)) {
        buffer_append(output, line);
    }
    
    int status = pclose(pipe);
    int exit_code = WEXITSTATUS(status);
    
    buffer_append(output, "─────────────────────────────\n");
    buffer_append_fmt(output, "Tests %s (exit code: %d)\n", 
                     exit_code == 0 ? "PASSED" : "FAILED", exit_code);
    
    return exit_code;
}

// Enhanced response extraction
static void extract_clean_response(const char *raw_output, Buffer *clean) {
    buffer_clear(clean);
    
    // Skip until we find assistant response markers
    const char *start = raw_output;
    
    // Look for common response start patterns
    const char *markers[] = {
        "<|assistant|>",
        "Assistant:",
        "Response:",
        "Output:",
        NULL
    };
    
    for (int i = 0; markers[i]; i++) {
        const char *found = strstr(raw_output, markers[i]);
        if (found) {
            start = found + strlen(markers[i]);
            while (*start && (*start == '\n' || *start == ' ')) start++;
            break;
        }
    }
    
    // If no markers found, look for end of system output
    if (start == raw_output) {
        // Skip llama.cpp initialization output
        const char *p = raw_output;
        while (*p) {
            if (strncmp(p, "llama_", 6) == 0 || 
                strncmp(p, "llm_", 4) == 0 ||
                strncmp(p, "ggml_", 5) == 0 ||
                strstr(p, "sampling:") == p ||
                strstr(p, "loaded ") == p ||
                strstr(p, "system_info:") == p) {
                // Skip this line
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                start = p;
            } else {
                break;
            }
        }
    }
    
    // Copy response, stopping at end markers
    const char *p = start;
    while (*p) {
        // Stop at common end markers
        if (strncmp(p, "<|endoftext|>", 13) == 0 ||
            strncmp(p, "<|end|>", 7) == 0 ||
            strncmp(p, "[end of text]", 13) == 0) {
            break;
        }
        
        // Skip timing information at the end
        if (strstr(p, "llama_print_timings:") == p) break;
        
        buffer_ensure_capacity(clean, 1);
        clean->data[clean->len++] = *p++;
    }
    
    clean->data[clean->len] = '\0';
    
    // Trim trailing whitespace
    while (clean->len > 0 && isspace(clean->data[clean->len - 1])) {
        clean->len--;
    }
    clean->data[clean->len] = '\0';
}

// UI Functions
static void init_colors(void) {
    start_color();
    init_pair(COLOR_HEADER, COLOR_CYAN, COLOR_BLACK);
    init_pair(COLOR_BORDER, COLOR_BLUE, COLOR_BLACK);
    init_pair(COLOR_HIGHLIGHT, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_ERROR, COLOR_RED, COLOR_BLACK);
    init_pair(COLOR_SUCCESS, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_INPUT, COLOR_WHITE, COLOR_BLUE);
    init_pair(COLOR_CODE, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_YELLOW);
}

static void draw_border(WINDOW *win, const char *title) {
    wattron(win, COLOR_PAIR(COLOR_BORDER));
    box(win, 0, 0);
    if (title) {
        mvwprintw(win, 0, 2, " %s ", title);
    }
    wattroff(win, COLOR_PAIR(COLOR_BORDER));
    wrefresh(win);
}

static void init_windows(void) {
    int height, width;
    getmaxyx(stdscr, height, width);
    
    if (height < 24 || width < 80) {
        endwin();
        printf("Terminal too small. Need at least 80x24.\n");
        exit(1);
    }
    
    // Config window (top section)
    config_win = newwin(8, width, 0, 0);
    
    // File browser (left side, if active)
    if (ui_mode == 1) {
        file_win = newwin(height - 9, width / 3, 8, 0);
        prompt_win = newwin(INPUT_HEIGHT + 2, width * 2 / 3, 8, width / 3);
        output_win = newwin(height - 17 - INPUT_HEIGHT, width * 2 / 3, 
                          8 + INPUT_HEIGHT + 2, width / 3);
    } else {
        prompt_win = newwin(INPUT_HEIGHT + 2, width, 8, 0);
        output_win = newwin(height - 17 - INPUT_HEIGHT, width, 8 + INPUT_HEIGHT + 2, 0);
    }
    
    // Status window (bottom line)
    status_win = newwin(1, width, height - 1, 0);
    
    // Enable scrolling
    if (output_win) scrollok(output_win, TRUE);
    if (prompt_win) keypad(prompt_win, TRUE);
    if (file_win) keypad(file_win, TRUE);
    
    refresh();
}

static void update_status(const char *msg, int color) {
    werase(status_win);
    wattron(status_win, COLOR_PAIR(color));
    mvwprintw(status_win, 0, 0, "%s", msg);
    wclrtoeol(status_win);
    wattroff(status_win, COLOR_PAIR(color));
    wrefresh(status_win);
}

static void display_file_browser(void) {
    if (!file_win || file_list.count == 0) return;
    
    werase(file_win);
    draw_border(file_win, "Files");
    
    int height = getmaxy(file_win) - 2;
    int start = 0;
    
    // Adjust view to show selected file
    if (file_list.selected >= start + height) {
        start = file_list.selected - height + 1;
    } else if (file_list.selected < start) {
        start = file_list.selected;
    }
    
    for (int i = start, row = 1; i < file_list.count && row < height + 1; i++, row++) {
        if (i == file_list.selected) {
            wattron(file_win, COLOR_PAIR(COLOR_SELECTED));
        }
        
        FileEntry *fe = &file_list.files[i];
        if (fe->is_dir) {
            mvwprintw(file_win, row, 2, "📁 %s", fe->path);
        } else {
            mvwprintw(file_win, row, 2, "📄 %s", fe->path);
        }
        
        if (i == file_list.selected) {
            wattroff(file_win, COLOR_PAIR(COLOR_SELECTED));
        }
    }
    
    wrefresh(file_win);
}

static void display_response_with_highlighting(const char *response) {
    if (!output_win) return;
    
    werase(output_win);
    draw_border(output_win, "AI Response");
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    wmove(output_win, 1, 2);
    wattron(output_win, COLOR_PAIR(COLOR_HIGHLIGHT));
    wprintw(output_win, "[%02d:%02d:%02d] ", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    wattroff(output_win, COLOR_PAIR(COLOR_HIGHLIGHT));
    
    // Parse and highlight code blocks and file changes
    const char *p = response;
    int in_code = 0;
    int in_file_change = 0;
    int y = 2, x = 2;
    int max_y = getmaxy(output_win) - 1;
    int max_x = getmaxx(output_win) - 2;
    
    while (*p && y < max_y) {
        if (strncmp(p, "```", 3) == 0) {
            in_code = !in_code;
            p += 3;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        
        if (strncmp(p, "<<<FILE:", 8) == 0) {
            in_file_change = 1;
            wattron(output_win, COLOR_PAIR(COLOR_SUCCESS) | A_BOLD);
        } else if (strncmp(p, "<<<REPLACEMENT_END>>>", 20) == 0) {
            in_file_change = 0;
            wattroff(output_win, COLOR_PAIR(COLOR_CODE));
            p += 20;
            if (*p == '\n') p++;
            continue;
        }
        
        if (in_code || in_file_change) {
            wattron(output_win, COLOR_PAIR(COLOR_CODE));
        }
        
        // Handle line breaks and word wrapping
        if (*p == '\n') {
            if (in_code || in_file_change) {
                wattroff(output_win, COLOR_PAIR(COLOR_CODE));
            }
            y++;
            x = 2;
            wmove(output_win, y, x);
            p++;
        } else {
            if (x >= max_x - 1) {
                y++;
                x = 2;
                wmove(output_win, y, x);
            }
            waddch(output_win, *p);
            x++;
            p++;
        }
        
        if (in_code || in_file_change) {
            wattroff(output_win, COLOR_PAIR(COLOR_CODE));
        }
    }
    
    wrefresh(output_win);
}

static void draw_config(void) {
    werase(config_win);
    draw_border(config_win, "Devstral Agent Configuration");
    
    wattron(config_win, COLOR_PAIR(COLOR_HEADER));
    mvwprintw(config_win, 1, 2, "Enhanced Repository Assistant v2.0");
    wattroff(config_win, COLOR_PAIR(COLOR_HEADER));
    
    mvwprintw(config_win, 2, 2, "Workdir: %.50s", global_cfg.workdir);
    mvwprintw(config_win, 3, 2, "Model:   %.50s", global_cfg.model);
    mvwprintw(config_win, 4, 2, "Mode:    %-10s | Context: %zu | Predict: %zu", 
              global_cfg.mode, global_cfg.ctx_size, global_cfg.n_predict);
    mvwprintw(config_win, 5, 2, "Options: Apply[%c] Tests[%c] Stream[%c] Code[%c] | Focus: %.30s",
              global_cfg.apply_changes ? 'X' : ' ',
              global_cfg.run_tests ? 'X' : ' ',
              global_cfg.stream_output ? 'X' : ' ',
              global_cfg.include_code ? 'X' : ' ',
              global_cfg.focus_file[0] ? global_cfg.focus_file : "none");
    
    wattron(config_win, COLOR_PAIR(COLOR_HIGHLIGHT));
    mvwprintw(config_win, 6, 2, "Commands: [c]onfig [f]iles [h]istory [t]est [a]pply [q]uit [Enter]prompt");
    wattroff(config_win, COLOR_PAIR(COLOR_HIGHLIGHT));
    
    wrefresh(config_win);
}

static void get_input(WINDOW *win, const char *prompt, char *buf, size_t buflen) {
    werase(win);
    draw_border(win, "Input");
    mvwprintw(win, 1, 2, "%s: ", prompt);
    wrefresh(win);
    
    echo();
    curs_set(1);
    int prompt_len = strlen(prompt) + 4;
    wmove(win, 1, prompt_len);
    wgetnstr(win, buf, buflen - 1);
    noecho();
    curs_set(0);
}

static void get_multiline_input(char *buf, size_t buflen) {
    werase(prompt_win);
    draw_border(prompt_win, "Enter prompt (Ctrl+D to send, Ctrl+C to cancel)");
    wmove(prompt_win, 1, 2);
    wrefresh(prompt_win);
    
    int y = 1, x = 2;
    size_t pos = 0;
    int ch;
    int max_y = INPUT_HEIGHT;
    int max_x = getmaxx(prompt_win) - 3;
    
    curs_set(1);
    wmove(prompt_win, y, x);
    
    while ((ch = wgetch(prompt_win)) != 4 && pos < buflen - 1) { // Ctrl+D
        if (ch == 3) { // Ctrl+C
            buf[0] = '\0';
            break;
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            buf[pos++] = '\n';
            y++;
            x = 2;
            if (y >= max_y) {
                y = max_y - 1;
                wscrl(prompt_win, 1);
                draw_border(prompt_win, "Enter prompt (Ctrl+D to send)");
            }
            wmove(prompt_win, y, x);
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                pos--;
                if (buf[pos] == '\n') {
                    if (y > 1) {
                        y--;
                        // Find end of previous line
                        int temp_pos = pos - 1;
                        x = 2;
                        while (temp_pos >= 0 && buf[temp_pos] != '\n') {
                            x++;
                            temp_pos--;
                        }
                    }
                } else {
                    if (x > 2) {
                        x--;
                    } else if (y > 1) {
                        y--;
                        x = max_x - 1;
                    }
                }
                mvwaddch(prompt_win, y, x, ' ');
                wmove(prompt_win, y, x);
            }
        } else if (isprint(ch)) {
            buf[pos++] = ch;
            waddch(prompt_win, ch);
            x++;
            if (x >= max_x) {
                x = 2;
                y++;
                if (y >= max_y) {
                    y = max_y - 1;
                    wscrl(prompt_win, 1);
                    draw_border(prompt_win, "Enter prompt (Ctrl+D to send)");
                }
                wmove(prompt_win, y, x);
            }
        }
        wrefresh(prompt_win);
    }
    
    buf[pos] = '\0';
    curs_set(0);
}

static void configure_settings(void) {
    char buf[PATH_MAX_LEN];
    
    get_input(prompt_win, "Repository workdir", buf, sizeof(buf));
    if (strlen(buf) > 0) strncpy(global_cfg.workdir, buf, sizeof(global_cfg.workdir) - 1);
    
    get_input(prompt_win, "Model path", buf, sizeof(buf));
    if (strlen(buf) > 0) strncpy(global_cfg.model, buf, sizeof(global_cfg.model) - 1);
    
    get_input(prompt_win, "CLI binary path", buf, sizeof(buf));
    if (strlen(buf) > 0) strncpy(global_cfg.cli, buf, sizeof(global_cfg.cli) - 1);
    
    get_input(prompt_win, "Mode (overview/edit/agent)", buf, sizeof(buf));
    if (strlen(buf) > 0) strncpy(global_cfg.mode, buf, sizeof(global_cfg.mode) - 1);
    
    get_input(prompt_win, "Context size (8192-32768)", buf, sizeof(buf));
    if (strlen(buf) > 0) {
        size_t val = strtoull(buf, NULL, 10);
        if (val > 0) global_cfg.ctx_size = val;
    }
    
    get_input(prompt_win, "Max tokens (1024-8192)", buf, sizeof(buf));
    if (strlen(buf) > 0) {
        size_t val = strtoull(buf, NULL, 10);
        if (val > 0) global_cfg.n_predict = val;
    }
    
    get_input(prompt_win, "Auto-apply changes? (y/n)", buf, sizeof(buf));
    global_cfg.apply_changes = (buf[0] == 'y' || buf[0] == 'Y');
    
    get_input(prompt_win, "Run tests after changes? (y/n)", buf, sizeof(buf));
    global_cfg.run_tests = (buf[0] == 'y' || buf[0] == 'Y');
    
    if (global_cfg.run_tests) {
        get_input(prompt_win, "Test command", buf, sizeof(buf));
        if (strlen(buf) > 0) strncpy(global_cfg.test_cmd, buf, sizeof(global_cfg.test_cmd) - 1);
    }
    
    get_input(prompt_win, "Include code in context? (y/n)", buf, sizeof(buf));
    global_cfg.include_code = (buf[0] == 'y' || buf[0] == 'Y');
    
    save_config(&global_cfg);
    update_status("Configuration saved", COLOR_SUCCESS);
}

static void save_config(const Config *cfg) {
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/.devstral_config", getenv("HOME") ?: ".");
    
    FILE *f = fopen(path, "w");
    if (!f) return;
    
    fprintf(f, "workdir=%s\n", cfg->workdir);
    fprintf(f, "model=%s\n", cfg->model);
    fprintf(f, "cli=%s\n", cfg->cli);
    fprintf(f, "mode=%s\n", cfg->mode);
    fprintf(f, "test_cmd=%s\n", cfg->test_cmd);
    fprintf(f, "focus_file=%s\n", cfg->focus_file);
    fprintf(f, "ctx_size=%zu\n", cfg->ctx_size);
    fprintf(f, "n_predict=%zu\n", cfg->n_predict);
    fprintf(f, "max_total=%zu\n", cfg->max_total);
    fprintf(f, "max_file=%zu\n", cfg->max_file);
    fprintf(f, "apply_changes=%d\n", cfg->apply_changes);
    fprintf(f, "run_tests=%d\n", cfg->run_tests);
    fprintf(f, "include_code=%d\n", cfg->include_code);
    
    fclose(f);
}

static void load_config(Config *cfg) {
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/.devstral_config", getenv("HOME") ?: ".");
    
    FILE *f = fopen(path, "r");
    if (!f) return;
    
    char line[PATH_MAX_LEN];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        
        // Remove newline
        char *nl = strchr(value, '\n');
        if (nl) *nl = '\0';
        
        if (strcmp(key, "workdir") == 0) strncpy(cfg->workdir, value, sizeof(cfg->workdir) - 1);
        else if (strcmp(key, "model") == 0) strncpy(cfg->model, value, sizeof(cfg->model) - 1);
        else if (strcmp(key, "cli") == 0) strncpy(cfg->cli, value, sizeof(cfg->cli) - 1);
        else if (strcmp(key, "mode") == 0) strncpy(cfg->mode, value, sizeof(cfg->mode) - 1);
        else if (strcmp(key, "test_cmd") == 0) strncpy(cfg->test_cmd, value, sizeof(cfg->test_cmd) - 1);
        else if (strcmp(key, "focus_file") == 0) strncpy(cfg->focus_file, value, sizeof(cfg->focus_file) - 1);
        else if (strcmp(key, "ctx_size") == 0) cfg->ctx_size = strtoull(value, NULL, 10);
        else if (strcmp(key, "n_predict") == 0) cfg->n_predict = strtoull(value, NULL, 10);
        else if (strcmp(key, "max_total") == 0) cfg->max_total = strtoull(value, NULL, 10);
        else if (strcmp(key, "max_file") == 0) cfg->max_file = strtoull(value, NULL, 10);
        else if (strcmp(key, "apply_changes") == 0) cfg->apply_changes = atoi(value);
        else if (strcmp(key, "run_tests") == 0) cfg->run_tests = atoi(value);
        else if (strcmp(key, "include_code") == 0) cfg->include_code = atoi(value);
    }
    
    fclose(f);
}

// Run llama.cpp with streaming support
static int run_llama_streaming(const Config *cfg, const char *prompt, Buffer *out) {
    char tmpfile[PATH_MAX_LEN];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/devstral_%d.txt", getpid());
    
    FILE *f = fopen(tmpfile, "w");
    if (!f) return -1;
    fputs(prompt, f);
    fclose(f);
    
    char cmd[PATH_MAX_LEN * 4];
    snprintf(cmd, sizeof(cmd), 
        "%s -m %s -c %zu -n %zu --temp 0.3 --top-k 20 --top-p 0.95 "
        "--threads 4 --batch-size 512 --file %s 2>/dev/null",
        cfg->cli, cfg->model, cfg->ctx_size, cfg->n_predict, tmpfile);
    
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        unlink(tmpfile);
        return -1;
    }
    
    buffer_clear(out);
    char buf[256];
    
    // Stream output
    while (fgets(buf, sizeof(buf), pipe)) {
        buffer_append(out, buf);
        
        // Update display periodically if streaming
        if (cfg->stream_output && out->len % 1024 == 0) {
            Buffer clean;
            buffer_init(&clean);
            extract_clean_response(out->data, &clean);
            if (clean.len > 0) {
                display_response_with_highlighting(clean.data);
            }
            buffer_free(&clean);
        }
    }
    
    int rc = pclose(pipe);
    unlink(tmpfile);
    return rc;
}

// Display history browser
static void show_history(void) {
    if (history.count == 0) {
        update_status("No conversation history yet", COLOR_ERROR);
        return;
    }
    
    WINDOW *hist_win = newwin(LINES - 2, COLS, 0, 0);
    keypad(hist_win, TRUE);
    
    int selected = history.count - 1;
    int ch;
    
    while ((ch = wgetch(hist_win)) != 'q' && ch != 27) { // q or ESC
        werase(hist_win);
        draw_border(hist_win, "Conversation History (↑↓ navigate, Enter to view, q to exit)");
        
        int max_y = getmaxy(hist_win) - 2;
        int start = selected - max_y / 2;
        if (start < 0) start = 0;
        if (start + max_y > history.count) start = history.count - max_y;
        if (start < 0) start = 0;
        
        for (int i = start, y = 2; i < history.count && y < max_y; i++, y++) {
            if (i == selected) {
                wattron(hist_win, COLOR_PAIR(COLOR_SELECTED));
            }
            
            // Show truncated prompt
            char truncated[80];
            strncpy(truncated, history.prompts[i], 77);
            truncated[77] = '\0';
            if (strlen(history.prompts[i]) > 77) strcat(truncated, "...");
            
            mvwprintw(hist_win, y, 2, "[%d] %s", i + 1, truncated);
            
            if (i == selected) {
                wattroff(hist_win, COLOR_PAIR(COLOR_SELECTED));
            }
        }
        
        // Show selected entry details at bottom
        mvwhline(hist_win, max_y, 1, '-', COLS - 2);
        mvwprintw(hist_win, max_y + 1, 2, "Prompt: %.100s", history.prompts[selected]);
        
        wrefresh(hist_win);
        
        switch (ch) {
            case KEY_UP:
                if (selected > 0) selected--;
                break;
            case KEY_DOWN:
                if (selected < history.count - 1) selected++;
                break;
            case '\n':
            case KEY_ENTER:
                // Display full response
                display_response_with_highlighting(history.responses[selected]);
                update_status("Press any key to return to history", COLOR_HIGHLIGHT);
                getch();
                break;
        }
    }
    
    delwin(hist_win);
}

// File browser mode
static void browse_files(void) {
    ui_mode = 1;
    init_windows();
    
    // Refresh file list
    file_list.count = 0;
    scan_directory(global_cfg.workdir, &file_list, global_cfg.workdir, 0);
    
    int ch;
    while ((ch = wgetch(file_win)) != 'q' && ch != 27) {
        display_file_browser();
        
        switch (ch) {
            case KEY_UP:
                if (file_list.selected > 0) file_list.selected--;
                break;
            case KEY_DOWN:
                if (file_list.selected < file_list.count - 1) file_list.selected++;
                break;
            case '\n':
            case KEY_ENTER: {
                FileEntry *fe = &file_list.files[file_list.selected];
                if (!fe->is_dir) {
                    strncpy(global_cfg.focus_file, fe->path, sizeof(global_cfg.focus_file) - 1);
                    update_status("File focused for next prompt", COLOR_SUCCESS);
                }
                break;
            }
            case 'v': { // View file
                FileEntry *fe = &file_list.files[file_list.selected];
                if (!fe->is_dir) {
                    char full_path[PATH_MAX_LEN];
                    snprintf(full_path, sizeof(full_path), "%s/%s", global_cfg.workdir, fe->path);
                    char *content = read_file_content(full_path, DEFAULT_MAX_FILE);
                    if (content) {
                        display_response_with_highlighting(content);
                        free(content);
                        update_status("Press any key to return", COLOR_HIGHLIGHT);
                        getch();
                    }
                }
                break;
            }
        }
    }
    
    ui_mode = 0;
    init_windows();
}

// Process user prompt
static void process_prompt(const char *prompt_text) {
    if (strlen(prompt_text) == 0) return;
    
    // Save to history
    if (history.count < MAX_HISTORY) {
        strncpy(history.prompts[history.count], prompt_text, sizeof(history.prompts[0]) - 1);
    }
    
    update_status("Generating response... Please wait.", COLOR_HIGHLIGHT);
    
    Buffer prompt_buf, output_buf, clean_response;
    buffer_init(&prompt_buf);
    buffer_init(&output_buf);
    buffer_init(&clean_response);
    
    build_enhanced_prompt(&global_cfg, prompt_text, &prompt_buf);
    
    int result;
    if (global_cfg.stream_output) {
        result = run_llama_streaming(&global_cfg, prompt_buf.data, &output_buf);
    } else {
        result = run_llama_streaming(&global_cfg, prompt_buf.data, &output_buf);
    }
    
    if (result == 0 && output_buf.len > 0) {
        extract_clean_response(output_buf.data, &clean_response);
        
        if (clean_response.len > 0) {
            display_response_with_highlighting(clean_response.data);
            
            // Save to history
            if (history.count < MAX_HISTORY) {
                strncpy(history.responses[history.count], clean_response.data,
                       sizeof(history.responses[0]) - 1);
                history.count++;
            }
            
            // Parse and apply changes if configured
            if (global_cfg.apply_changes) {
                FileChange *changes;
                int num_changes;
                if (parse_file_changes(clean_response.data, &changes, &num_changes) == 0) {
                    update_status("Found file changes. Applying...", COLOR_HIGHLIGHT);
                    int applied = apply_file_changes(&global_cfg, changes, num_changes);
                    
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Applied %d/%d file changes", applied, num_changes);
                    update_status(msg, applied == num_changes ? COLOR_SUCCESS : COLOR_ERROR);
                    
                    // Free changes
                    for (int i = 0; i < num_changes; i++) {
                        free(changes[i].content);
                    }
                    free(changes);
                    
                    // Run tests if configured
                    if (global_cfg.run_tests && applied > 0) {
                        Buffer test_output;
                        buffer_init(&test_output);
                        int test_result = run_tests(&global_cfg, &test_output);
                        
                        display_response_with_highlighting(test_output.data);
                        update_status(test_result == 0 ? "Tests passed!" : "Tests failed!", 
                                    test_result == 0 ? COLOR_SUCCESS : COLOR_ERROR);
                        buffer_free(&test_output);
                    }
                } else {
                    update_status("Response generated (no file changes detected)", COLOR_SUCCESS);
                }
            } else {
                update_status("Response generated. Press 'a' to apply changes if any.", COLOR_SUCCESS);
            }
        } else {
            update_status("Warning: Empty response from model", COLOR_ERROR);
        }
    } else {
        update_status("Error: Failed to generate response", COLOR_ERROR);
    }
    
    buffer_free(&prompt_buf);
    buffer_free(&output_buf);
    buffer_free(&clean_response);
}

// Main loop
static void main_loop(void) {
    draw_config();
    update_status("Ready. Press ? for help", COLOR_HEADER);
    
    int ch;
    while (!should_exit) {
        ch = getch();
        
        switch (ch) {
            case 'q':
            case 'Q':
                should_exit = 1;
                break;
                
            case 'c
