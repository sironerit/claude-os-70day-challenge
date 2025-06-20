// ClaudeOS Demo Kernel - Stable Day 6 Base + Basic Shell
// Simplified version for demonstration without complex memory management

#include "kernel.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "serial.h"
#include "pmm.h"
#include "syscall_simple.h"
#include "../fs/memfs_simple.h"

// VGA Text Mode Constants
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

// Variable argument list support
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v,l) __builtin_va_arg(v,l)

// Global variables
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

// System information variables (Phase 4)
static uint32_t system_uptime_seconds = 0;

// VGA cursor management
void update_cursor(size_t x, size_t y) {
    uint16_t pos = y * VGA_WIDTH + x;
    
    // Tell the VGA board we are setting the high cursor byte
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    
    // Tell the VGA board we are setting the low cursor byte
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

// Advanced shell state
static char shell_buffer[256];
static int shell_pos = 0;

// Tab completion and aliases (Phase 4-3)
#define MAX_ALIASES 10
#define MAX_ALIAS_NAME 16
#define MAX_ALIAS_VALUE 64

typedef struct {
    char name[MAX_ALIAS_NAME];
    char value[MAX_ALIAS_VALUE];
    bool in_use;
} alias_t;

static alias_t aliases[MAX_ALIASES];
static bool aliases_initialized = false;

// Tab completion (simplified version)

// Command history (Phase 3)
#define HISTORY_SIZE 10
#define HISTORY_MAX_LEN 255
static char command_history[HISTORY_SIZE][HISTORY_MAX_LEN + 1];
static int history_count = 0;
static int history_current = -1;  // Current position in history (-1 = not browsing)

// Command parsing
#define MAX_ARGS 8
#define MAX_ARG_LEN 64
static char cmd_args[MAX_ARGS][MAX_ARG_LEN];
static int cmd_argc;

// VGA utility functions
static inline uint8_t vga_entry_color(vga_color fg, vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | (uint16_t) color << 8;
}

// Terminal functions
void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal_buffer = (uint16_t*) VGA_MEMORY;
    
    // Clear screen
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
    
    // Update hardware cursor
    update_cursor(terminal_column, terminal_row);
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_scroll(void) {
    // Move all lines up by one
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            size_t src_index = (y + 1) * VGA_WIDTH + x;
            size_t dst_index = y * VGA_WIDTH + x;
            terminal_buffer[dst_index] = terminal_buffer[src_index];
        }
    }
    
    // Clear the last line
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        size_t index = (VGA_HEIGHT - 1) * VGA_WIDTH + x;
        terminal_buffer[index] = vga_entry(' ', terminal_color);
    }
}

void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;
        }
        update_cursor(terminal_column, terminal_row);
        return;
    }
    
    if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
            terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
        }
        update_cursor(terminal_column, terminal_row);
        return;
    }
    
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;
        }
    }
    update_cursor(terminal_column, terminal_row);
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    size_t len = 0;
    while (data[len]) len++; // Simple strlen
    terminal_write(data, len);
}

void terminal_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
    terminal_column = 0;
    terminal_row = 0;
    update_cursor(terminal_column, terminal_row);
}

// String utility functions (moved up)
static size_t simple_strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

static int shell_strcmp(const char* str1, const char* str2) {
    while (*str1 && *str2 && *str1 == *str2) {
        str1++;
        str2++;
    }
    return *str1 - *str2;
}

