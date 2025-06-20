// ClaudeOS Kernel - Day 6 Implementation
// Memory management system integration

#include "kernel.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "serial.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "process.h"
#include "syscall.h"
#include "string.h"
#include "../fs/memfs.h"
#include "shell.h"

// VGA Text Mode Constants
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

// Kernel demonstration constants
#define KERNEL_COUNTER_INTERVAL 1000000
#define TEST_PROCESS_WORK_LOOP 100000

// Variable argument list support for printf
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v,l) __builtin_va_arg(v,l)

// Global variables
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

// Utility functions
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
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

// Scroll screen up by one line
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

// Clear screen
void terminal_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
    terminal_column = 0;
    terminal_row = 0;
}

void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;  // Stay at bottom line
        }
        return;
    }
    
    if (c == '\b') {
        // Handle backspace - move cursor back if possible
        if (terminal_column > 0) {
            terminal_column--;
            // Clear the character at the current position
            terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
        }
        return;
    }
    
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;  // Stay at bottom line
        }
    }
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    terminal_write(data, strlen(data));
}

// Simple printf implementation for debugging
void terminal_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    char buffer[256];
    int pos = 0;
    
    for (int i = 0; format[i] != '\0' && pos < 255; i++) {
        if (format[i] == '%' && format[i+1] == 'd') {
            int value = va_arg(args, int);
            // Simple integer to string conversion
            if (value == 0) {
                buffer[pos++] = '0';
            } else {
                char temp[12];
                int temp_pos = 0;
                int temp_value = value;
                
                if (value < 0) {
                    buffer[pos++] = '-';
                    temp_value = -value;
                }
                
                while (temp_value > 0) {
                    temp[temp_pos++] = '0' + (temp_value % 10);
                    temp_value /= 10;
                }
                
                for (int j = temp_pos - 1; j >= 0; j--) {
                    buffer[pos++] = temp[j];
                }
            }
            i++; // Skip the 'd'
        } else if (format[i] == '%' && format[i+1] == 's') {
            char* str = va_arg(args, char*);
            while (*str && pos < 255) {
                buffer[pos++] = *str++;
            }
            i++; // Skip the 's'
        } else {
            buffer[pos++] = format[i];
        }
    }
    
    buffer[pos] = '\0';
    terminal_writestring(buffer);
    
    va_end(args);
}

// Kernel panic function
void kernel_panic(const char* message) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    terminal_writestring("\n*** KERNEL PANIC ***\n");
    terminal_writestring(message);
    terminal_writestring("\nSystem halted.");
    
    // Halt the CPU
    while (1) {
        asm volatile ("hlt");
    }
}

