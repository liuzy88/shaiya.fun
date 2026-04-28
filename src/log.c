#include "install.h"

#ifdef _WIN32

static void logger_write_line(file_logger_t *logger, const char *line) {
    DWORD written = 0;
    if (logger->file == INVALID_HANDLE_VALUE) {
        return;
    }
    EnterCriticalSection(&logger->lock);
    SetFilePointer(logger->file, 0, NULL, FILE_END);
    WriteFile(logger->file, line, (DWORD) strlen(line), &written, NULL);
    WriteFile(logger->file, "\r\n", 2, &written, NULL);
    LeaveCriticalSection(&logger->lock);
}

void log_message(file_logger_t *logger, const char *fmt, ...) {
    SYSTEMTIME st;
    char text[2048];
    char line[2300];
    va_list args;

    GetLocalTime(&st);
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    snprintf(line,
             sizeof(line),
             "[%04u-%02u-%02u %02u:%02u:%02u] %s",
             st.wYear,
             st.wMonth,
             st.wDay,
             st.wHour,
             st.wMinute,
             st.wSecond,
             text);
    logger_write_line(logger, line);
}

int init_logger(file_logger_t *logger) {
    char exe_path[MAX_PATH];
    char *slash = NULL;
    char candidate[MAX_PATH];
    char temp_dir[MAX_PATH];
    DWORD temp_len;

    memset(logger, 0, sizeof(*logger));
    logger->file = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&logger->lock);

    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) == 0) {
        return -1;
    }
    slash = strrchr(exe_path, '\\');
    if (slash != NULL) {
        *slash = '\0';
    }
    if (checked_format(candidate, sizeof(candidate), "%s\\install.log", exe_path) == 0) {
        logger->file = CreateFileA(candidate, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
        if (logger->file != INVALID_HANDLE_VALUE) {
            safe_copy(logger->path, sizeof(logger->path), candidate);
            return 0;
        }
    }

    temp_len = GetTempPathA(sizeof(temp_dir), temp_dir);
    if (temp_len == 0 || temp_len >= sizeof(temp_dir)) {
        return -1;
    }
    if (checked_format(candidate, sizeof(candidate), "%sinstall.log", temp_dir) != 0) {
        return -1;
    }
    logger->file = CreateFileA(candidate, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (logger->file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    safe_copy(logger->path, sizeof(logger->path), candidate);
    return 0;
}

void close_logger(file_logger_t *logger) {
    if (logger->file != INVALID_HANDLE_VALUE) {
        CloseHandle(logger->file);
        logger->file = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&logger->lock);
}

static void sanitize_preview(char *buffer, size_t size) {
    size_t i;
    for (i = 0; i < size; ++i) {
        unsigned char c = (unsigned char) buffer[i];
        if ((c < 32 || c > 126) && c != '\r' && c != '\n' && c != '\t') {
            buffer[i] = '.';
        }
    }
}

void log_buffer_preview(file_logger_t *logger, const char *label, const char *buffer, size_t size) {
    char preview[257];
    size_t copy_size = size > sizeof(preview) - 1 ? sizeof(preview) - 1 : size;
    memcpy(preview, buffer, copy_size);
    preview[copy_size] = '\0';
    sanitize_preview(preview, copy_size);
    log_message(logger, "%s preview: %s", label, preview);
}

#endif
