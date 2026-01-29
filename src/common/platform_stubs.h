// Minimal platform type stubs to satisfy LSP diagnostics in CI
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int BOOL;
typedef void VOID;
typedef char CHAR;
typedef unsigned char BYTE;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef int64_t INT64;
typedef int8_t INT8;
typedef float FLOAT;

// std va_list provided by <stdarg.h>

// Minimal C library function prototypes used by project sources (stubs for LSP)
static inline int strcmp_stub(const char* a, const char* b) { return strcmp(a, b); }
static inline void* memset_stub(void* s, int c, size_t n) { return memset(s, c, n); }
static inline int vsprintf_s(char* buffer, const char* format, va_list vl) { return vsprintf(buffer, format, vl); }
static inline int vprintf_stub(const char* format, va_list vl) { return vprintf(format, vl); }
static inline size_t strlen_stub(const char* s) { return strlen(s); }

// Minimal Windows API stubs used in headers (for LSP only)
typedef void* HMODULE;
static inline HMODULE GetModuleHandle(const char* name) { return NULL; }
typedef void* FARPROC;
static inline FARPROC GetProcAddress(HMODULE h, const char* procName) { return NULL; }
static inline int MessageBoxA(void* hwnd, const char* text, const char* title, unsigned int flags) { return 0; }
static inline void exit_stub(int code) { exit(code); }
// Define MB_OK for compatibility
#define MB_OK 0

// Expose standard C functions with expected names for LSP diagnostics
int strcmp(const char* a, const char* b);
void* memset(void* s, int c, size_t n);
int vsprintf_s(char* buffer, const char* format, va_list vl);
int vprintf(const char* format, va_list vl);
size_t strlen(const char* s);
int MessageBoxA(void* hwnd, const char* text, const char* title, unsigned int flags);
void exit(int code);
