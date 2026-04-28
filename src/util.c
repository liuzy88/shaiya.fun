#include "install.h"

#ifdef _WIN32

void safe_copy(char *dst, size_t dst_size, const char *src) {
    size_t len;

    if (dst_size == 0) {
        return;
    }
    len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

int checked_format(char *dst, size_t dst_size, const char *fmt, ...) {
    va_list args;
    int written;

    if (dst_size == 0) {
        return -1;
    }
    va_start(args, fmt);
    written = vsnprintf(dst, dst_size, fmt, args);
    va_end(args);
    if (written < 0 || (size_t) written >= dst_size) {
        dst[dst_size - 1] = '\0';
        return -1;
    }
    return 0;
}
void format_size(unsigned long long bytes, char *buffer, size_t buffer_size) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double) bytes;
    int unit = 0;

    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        snprintf(buffer, buffer_size, "%llu %s", bytes, units[unit]);
    } else {
        snprintf(buffer, buffer_size, "%.1f %s", value, units[unit]);
    }
}

void format_speed(unsigned long long bytes, ULONGLONG started_tick, ULONGLONG ended_tick, char *buffer, size_t buffer_size) {
    double elapsed_seconds;
    double bytes_per_second;

    if (started_tick == 0 || ended_tick <= started_tick) {
        snprintf(buffer, buffer_size, "0.00 KB/s");
        return;
    }

    elapsed_seconds = (double) (ended_tick - started_tick) / 1000.0;
    if (elapsed_seconds <= 0.0) {
        snprintf(buffer, buffer_size, "0.00 KB/s");
        return;
    }

    bytes_per_second = (double) bytes / elapsed_seconds;
    if (bytes_per_second >= 1024.0 * 1024.0) {
        snprintf(buffer, buffer_size, "%.2f MB/s", bytes_per_second / (1024.0 * 1024.0));
    } else {
        snprintf(buffer, buffer_size, "%.2f KB/s", bytes_per_second / 1024.0);
    }
}
int is_safe_output_file_name(const char *file_name) {
    return file_name[0] != '\0' && strcmp(file_name, ".") != 0 && strcmp(file_name, "..") != 0 &&
           strchr(file_name, '/') == NULL && strchr(file_name, '\\') == NULL && strchr(file_name, ':') == NULL &&
           strstr(file_name, "..") == NULL;
}
static int ensure_directory(const char *path) {
    if (CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    return -1;
}

int ensure_directory_recursive(const char *path) {
    char temp[MAX_PATH];
    size_t len;
    size_t i;

    safe_copy(temp, sizeof(temp), path);
    len = strlen(temp);
    if (len == 0) {
        return -1;
    }
    if (len > 3 && (temp[len - 1] == '\\' || temp[len - 1] == '/')) {
        temp[len - 1] = '\0';
        len--;
    }
    if (len == 3 && temp[1] == ':' && (temp[2] == '\\' || temp[2] == '/')) {
        return 0;
    }
    for (i = 3; temp[i] != '\0'; ++i) {
        if (temp[i] == '\\' || temp[i] == '/') {
            char saved = temp[i];
            temp[i] = '\0';
            if (ensure_directory(temp) != 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
                return -1;
            }
            temp[i] = saved;
        }
    }
    return ensure_directory(temp);
}
int join_path(char *out, size_t out_size, const char *dir, const char *file) {
    return checked_format(out, out_size, "%s\\%s", dir, file);
}

#endif