// Command history functions (Phase 3)
static void simple_strcpy_safe(char* dest, const char* src, size_t max_len) {
    size_t i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

void add_to_history(const char* command) {
    if (!command || command[0] == '\0') return;
    
    // Check if command is same as last entry
    if (history_count > 0) {
        int last_idx = (history_count - 1) % HISTORY_SIZE;
        if (shell_strcmp(command_history[last_idx], command) == 0) {
            return; // Don't add duplicate
        }
    }
    
    // Add command to history
    int idx = history_count % HISTORY_SIZE;
    simple_strcpy_safe(command_history[idx], command, HISTORY_MAX_LEN + 1);
    history_count++;
}

const char* get_history_command(int direction) {
    if (history_count == 0) return NULL;
    
    int max_history = (history_count < HISTORY_SIZE) ? history_count : HISTORY_SIZE;
    
    if (direction > 0) { // Up arrow - go back in history
        if (history_current == -1) {
            history_current = history_count - 1;
        } else if (history_current > history_count - max_history) {
            history_current--;
        } else {
            return NULL; // At oldest command
        }
    } else { // Down arrow - go forward in history
        if (history_current == -1) {
            return NULL; // Not browsing history
        } else if (history_current < history_count - 1) {
            history_current++;
        } else {
            history_current = -1; // Back to current input
            return "";
        }
    }
    
    if (history_current >= 0) {
        int idx = history_current % HISTORY_SIZE;
        return command_history[idx];
    }
    
    return NULL;
}

void reset_history_position(void) {
    history_current = -1;
}

// System information functions (Phase 4)
void update_uptime(void) {
    system_uptime_seconds++;
}

void format_uptime(uint32_t uptime_seconds, char* buffer, size_t size) {
    if (!buffer || size < 20) return;
    
    uint32_t hours = uptime_seconds / 3600;
    uint32_t minutes = (uptime_seconds % 3600) / 60;
    uint32_t seconds = uptime_seconds % 60;
    
    // Format as "HH:MM:SS"
    int pos = 0;
    
    // Hours
    if (hours >= 10) {
        buffer[pos++] = '0' + (hours / 10);
    } else {
        buffer[pos++] = '0';
    }
    buffer[pos++] = '0' + (hours % 10);
    buffer[pos++] = ':';
    
    // Minutes
    buffer[pos++] = '0' + (minutes / 10);
    buffer[pos++] = '0' + (minutes % 10);
    buffer[pos++] = ':';
    
    // Seconds
    buffer[pos++] = '0' + (seconds / 10);
    buffer[pos++] = '0' + (seconds % 10);
    buffer[pos] = '\0';
}

void display_system_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("ClaudeOS System Information:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    // OS Version
    terminal_writestring("  OS: ClaudeOS Day 11 Phase 4\n");
    terminal_writestring("  Version: Complete Integrated System v1.1\n");
    terminal_writestring("  Architecture: x86 32-bit\n");
    
    // Uptime
    char uptime_str[20];
    format_uptime(system_uptime_seconds, uptime_str, sizeof(uptime_str));
    terminal_writestring("  Uptime: ");
    terminal_writestring(uptime_str);
    terminal_writestring("\n");
    
    // Memory information
    terminal_writestring("  Memory:\n");
    pmm_dump_stats();
    
    // File system information
    terminal_writestring("  File System:\n");
    memfs_simple_dump_stats();
    
    terminal_writestring("\n");
}

void display_uptime_info(void) {
    char uptime_str[20];
    format_uptime(system_uptime_seconds, uptime_str, sizeof(uptime_str));
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("System uptime: ");
    terminal_writestring(uptime_str);
    terminal_writestring("\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

void display_process_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("ClaudeOS Process Information (simplified):\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    terminal_writestring("  PID  NAME           STATE    MEMORY\n");
    terminal_writestring("  ---  ----           -----    ------\n");
    terminal_writestring("   0   kernel         running  active\n");
    terminal_writestring("   1   shell          running  active\n");
    terminal_writestring("   2   timer          running  active\n");
    terminal_writestring("   3   keyboard       running  active\n");
    
    terminal_writestring("\n");
    terminal_writestring("  Total processes: 4 (system)\n");
    terminal_writestring("  Memory usage: kernel space only\n");
    terminal_writestring("  Scheduler: cooperative (single-threaded)\n");
    terminal_writestring("\n");
}

// Advanced file system functions (Phase 4-2)
const char* detect_file_type(const char* filename, const char* content, size_t content_size) {
    if (!filename || !content) return "unknown";
    
    // Check file extension
    size_t name_len = simple_strlen(filename);
    if (name_len < 3) return "data";
    
    // Simple file type detection by extension
    if (name_len >= 4) {
        const char* ext = filename + name_len - 4;
        if (shell_strcmp(ext, ".txt") == 0) return "text file";
        if (shell_strcmp(ext, ".cfg") == 0) return "configuration file";
        if (shell_strcmp(ext, ".log") == 0) return "log file";
        if (shell_strcmp(ext, ".dat") == 0) return "data file";
    }
    
    if (name_len >= 3) {
        const char* ext = filename + name_len - 3;
        if (shell_strcmp(ext, ".md") == 0) return "markdown file";
        if (shell_strcmp(ext, ".sh") == 0) return "shell script";
    }
    
    // Content-based detection (simple)
    if (content_size == 0) return "empty file";
    
    // Check if content appears to be text
    bool is_text = true;
    for (size_t i = 0; i < content_size && i < 100; i++) {
        char c = content[i];
        if (c != '\n' && c != '\t' && (c < 32 || c > 126)) {
            is_text = false;
            break;
        }
    }
    
    return is_text ? "text file" : "binary data";
}

void display_file_info(const char* filename) {
    if (!filename) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
        terminal_writestring("Usage: file <filename>\n");
        terminal_writestring("Example: file hello.txt\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    int index = memfs_simple_find_file(filename);
    if (index < 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("File not found: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    // Read file content for analysis
    char buffer[256];
    int size = memfs_simple_read(filename, buffer, sizeof(buffer));
    if (size < 0) size = 0;
    
    const char* file_type = detect_file_type(filename, buffer, size);
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    terminal_writestring(filename);
    terminal_writestring(": ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring(file_type);
    
    // Additional info
    terminal_writestring(" (");
    char size_str[16];
    uint32_t file_size = (size >= 0) ? (uint32_t)size : 0;
    int pos = 0;
    if (file_size == 0) {
        size_str[pos++] = '0';
    } else {
        while (file_size > 0) {
            size_str[pos++] = '0' + (file_size % 10);
            file_size /= 10;
        }
    }
    for (int i = 0; i < pos / 2; i++) {
        char temp = size_str[i];
        size_str[i] = size_str[pos - 1 - i];
        size_str[pos - 1 - i] = temp;
    }
    size_str[pos] = '\0';
    
    terminal_writestring(size_str);
    terminal_writestring(" bytes)\n");
}

void count_file_stats(const char* filename) {
    if (!filename) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
        terminal_writestring("Usage: wc <filename>\n");
        terminal_writestring("Example: wc hello.txt\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    char buffer[4096];  // Use larger buffer for wc
    int size = memfs_simple_read(filename, buffer, sizeof(buffer));
    if (size < 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("File not found: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    // Count lines, words, and characters
    uint32_t lines = 0;
    uint32_t words = 0;
    uint32_t chars = size;
    bool in_word = false;
    
    for (int i = 0; i < size; i++) {
        char c = buffer[i];
        
        if (c == '\n') {
            lines++;
        }
        
        if (c == ' ' || c == '\t' || c == '\n') {
            if (in_word) {
                words++;
                in_word = false;
            }
        } else {
            in_word = true;
        }
    }
    
    // Add final word if file doesn't end with whitespace
    if (in_word) {
        words++;
    }
    
    // Display results
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    // Lines
    char num_str[16];
    uint32_t num = lines;
    int pos = 0;
    if (num == 0) {
        num_str[pos++] = '0';
    } else {
        while (num > 0) {
            num_str[pos++] = '0' + (num % 10);
            num /= 10;
        }
    }
    for (int i = 0; i < pos / 2; i++) {
        char temp = num_str[i];
        num_str[i] = num_str[pos - 1 - i];
        num_str[pos - 1 - i] = temp;
    }
    num_str[pos] = '\0';
    
    terminal_writestring("  ");
    terminal_writestring(num_str);
    
    // Words
    num = words;
    pos = 0;
    if (num == 0) {
        num_str[pos++] = '0';
    } else {
        while (num > 0) {
            num_str[pos++] = '0' + (num % 10);
            num /= 10;
        }
    }
    for (int i = 0; i < pos / 2; i++) {
        char temp = num_str[i];
        num_str[i] = num_str[pos - 1 - i];
        num_str[pos - 1 - i] = temp;
    }
    num_str[pos] = '\0';
    
    terminal_writestring("  ");
    terminal_writestring(num_str);
    
    // Characters
    num = chars;
    pos = 0;
    if (num == 0) {
        num_str[pos++] = '0';
    } else {
        while (num > 0) {
            num_str[pos++] = '0' + (num % 10);
            num /= 10;
        }
    }
    for (int i = 0; i < pos / 2; i++) {
        char temp = num_str[i];
        num_str[i] = num_str[pos - 1 - i];
        num_str[pos - 1 - i] = temp;
    }
    num_str[pos] = '\0';
    
    terminal_writestring("  ");
    terminal_writestring(num_str);
    terminal_writestring("  ");
    terminal_writestring(filename);
    terminal_writestring("\n");
}

void search_in_file(const char* pattern, const char* filename) {
    if (!pattern || !filename) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
        terminal_writestring("Usage: grep <pattern> <filename>\n");
        terminal_writestring("Example: grep ClaudeOS hello.txt\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    char buffer[4096];  // Use larger buffer for grep
    int size = memfs_simple_read(filename, buffer, sizeof(buffer));
    if (size < 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("File not found: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[GREP] Searching for \"");
    terminal_writestring(pattern);
    terminal_writestring("\" in ");
    terminal_writestring(filename);
    terminal_writestring(":\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    size_t pattern_len = simple_strlen(pattern);
    uint32_t line_num = 1;
    int line_start = 0;
    int matches = 0;
    
    for (int i = 0; i <= size; i++) {
        // End of line or end of file
        if (i == size || buffer[i] == '\n') {
            // Check if this line contains the pattern
            bool line_matches = false;
            
            for (int j = line_start; j <= i - (int)pattern_len; j++) {
                bool match = true;
                for (size_t k = 0; k < pattern_len; k++) {
                    if (buffer[j + k] != pattern[k]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    line_matches = true;
                    break;
                }
            }
            
            if (line_matches) {
                matches++;
                
                // Display line number
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                char line_str[8];
                uint32_t num = line_num;
                int pos = 0;
                if (num == 0) {
                    line_str[pos++] = '0';
                } else {
                    while (num > 0) {
                        line_str[pos++] = '0' + (num % 10);
                        num /= 10;
                    }
                }
                for (int idx = 0; idx < pos / 2; idx++) {
                    char temp = line_str[idx];
                    line_str[idx] = line_str[pos - 1 - idx];
                    line_str[pos - 1 - idx] = temp;
                }
                line_str[pos] = '\0';
                
                terminal_writestring(line_str);
                terminal_writestring(": ");
                
                // Display line content
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
                for (int j = line_start; j < i; j++) {
                    char c = buffer[j];
                    if (c >= 32 && c <= 126) {
                        terminal_putchar(c);
                    }
                }
                terminal_writestring("\n");
            }
            
            line_num++;
            line_start = i + 1;
        }
    }
    
    // Summary
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    if (matches == 0) {
        terminal_writestring("No matches found.\n");
    } else {
        terminal_writestring("Found ");
        char match_str[8];
        uint32_t num = matches;
        int pos = 0;
        if (num == 0) {
            match_str[pos++] = '0';
        } else {
            while (num > 0) {
                match_str[pos++] = '0' + (num % 10);
                num /= 10;
            }
        }
        for (int i = 0; i < pos / 2; i++) {
            char temp = match_str[i];
            match_str[i] = match_str[pos - 1 - i];
            match_str[pos - 1 - i] = temp;
        }
        match_str[pos] = '\0';
        
        terminal_writestring(match_str);
        terminal_writestring(" matching line(s).\n");
    }
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// Tab completion and alias functions (Phase 4-3)
void init_aliases(void) {
    if (aliases_initialized) return;
    
    // Clear aliases
    for (int i = 0; i < MAX_ALIASES; i++) {
        aliases[i].in_use = false;
        aliases[i].name[0] = '\0';
        aliases[i].value[0] = '\0';
    }
    
    // Add default aliases
    simple_strcpy_safe(aliases[0].name, "ll", MAX_ALIAS_NAME);
    simple_strcpy_safe(aliases[0].value, "ls -l", MAX_ALIAS_VALUE);
    aliases[0].in_use = true;
    
    simple_strcpy_safe(aliases[1].name, "h", MAX_ALIAS_NAME);
    simple_strcpy_safe(aliases[1].value, "history", MAX_ALIAS_VALUE);
    aliases[1].in_use = true;
    
    simple_strcpy_safe(aliases[2].name, "c", MAX_ALIAS_NAME);
    simple_strcpy_safe(aliases[2].value, "clear", MAX_ALIAS_VALUE);
    aliases[2].in_use = true;
    
    simple_strcpy_safe(aliases[3].name, "info", MAX_ALIAS_NAME);
    simple_strcpy_safe(aliases[3].value, "sysinfo", MAX_ALIAS_VALUE);
    aliases[3].in_use = true;
    
    aliases_initialized = true;
}

const char* expand_alias(const char* command) {
    if (!command) return NULL;
    
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (aliases[i].in_use && shell_strcmp(aliases[i].name, command) == 0) {
            return aliases[i].value;
        }
    }
    return command;  // Return original if no alias found
}

void list_aliases(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Active aliases:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    bool found = false;
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (aliases[i].in_use) {
            terminal_writestring("  ");
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring(aliases[i].name);
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            terminal_writestring(" = ");
            terminal_writestring(aliases[i].value);
            terminal_writestring("\n");
            found = true;
        }
    }
    
    if (!found) {
        terminal_writestring("  No aliases defined\n");
    }
}

const char* tab_complete_command(const char* partial) {
    if (!partial) return NULL;
    
    size_t partial_len = simple_strlen(partial);
    if (partial_len == 0) return NULL;
    
    // List of available commands (ordered by frequency/priority)
    const char* commands[] = {
        "help", "clear", "version", "hello", "demo", "meminfo", "sysinfo",
        "ls", "cat", "create", "delete", "write", "mkdir", "rmdir", "cd", "pwd",
        "touch", "cp", "mv", "find", "history", "fsinfo", "uptime", "syscalls",
        "top", "file", "wc", "grep", "alias", NULL
    };
    
    const char* match = NULL;
    int match_count = 0;
    
    // Find matching commands
    for (int i = 0; commands[i] != NULL; i++) {
        bool matches = true;
        for (size_t j = 0; j < partial_len; j++) {
            if (commands[i][j] != partial[j]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            if (match_count == 0) {
                match = commands[i];  // Store first match
            }
            match_count++;
        }
    }
    
    // Return first match if any found
    return match;
}

void handle_tab_completion(void) {
    if (shell_pos == 0) return;
    
    // Find the current word being typed
    int word_start = shell_pos - 1;
    while (word_start > 0 && shell_buffer[word_start - 1] != ' ') {
        word_start--;
    }
    
    // Extract the current word
    char current_word[64];
    int word_len = shell_pos - word_start;
    if (word_len >= 63) return;  // Word too long
    
    for (int i = 0; i < word_len; i++) {
        current_word[i] = shell_buffer[word_start + i];
    }
    current_word[word_len] = '\0';
    
    // Try to complete as command (if it's the first word)
    if (word_start == 0) {
        const char* completion = tab_complete_command(current_word);
        if (completion) {
            // Clear current word and replace with completion
            for (int i = 0; i < word_len; i++) {
                terminal_putchar('\b');
            }
            
            shell_pos = word_start;
            size_t completion_len = simple_strlen(completion);
            for (size_t i = 0; i < completion_len && shell_pos < 255; i++) {
                shell_buffer[shell_pos] = completion[i];
                terminal_putchar(completion[i]);
                shell_pos++;
            }
            shell_buffer[shell_pos] = '\0';
        } else {
            // No completion found - show temporary visual feedback
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring(" [no match]");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            
            // Wait briefly then clear the message
            for (volatile int i = 0; i < 1000000; i++);
            for (int i = 0; i < 11; i++) {
                terminal_putchar('\b');
            }
        }
    }
}

// Simple shell functions
void shell_print_prompt(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("claudeos> ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// Parse command line into arguments
void parse_command_line(const char* cmdline) {
    cmd_argc = 0;
    int i = 0;
    int arg_pos = 0;
    bool in_arg = false;
    
    // Clear previous arguments
    for (int j = 0; j < MAX_ARGS; j++) {
        cmd_args[j][0] = '\0';
    }
    
    while (cmdline[i] && cmd_argc < MAX_ARGS) {
        if (cmdline[i] == ' ' || cmdline[i] == '\t') {
            if (in_arg) {
                cmd_args[cmd_argc][arg_pos] = '\0';
                cmd_argc++;
                arg_pos = 0;
                in_arg = false;
            }
        } else {
            if (!in_arg) {
                in_arg = true;
            }
            if (arg_pos < MAX_ARG_LEN - 1) {
                cmd_args[cmd_argc][arg_pos] = cmdline[i];
                arg_pos++;
            }
        }
        i++;
    }
    
    // Add final argument if exists
    if (in_arg && cmd_argc < MAX_ARGS) {
        cmd_args[cmd_argc][arg_pos] = '\0';
        cmd_argc++;
    }
}

// Clear current line and redraw
void clear_current_line(void) {
    // Move cursor to beginning of current line after prompt
    terminal_column = 10; // Length of "claudeos> "
    // Clear to end of line
    for (int i = 0; i < shell_pos; i++) {
        terminal_putchar(' ');
    }
    terminal_column = 10;
    update_cursor(terminal_column, terminal_row);
}

void display_command(const char* cmd) {
    if (!cmd) return;
    
    // Clear current input
    clear_current_line();
    
    // Copy command to buffer and display
    shell_pos = 0;
    while (cmd[shell_pos] && shell_pos < 255) {
        shell_buffer[shell_pos] = cmd[shell_pos];
        terminal_putchar(cmd[shell_pos]);
        shell_pos++;
    }
    shell_buffer[shell_pos] = '\0';
}

void shell_process_command(const char* cmd) {
    // Parse command line into arguments
    parse_command_line(cmd);
    
    if (cmd_argc == 0) return;
    
    // Expand aliases for the first argument (command)
    const char* expanded_cmd = expand_alias(cmd_args[0]);
    if (expanded_cmd != cmd_args[0]) {
        // Alias was expanded, re-parse the expanded command
        parse_command_line(expanded_cmd);
        if (cmd_argc == 0) return;
    }
    
    if (shell_strcmp(cmd_args[0], "help") == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
        terminal_writestring("ClaudeOS Demo Shell - Available Commands:\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        terminal_writestring("  help     - Show this help\n");
        terminal_writestring("  clear    - Clear screen\n");
        terminal_writestring("  version  - Show version\n");
        terminal_writestring("  hello    - Say hello\n");
        terminal_writestring("  demo     - Demo message\n");
        terminal_writestring("  meminfo  - Show memory statistics\n");
        terminal_writestring("  syscalls - Test system calls\n");
        terminal_writestring("  ls       - List files\n");
        terminal_writestring("  ls -l    - List files with details\n");
        terminal_writestring("  cat <file> - Display file content\n");
        terminal_writestring("  create <file> - Create new file\n");
        terminal_writestring("  delete <file> - Delete file\n");
        terminal_writestring("  write <file> <text> - Write to file\n");
        terminal_writestring("  mkdir <dir> - Create directory\n");
        terminal_writestring("  rmdir <dir> - Remove directory\n");
        terminal_writestring("  cd <dir> - Change directory\n");
        terminal_writestring("  pwd      - Show current directory\n");
        terminal_writestring("  touch <file> - Create/update file timestamp\n");
        terminal_writestring("  cp <src> <dst> - Copy file\n");
        terminal_writestring("  mv <src> <dst> - Move/rename file\n");
        terminal_writestring("  find <name> - Search for files\n");
        terminal_writestring("  history  - Show command history\n");
        terminal_writestring("  fsinfo   - File system statistics\n");
        terminal_writestring("  sysinfo  - Complete system information\n");
        terminal_writestring("  uptime   - System uptime\n");
        terminal_writestring("  top      - Process information\n");
        terminal_writestring("  file <name> - File type detection\n");
        terminal_writestring("  wc <file> - Count lines, words, characters\n");
        terminal_writestring("  grep <pattern> <file> - Search in file\n");
        terminal_writestring("  alias    - Show active aliases\n");
        terminal_writestring("\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
        terminal_writestring("Navigation & Features:\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        terminal_writestring("  Ctrl+P   - Previous command (up arrow)\n");
        terminal_writestring("  Ctrl+N   - Next command (down arrow)\n");
        terminal_writestring("  Tab      - Command completion\n");
        terminal_writestring("  Aliases  - ll, h, c, info (shortcuts)\n\n");
    } else if (shell_strcmp(cmd_args[0], "clear") == 0) {
        terminal_clear();
        // Don't print prompt here - let the main loop handle it
        return;
    } else if (shell_strcmp(cmd_args[0], "version") == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
        terminal_writestring("ClaudeOS Day 11 Phase 4 - Complete Integrated System v1.1\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        terminal_writestring("Enhanced with command history, system monitoring, and advanced operations\n");
    } else if (shell_strcmp(cmd_args[0], "hello") == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
        terminal_writestring("Hello from ClaudeOS Shell!\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    } else if (shell_strcmp(cmd_args[0], "demo") == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK));
        terminal_writestring("Demo: Advanced shell with argument parsing!\n");
        terminal_writestring("Day 10 functionality working!\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    } else if (shell_strcmp(cmd_args[0], "meminfo") == 0) {
        pmm_dump_stats();
    } else if (shell_strcmp(cmd_args[0], "syscalls") == 0) {
        test_syscalls();
    } else if (shell_strcmp(cmd_args[0], "ls") == 0) {
        if (cmd_argc > 1 && shell_strcmp(cmd_args[1], "-l") == 0) {
            memfs_simple_list_detailed();
        } else {
            memfs_simple_list_files();
        }
    } else if (shell_strcmp(cmd_args[0], "cat") == 0) {
        if (cmd_argc < 2) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: cat <filename>\n");
            terminal_writestring("Available files: hello.txt, readme.md, test.txt\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
            terminal_writestring("Displaying ");
            terminal_writestring(cmd_args[1]);
            
            // Debug: show file size
            int file_size = memfs_simple_get_size(cmd_args[1]);
            terminal_writestring(" (");
            char size_str[16];
            uint32_t size = (file_size > 0) ? (uint32_t)file_size : 0;
            int pos = 0;
            if (size == 0) {
                size_str[pos++] = '0';
            } else {
                while (size > 0) {
                    size_str[pos++] = '0' + (size % 10);
                    size /= 10;
                }
            }
            for (int i = 0; i < pos / 2; i++) {
                char temp = size_str[i];
                size_str[i] = size_str[pos - 1 - i];
                size_str[pos - 1 - i] = temp;
            }
            size_str[pos] = '\0';
            terminal_writestring(size_str);
            terminal_writestring(" bytes):\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            
            char buffer[256];
            // Clear buffer first
            for (int i = 0; i < 256; i++) {
                buffer[i] = '\0';
            }
            
            int result = memfs_simple_read(cmd_args[1], buffer, 256);
            if (result > 0) {
                // Process and display the content
                for (int i = 0; i < result; i++) {
                    char c = buffer[i];
                    if (c == '\0') break;  // Stop at null terminator
                    if (c == '\n') {
                        terminal_putchar('\n');
                    } else if (c >= 32 && c <= 126) {  // Printable ASCII only
                        terminal_putchar(c);
                    }
                }
                terminal_putchar('\n');
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("File not found or read error\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "create") == 0) {
        if (cmd_argc < 2) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: create <filename>\n");
            terminal_writestring("Example: create myfile.txt\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
            terminal_writestring("Creating file: ");
            terminal_writestring(cmd_args[1]);
            terminal_writestring("\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            
            int result = memfs_simple_create(cmd_args[1]);
            if (result == MEMFS_SUCCESS) {
                memfs_simple_write(cmd_args[1], "This is a newly created file!\nDay 10 Advanced Shell working!");
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("File created successfully!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_EXISTS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("File already exists!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to create file\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "delete") == 0) {
        if (cmd_argc < 2) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: delete <filename>\n");
            terminal_writestring("Example: delete test.txt\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
            terminal_writestring("Deleting file: ");
            terminal_writestring(cmd_args[1]);
            terminal_writestring("\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            
            int result = memfs_simple_delete(cmd_args[1]);
            if (result == MEMFS_SUCCESS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("File deleted successfully!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_NOT_FOUND) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("File not found!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to delete file\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "write") == 0) {
        if (cmd_argc < 3) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: write <filename> <text>\n");
            terminal_writestring("Example: write myfile.txt Hello World\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            // Combine all arguments after filename into content
            char content[256] = {0};
            int content_pos = 0;
            for (int i = 2; i < cmd_argc && content_pos < 250; i++) {
                if (i > 2) {
                    content[content_pos++] = ' ';
                }
                for (int j = 0; cmd_args[i][j] && content_pos < 250; j++) {
                    content[content_pos++] = cmd_args[i][j];
                }
            }
            content[content_pos] = '\0';
            
            terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
            terminal_writestring("Writing to file: ");
            terminal_writestring(cmd_args[1]);
            terminal_writestring("\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            
            int result = memfs_simple_write(cmd_args[1], content);
            if (result > 0) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("Content written successfully!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to write to file\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "mkdir") == 0) {
        if (cmd_argc < 2) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: mkdir <dirname>\n");
            terminal_writestring("Example: mkdir documents\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
            terminal_writestring("Creating directory: ");
            terminal_writestring(cmd_args[1]);
            terminal_writestring("\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            
            int result = memfs_simple_mkdir(cmd_args[1]);
            if (result == MEMFS_SUCCESS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("Directory created successfully!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_EXISTS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Directory already exists!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to create directory\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "rmdir") == 0) {
        if (cmd_argc < 2) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: rmdir <dirname>\n");
            terminal_writestring("Example: rmdir documents\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
            terminal_writestring("Removing directory: ");
            terminal_writestring(cmd_args[1]);
            terminal_writestring("\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            
            int result = memfs_simple_rmdir(cmd_args[1]);
            if (result == MEMFS_SUCCESS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("Directory removed successfully!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_NOT_FOUND) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Directory not found!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_NOT_DIR) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Not a directory!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to remove directory (not empty?)\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "cd") == 0) {
        if (cmd_argc < 2) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: cd <dirname>\n");
            terminal_writestring("Special: cd .. (parent), cd / (root)\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            int result = memfs_simple_chdir(cmd_args[1]);
            if (result == MEMFS_SUCCESS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("Changed directory successfully\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_NOT_FOUND) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Directory not found!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_NOT_DIR) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Not a directory!\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to change directory\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "pwd") == 0) {
        char cwd[MEMFS_MAX_PATH];
        memfs_simple_getcwd(cwd, MEMFS_MAX_PATH);
        terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
        terminal_writestring("Current directory: ");
        terminal_writestring(cwd);
        terminal_writestring("\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    } else if (shell_strcmp(cmd_args[0], "touch") == 0) {
        if (cmd_argc < 2) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: touch <filename>\n");
            terminal_writestring("Example: touch newfile.txt\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            int result = memfs_simple_touch(cmd_args[1]);
            if (result == MEMFS_SUCCESS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("File touched successfully\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to touch file\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "cp") == 0) {
        if (cmd_argc < 3) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: cp <source> <destination>\n");
            terminal_writestring("Example: cp hello.txt backup.txt\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            int result = memfs_simple_copy(cmd_args[1], cmd_args[2]);
            if (result == MEMFS_SUCCESS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("File copied successfully\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_NOT_FOUND) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Source file not found\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_EXISTS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Destination file already exists\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to copy file\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "mv") == 0) {
        if (cmd_argc < 3) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: mv <source> <destination>\n");
            terminal_writestring("Example: mv oldname.txt newname.txt\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            int result = memfs_simple_move(cmd_args[1], cmd_args[2]);
            if (result == MEMFS_SUCCESS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("File moved/renamed successfully\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_NOT_FOUND) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Source file not found\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else if (result == MEMFS_EXISTS) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
                terminal_writestring("Destination file already exists\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            } else {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("Failed to move file\n");
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
            }
        }
    } else if (shell_strcmp(cmd_args[0], "find") == 0) {
        if (cmd_argc < 2) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
            terminal_writestring("Usage: find <filename>\n");
            terminal_writestring("Example: find hello.txt\n");
            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        } else {
            memfs_simple_find(cmd_args[1]);
        }
    } else if (shell_strcmp(cmd_args[0], "history") == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
        terminal_writestring("Command History:\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        
        if (history_count == 0) {
            terminal_writestring("  (no commands in history)\n");
        } else {
            int max_history = (history_count < HISTORY_SIZE) ? history_count : HISTORY_SIZE;
            int start_idx = (history_count > HISTORY_SIZE) ? history_count - HISTORY_SIZE : 0;
            
            for (int i = 0; i < max_history; i++) {
                int hist_num = start_idx + i + 1;
                int idx = (start_idx + i) % HISTORY_SIZE;
                
                terminal_writestring("  ");
                
                // Print history number
                char num_str[8];
                uint32_t num = hist_num;
                int pos = 0;
                if (num == 0) {
                    num_str[pos++] = '0';
                } else {
                    while (num > 0) {
                        num_str[pos++] = '0' + (num % 10);
                        num /= 10;
                    }
                }
                for (int j = 0; j < pos / 2; j++) {
                    char temp = num_str[j];
                    num_str[j] = num_str[pos - 1 - j];
                    num_str[pos - 1 - j] = temp;
                }
                num_str[pos] = '\0';
                
                terminal_writestring(num_str);
                terminal_writestring(": ");
                terminal_writestring(command_history[idx]);
                terminal_writestring("\n");
            }
        }
    } else if (shell_strcmp(cmd_args[0], "fsinfo") == 0) {
        memfs_simple_dump_stats();
    } else if (shell_strcmp(cmd_args[0], "sysinfo") == 0) {
        display_system_info();
    } else if (shell_strcmp(cmd_args[0], "uptime") == 0) {
        display_uptime_info();
    } else if (shell_strcmp(cmd_args[0], "top") == 0) {
        display_process_info();
    } else if (shell_strcmp(cmd_args[0], "file") == 0) {
        if (cmd_argc < 2) {
            display_file_info(NULL);  // Show usage
        } else {
            display_file_info(cmd_args[1]);
        }
    } else if (shell_strcmp(cmd_args[0], "wc") == 0) {
        if (cmd_argc < 2) {
            count_file_stats(NULL);  // Show usage
        } else {
            count_file_stats(cmd_args[1]);
        }
    } else if (shell_strcmp(cmd_args[0], "grep") == 0) {
        if (cmd_argc < 3) {
            search_in_file(NULL, NULL);  // Show usage
        } else {
            search_in_file(cmd_args[1], cmd_args[2]);
        }
    } else if (shell_strcmp(cmd_args[0], "alias") == 0) {
        list_aliases();
    } else if (cmd_argc > 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("Command not found: ");
        terminal_writestring(cmd_args[0]);
        terminal_writestring("\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    }
}

// Main kernel entry point
void kernel_main(void) {
    // Initialize terminal
    terminal_initialize();
    
    // Display welcome message
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("ClaudeOS Day 11 Phase 4 - Complete Integrated System\n");
    terminal_writestring("=====================================================\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Phase 4: Advanced System Monitoring + Command History + File Operations\n\n");
    
    // Initialize basic systems
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("Initializing systems...\n");
    
    gdt_init();
    terminal_writestring("GDT: OK\n");
    
    idt_init();
    terminal_writestring("IDT: OK\n");
    
    pic_init();
    terminal_writestring("PIC: OK\n");
    
    timer_init();
    terminal_writestring("Timer: OK\n");
    
    keyboard_init();
    terminal_writestring("Keyboard: OK\n");
    
    if (serial_init(SERIAL_COM1_BASE) == 0) {
        terminal_writestring("Serial: OK\n");
    }
    
    pmm_init();
    terminal_writestring("PMM: OK\n");
    
    syscall_simple_init();
    terminal_writestring("Syscalls: OK\n");
    
    memfs_simple_init();
    terminal_writestring("MemFS: OK\n");
    
    init_aliases();
    terminal_writestring("Aliases: OK\n");
    
    // Enable interrupts
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Enabling interrupts...\n");
    asm volatile ("sti");
    terminal_writestring("All systems ready!\n\n");
    
    // Start shell
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Type 'help' for available commands.\n\n");
    shell_print_prompt();
    
    // Main shell loop
    while (1) {
        asm volatile ("hlt");
        
        char c = keyboard_get_char();
        if (c != 0) {
            if (c == '\n') {
                terminal_putchar('\n');
                shell_buffer[shell_pos] = '\0';
                
                if (shell_pos > 0) {
                    add_to_history(shell_buffer);  // Add to history
                    shell_process_command(shell_buffer);
                }
                
                shell_pos = 0;
                shell_buffer[0] = '\0';
                reset_history_position();  // Reset history browsing
                shell_print_prompt();
                
            } else if (c == '\b') {
                if (shell_pos > 0) {
                    shell_pos--;
                    shell_buffer[shell_pos] = '\0';
                    terminal_putchar('\b');
                }
                reset_history_position();  // Reset when user types
            } else if (c == 0x1B) {  // ESC sequence for arrow keys
                // Note: This is simplified - real arrow key detection needs escape sequence parsing
                // For now, we'll use Ctrl+P (up) and Ctrl+N (down) as alternatives
            } else if (c == 0x10) {  // Ctrl+P (up arrow alternative)
                const char* hist_cmd = get_history_command(1);
                if (hist_cmd) {
                    display_command(hist_cmd);
                }
            } else if (c == 0x0E) {  // Ctrl+N (down arrow alternative)
                const char* hist_cmd = get_history_command(-1);
                if (hist_cmd) {
                    if (hist_cmd[0] == '\0') {
                        // Empty string means back to current input
                        clear_current_line();
                        shell_pos = 0;
                        shell_buffer[0] = '\0';
                    } else {
                        display_command(hist_cmd);
                    }
                }
            } else if (c == '\t') {  // Tab key
                handle_tab_completion();
                reset_history_position();  // Reset when user uses tab
            } else if (c >= 32 && c <= 126) {
                if (shell_pos < 255) {
                    shell_buffer[shell_pos] = c;
                    shell_pos++;
                    terminal_putchar(c);
                }
                reset_history_position();  // Reset when user types
            }
        }
    }
}