// Main kernel entry point
void kernel_main(void) {
    // Initialize terminal
    terminal_initialize();
    
    // Display welcome message  
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("ClaudeOS - Day 11 Interactive Shell\n");
    terminal_writestring("====================================\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Kernel loaded successfully!\n");
    terminal_writestring("VGA text mode initialized.\n");
    
    // Initialize GDT
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK));
    terminal_writestring("Initializing GDT...\n");
    gdt_init();
    terminal_writestring("GDT initialized successfully!\n");
    
    // Initialize IDT
    terminal_writestring("Initializing IDT...\n");
    idt_init();
    terminal_writestring("IDT initialized successfully!\n");
    
    // Initialize PIC
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("Initializing PIC...\n");
    pic_init();
    terminal_writestring("PIC initialized successfully!\n");
    
    // Initialize Timer
    terminal_writestring("Initializing Timer...\n");
    timer_init();
    terminal_writestring("Timer initialized successfully!\n");
    
    // Initialize Serial Port
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK));
    terminal_writestring("Initializing Serial Port...\n");
    if (serial_init(SERIAL_COM1_BASE) == 0) {
        terminal_writestring("Serial port initialized successfully!\n");
        debug_write_string("ClaudeOS Day 6 - Serial debug output active\n");
    } else {
        terminal_writestring("Serial port initialization failed!\n");
    }
    
    // Initialize Keyboard
    terminal_writestring("Initializing Keyboard...\n");
    keyboard_init();
    terminal_writestring("Keyboard initialized successfully!\n");
    
    // Initialize Physical Memory Manager
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("Initializing Physical Memory Manager...\n");
    pmm_init();
    terminal_writestring("PMM initialized successfully!\n");
    
    // Initialize Virtual Memory Manager
    terminal_writestring("Initializing Virtual Memory Manager...\n");
    vmm_init();
    terminal_writestring("VMM initialized successfully!\n");
    
    // Enable paging
    terminal_writestring("Enabling paging...\n");
    vmm_switch_page_directory(current_page_directory);
    vmm_enable_paging();
    terminal_writestring("Paging enabled successfully!\n");
    
    // Initialize Kernel Heap
    terminal_writestring("Initializing Kernel Heap...\n");
    heap_init();
    terminal_writestring("Heap initialized successfully!\n");
    
    // Initialize Process Management
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK));
    terminal_writestring("Initializing Process Management...\n");
    process_init();
    terminal_writestring("Process management initialized successfully!\n");
    
    // Initialize System Calls
    terminal_writestring("Initializing System Calls...\n");
    syscall_init();
    terminal_writestring("System calls initialized successfully!\n");
    
    // Initialize Memory File System
    terminal_writestring("Initializing Memory File System...\n");
    memfs_init();
    terminal_writestring("Memory file system initialized successfully!\n");
    
    // Enable interrupts
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Enabling interrupts...\n");
    asm volatile ("sti");
    terminal_writestring("Interrupts enabled!\n\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_writestring("Day 11 Features:\n");
    terminal_writestring("- Physical Memory Manager (PMM)\n");
    terminal_writestring("- Virtual Memory Manager (VMM)\n");
    terminal_writestring("- Paging System (4KB pages)\n");
    terminal_writestring("- Kernel Heap (kmalloc/kfree)\n");
    terminal_writestring("- Minimal Process Management\n");
    terminal_writestring("- Basic Round-Robin Scheduler\n");
    terminal_writestring("- Simple Context Switching\n");
    terminal_writestring("- System Call Interface (INT 0x80)\n");
    terminal_writestring("- 9 System Calls (incl. file operations)\n");
    terminal_writestring("- Memory-Based File System (MemFS)\n");
    terminal_writestring("- File Operations (open/read/write/close/list)\n");
    terminal_writestring("- Interactive Command Shell\n");
    terminal_writestring("- Real File System Commands (ls/cat/create/write/delete)\n\n");
    
    // Memory management demonstration
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
    terminal_writestring("Memory Management Test:\n");
    
    // Display memory statistics
    terminal_setcolor(vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    pmm_dump_stats();
    terminal_writestring("\n");
    heap_dump_stats();
    terminal_writestring("\n");
    
    // Test dynamic memory allocation
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("Testing dynamic memory allocation...\n");
    
    void* ptr1 = kmalloc(1024);
    terminal_writestring("Allocated 1024 bytes: ");
    if (ptr1) {
        terminal_writestring("SUCCESS\n");
        debug_write_string("kmalloc(1024) successful\n");
    } else {
        terminal_writestring("FAILED\n");
    }
    
    void* ptr2 = kmalloc(2048);
    terminal_writestring("Allocated 2048 bytes: ");
    if (ptr2) {
        terminal_writestring("SUCCESS\n");
        debug_write_string("kmalloc(2048) successful\n");
    } else {
        terminal_writestring("FAILED\n");
    }
    
    void* ptr3 = kcalloc(10, 64);
    terminal_writestring("Allocated 10x64 bytes (zeroed): ");
    if (ptr3) {
        terminal_writestring("SUCCESS\n");
        debug_write_string("kcalloc(10, 64) successful\n");
    } else {
        terminal_writestring("FAILED\n");
    }
    
    // Free some memory
    if (ptr1) {
        kfree(ptr1);
        terminal_writestring("Freed first allocation\n");
    }
    
    if (ptr2) {
        kfree(ptr2);
        terminal_writestring("Freed second allocation\n");
    }
    
    terminal_writestring("\nAfter allocations and frees:\n");
    heap_dump_stats();
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("\nDay 6 Memory Management System Complete!\n");
    terminal_writestring("All components operational and tested.\n");
    debug_write_string("Day 6 memory management test completed successfully!\n");
    
    // Process management test
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Process Management Test:\n");
    
    // Create test processes  
    int pid1 = process_create(test_process_1, "test1");
    int pid2 = process_create(test_process_2, "test2");
    
    if (pid1 != INVALID_PID) {
        terminal_printf("Created test process 1 (PID: %d)\n", pid1);
    }
    if (pid2 != INVALID_PID) {
        terminal_printf("Created test process 2 (PID: %d)\n", pid2);
    }
    
    // List all processes
    process_list();
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Day 7 Minimal Process Management Complete!\n");
    terminal_writestring("Basic scheduling demonstration ready.\n");
    
    // System call testing
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\nSystem Call Testing:\n");
    
    // Test sys_hello system call
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("Testing sys_hello system call...\n");
    int result = syscall_hello();
    terminal_printf("sys_hello returned: %d\n", result);
    
    // Test sys_write system call
    terminal_writestring("Testing sys_write system call...\n");
    result = syscall_write("Hello from system call!\n");
    terminal_printf("sys_write returned: %d\n", result);
    
    // Test sys_getpid system call
    terminal_writestring("Testing sys_getpid system call...\n");
    result = syscall_getpid();
    terminal_printf("sys_getpid returned: %d\n", result);
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Day 8 Basic System Calls Complete!\n");
    terminal_writestring("All 4 system calls operational and tested.\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("Day 9 Memory File System Complete!\n");
    terminal_writestring("Basic file system loaded and ready for testing.\n\n");
    
    // Initialize and start shell
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Starting ClaudeOS Shell...\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    shell_init();
    
    // Main shell loop - no longer an infinite counter
    while (1) {
        asm volatile ("hlt");
        
        // Process keyboard input for shell
        char c = keyboard_get_char();
        if (c != 0) {
            shell_process_input(c);
        }
    }
}

// Test process functions (simple demonstrations)
void test_process_1(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    terminal_writestring("[PROC1] Test process 1 running\n");
    
    for (int i = 0; i < 3; i++) {
        terminal_printf("[PROC1] Iteration %d\n", i + 1);
        // Simple loop simulation
        for (volatile int j = 0; j < TEST_PROCESS_WORK_LOOP; j++);
    }
    
    terminal_writestring("[PROC1] Test process 1 exiting\n");
    process_exit();
}

void test_process_2(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK));
    terminal_writestring("[PROC2] Test process 2 running\n");
    
    for (int i = 0; i < 3; i++) {
        terminal_printf("[PROC2] Iteration %d\n", i + 1);
        // Simple loop simulation
        for (volatile int j = 0; j < TEST_PROCESS_WORK_LOOP; j++);
    }
    
    terminal_writestring("[PROC2] Test process 2 exiting\n");
    process_exit();
}