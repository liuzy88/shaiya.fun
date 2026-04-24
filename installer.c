#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#define MAX_STR 2048
#define BUFFER_SIZE 65536
#define DEFAULT_REPO "liuzy88/shaiya.fun"
#define DEFAULT_REF "main"
#define DEFAULT_PROXY_LIST "https://ghfast.top/;DIRECT"
#define INSTALLER_VERSION "api-20260424-6"

typedef struct {
    char package_id[MAX_STR];
    char md5[MAX_STR];
    char repo[MAX_STR];
    char ref[MAX_STR];
    char file_name[MAX_STR];
    char chunk_pattern[MAX_STR];
    char proxy_list[MAX_STR];
    unsigned long long total_size;
    unsigned long long chunk_size;
    unsigned long long chunk_count;
} manifest_t;

typedef struct {
    char *data;
    size_t size;
    DWORD status_code;
    char content_type[256];
} http_buffer_t;

static void log_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    fputc('\n', stdout);
    va_end(args);
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

static void log_buffer_preview(const char *label, const char *buffer, size_t size) {
    char preview[257];
    size_t copy_size = size > sizeof(preview) - 1 ? sizeof(preview) - 1 : size;

    memcpy(preview, buffer, copy_size);
    preview[copy_size] = '\0';
    sanitize_preview(preview, copy_size);
    log_message("%s preview: %s", label, preview);
}

static int manifest_set_value(manifest_t *manifest, const char *key, const char *value) {
    if (strcmp(key, "repo") == 0) {
        strncpy(manifest->repo, value, sizeof(manifest->repo) - 1);
    } else if (strcmp(key, "ref") == 0) {
        strncpy(manifest->ref, value, sizeof(manifest->ref) - 1);
    } else if (strcmp(key, "package_id") == 0) {
        strncpy(manifest->package_id, value, sizeof(manifest->package_id) - 1);
    } else if (strcmp(key, "md5") == 0) {
        strncpy(manifest->md5, value, sizeof(manifest->md5) - 1);
    } else if (strcmp(key, "file_name") == 0) {
        strncpy(manifest->file_name, value, sizeof(manifest->file_name) - 1);
    } else if (strcmp(key, "chunk_pattern") == 0) {
        strncpy(manifest->chunk_pattern, value, sizeof(manifest->chunk_pattern) - 1);
    } else if (strcmp(key, "proxy_list") == 0) {
        strncpy(manifest->proxy_list, value, sizeof(manifest->proxy_list) - 1);
    } else if (strcmp(key, "total_size") == 0) {
        manifest->total_size = _strtoui64(value, NULL, 10);
    } else if (strcmp(key, "chunk_size") == 0) {
        manifest->chunk_size = _strtoui64(value, NULL, 10);
    } else if (strcmp(key, "chunk_count") == 0) {
        manifest->chunk_count = _strtoui64(value, NULL, 10);
    }
    return 0;
}

static int manifest_looks_valid(const manifest_t *manifest) {
    return manifest->repo[0] != '\0' && manifest->ref[0] != '\0' && manifest->package_id[0] != '\0' &&
           manifest->file_name[0] != '\0' && manifest->chunk_pattern[0] != '\0' && manifest->chunk_count != 0;
}

static int parse_manifest_buffer(const char *buffer, size_t size, manifest_t *manifest) {
    char *copy;
    char *line_ptr;
    char *next = NULL;

    memset(manifest, 0, sizeof(*manifest));
    copy = (char *) malloc(size + 1);
    if (copy == NULL) {
        log_message("failed to allocate manifest buffer copy");
        return -1;
    }
    memcpy(copy, buffer, size);
    copy[size] = '\0';

    line_ptr = strtok_s(copy, "\r\n", &next);
    while (line_ptr != NULL) {
        char *eq = strchr(line_ptr, '=');
        if (eq == NULL || eq == line_ptr) {
            line_ptr = strtok_s(NULL, "\r\n", &next);
            continue;
        }
        *eq = '\0';
        manifest_set_value(manifest, line_ptr, eq + 1);
        line_ptr = strtok_s(NULL, "\r\n", &next);
    }

    free(copy);

    if (!manifest_looks_valid(manifest)) {
        log_message("manifest is incomplete");
        return -1;
    }
    return 0;
}

static int ensure_directory(const char *path) {
    if (CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    log_message("failed to create directory %s", path);
    return -1;
}

static int join_path(char *out, size_t out_size, const char *dir, const char *file) {
    int written = snprintf(out, out_size, "%s\\%s", dir, file);
    return written > 0 && (size_t) written < out_size ? 0 : -1;
}

static int extract_proxy(const char *proxy_list, int index, char *proxy, size_t proxy_size) {
    int current = 0;
    const char *start = proxy_list;
    const char *cursor = proxy_list;

    while (1) {
        if (*cursor == ';' || *cursor == '\0') {
            if (current == index) {
                size_t len = (size_t) (cursor - start);
                if (len >= proxy_size) {
                    return -1;
                }
                memcpy(proxy, start, len);
                proxy[len] = '\0';
                return 0;
            }
            if (*cursor == '\0') {
                break;
            }
            current++;
            start = cursor + 1;
        }
        cursor++;
    }
    return -1;
}

static void build_asset_url(const char *repo, const char *ref, const char *package_id, const char *asset_name, char *url,
                            size_t url_size) {
    snprintf(url,
             url_size,
             "https://api.github.com/repos/%s/contents/%s/%s?ref=%s",
             repo,
             package_id,
             asset_name,
             ref);
}

static void build_request_url(const char *proxy, const char *origin_url, char *url, size_t url_size) {
    if (_stricmp(proxy, "DIRECT") == 0) {
        strncpy(url, origin_url, url_size - 1);
        url[url_size - 1] = '\0';
        return;
    }
    snprintf(url, url_size, "%s%s", proxy, origin_url);
}

static int split_url(const char *url, wchar_t *host, size_t host_size, wchar_t *path, size_t path_size, INTERNET_PORT *port,
                     int *secure) {
    URL_COMPONENTSW parts;
    wchar_t wide_url[MAX_STR];
    wchar_t url_path[MAX_STR];
    wchar_t extra_info[MAX_STR];

    memset(&parts, 0, sizeof(parts));
    memset(wide_url, 0, sizeof(wide_url));
    memset(url_path, 0, sizeof(url_path));
    memset(extra_info, 0, sizeof(extra_info));
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wide_url, (int) (sizeof(wide_url) / sizeof(wide_url[0])));

    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD) host_size;
    parts.lpszHostName = host;
    parts.dwUrlPathLength = (DWORD) (sizeof(url_path) / sizeof(url_path[0]));
    parts.lpszUrlPath = url_path;
    parts.dwExtraInfoLength = (DWORD) (sizeof(extra_info) / sizeof(extra_info[0]));
    parts.lpszExtraInfo = extra_info;

    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts)) {
        return -1;
    }

    host[parts.dwHostNameLength] = L'\0';
    url_path[parts.dwUrlPathLength] = L'\0';
    extra_info[parts.dwExtraInfoLength] = L'\0';

    if (_snwprintf(path, path_size, L"%ls%ls", url_path, extra_info) < 0) {
        return -1;
    }
    path[path_size - 1] = L'\0';

    *port = parts.nPort;
    *secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return 0;
}

static int query_content_type(HINTERNET request, char *buffer, size_t buffer_size) {
    DWORD size = (DWORD) buffer_size;

    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CONTENT_TYPE,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             buffer,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        buffer[0] = '\0';
        return -1;
    }
    if (size >= 2 && buffer[size - 2] == '\0') {
        return 0;
    }
    buffer[buffer_size - 1] = '\0';
    return 0;
}

static int download_response(const char *url, http_buffer_t *response) {
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    static const wchar_t *headers =
        L"User-Agent: shaiya-installer/1.0\r\n"
        L"Accept: application/vnd.github.raw\r\n";
    wchar_t host[MAX_STR];
    wchar_t path[MAX_STR];
    INTERNET_PORT port = 0;
    int secure = 0;
    BYTE buffer[BUFFER_SIZE];
    DWORD bytes_read = 0;
    DWORD status_code_size = sizeof(response->status_code);
    size_t capacity = 0;
    int ok = -1;

    memset(response, 0, sizeof(*response));

    if (split_url(url, host, MAX_STR, path, MAX_STR, &port, &secure) != 0) {
        log_message("invalid url: %s", url);
        return -1;
    }

    log_message("request url: %s", url);
    wprintf(L"request host=%ls path=%ls\n", host, path);

    session = WinHttpOpen(L"ChunkInstaller/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        goto cleanup;
    }

    connect = WinHttpConnect(session, host, port, 0);
    if (connect == NULL) {
        goto cleanup;
    }

    request = WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == NULL) {
        goto cleanup;
    }

    if (!WinHttpSendRequest(request, headers, (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        goto cleanup;
    }

    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &response->status_code,
                             &status_code_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        goto cleanup;
    }

    query_content_type(request, response->content_type, sizeof(response->content_type));
    log_message("response status=%lu content-type=%s",
                (unsigned long) response->status_code,
                response->content_type[0] != '\0' ? response->content_type : "(unknown)");

    if (response->status_code != 200) {
        log_message("unexpected status %lu for %s", (unsigned long) response->status_code, url);
        goto cleanup;
    }

    do {
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read)) {
            goto cleanup;
        }
        if (bytes_read > 0) {
            char *new_data;
            if (response->size + bytes_read + 1 > capacity) {
                size_t new_capacity = capacity == 0 ? (size_t) bytes_read + 1 : capacity * 2;
                while (new_capacity < response->size + bytes_read + 1) {
                    new_capacity *= 2;
                }
                new_data = (char *) realloc(response->data, new_capacity);
                if (new_data == NULL) {
                    log_message("failed to grow response buffer");
                    goto cleanup;
                }
                response->data = new_data;
                capacity = new_capacity;
            }
            memcpy(response->data + response->size, buffer, bytes_read);
            response->size += bytes_read;
        }
    } while (bytes_read > 0);

    if (response->data == NULL) {
        response->data = (char *) malloc(1);
        if (response->data == NULL) {
            goto cleanup;
        }
    }
    response->data[response->size] = '\0';
    log_message("response bytes=%lu", (unsigned long) response->size);

    ok = 0;

cleanup:
    if (request != NULL) {
        WinHttpCloseHandle(request);
    }
    if (connect != NULL) {
        WinHttpCloseHandle(connect);
    }
    if (session != NULL) {
        WinHttpCloseHandle(session);
    }
    if (ok != 0) {
        free(response->data);
        response->data = NULL;
        response->size = 0;
    }
    return ok;
}

static int write_buffer_to_file(const char *path, const char *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        log_message("failed to create %s", path);
        return -1;
    }
    if (size > 0 && fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        DeleteFileA(path);
        log_message("failed to write %s", path);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int download_asset_with_fallback(const char *repo, const char *ref, const char *package_id, const char *proxy_list,
                                        const char *asset_name, const char *output_path) {
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_STR];
    http_buffer_t response;
    int proxy_index = 0;

    build_asset_url(repo, ref, package_id, asset_name, origin_url, sizeof(origin_url));
    while (extract_proxy(proxy_list, proxy_index++, proxy, sizeof(proxy)) == 0) {
        build_request_url(proxy, origin_url, request_url, sizeof(request_url));
        log_message("downloading %s via %s", asset_name, proxy);
        if (download_response(request_url, &response) == 0) {
            if (write_buffer_to_file(output_path, response.data, response.size) == 0) {
                free(response.data);
                return 0;
            }
            free(response.data);
            return -1;
        }
    }

    log_message("failed to download %s", asset_name);
    return -1;
}

static int download_manifest_with_fallback(const char *repo, const char *ref, const char *package_id, const char *proxy_list,
                                           manifest_t *manifest) {
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_STR];
    http_buffer_t response;
    int proxy_index = 0;

    build_asset_url(repo, ref, package_id, "manifest.txt", origin_url, sizeof(origin_url));
    while (extract_proxy(proxy_list, proxy_index++, proxy, sizeof(proxy)) == 0) {
        build_request_url(proxy, origin_url, request_url, sizeof(request_url));
        log_message("downloading manifest.txt via %s", proxy);
        if (download_response(request_url, &response) != 0) {
            continue;
        }

        log_buffer_preview("manifest", response.data, response.size);
        if (strstr(response.data, "<html") != NULL || strstr(response.data, "<!DOCTYPE html") != NULL) {
            log_message("manifest response looks like HTML, rejecting");
            free(response.data);
            continue;
        }

        if (parse_manifest_buffer(response.data, response.size, manifest) == 0) {
            log_message("manifest parsed: package_id=%s file_name=%s chunk_count=%lu",
                        manifest->package_id,
                        manifest->file_name,
                        (unsigned long) manifest->chunk_count);
            free(response.data);
            return 0;
        }
        free(response.data);
    }

    log_message("failed to download manifest.txt");
    return -1;
}

static int download_default_manifest_with_fallback(const char *repo, const char *ref, const char *proxy_list,
                                                   manifest_t *manifest) {
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_STR];
    http_buffer_t response;
    int proxy_index = 0;

    snprintf(origin_url,
             sizeof(origin_url),
             "https://api.github.com/repos/%s/contents/manifest.txt?ref=%s",
             repo,
             ref);

    while (extract_proxy(proxy_list, proxy_index++, proxy, sizeof(proxy)) == 0) {
        build_request_url(proxy, origin_url, request_url, sizeof(request_url));
        log_message("downloading default manifest.txt via %s", proxy);
        if (download_response(request_url, &response) != 0) {
            continue;
        }

        log_buffer_preview("default manifest", response.data, response.size);
        if (strstr(response.data, "<html") != NULL || strstr(response.data, "<!DOCTYPE html") != NULL) {
            log_message("default manifest response looks like HTML, rejecting");
            free(response.data);
            continue;
        }

        if (parse_manifest_buffer(response.data, response.size, manifest) == 0) {
            log_message("default manifest parsed: package_id=%s file_name=%s chunk_count=%lu",
                        manifest->package_id,
                        manifest->file_name,
                        (unsigned long) manifest->chunk_count);
            free(response.data);
            return 0;
        }
        free(response.data);
    }

    log_message("failed to download default manifest.txt");
    return -1;
}

static int merge_chunks(const manifest_t *manifest, const char *download_dir, const char *output_path) {
    FILE *output = fopen(output_path, "wb");
    BYTE buffer[BUFFER_SIZE];
    unsigned long long i;

    if (output == NULL) {
        log_message("failed to create merged file %s", output_path);
        return -1;
    }

    for (i = 0; i < manifest->chunk_count; ++i) {
        char asset_name[MAX_STR];
        char chunk_path[MAX_STR];
        FILE *input;
        size_t read_count;

        snprintf(asset_name, sizeof(asset_name), manifest->chunk_pattern, (int) i);
        if (join_path(chunk_path, sizeof(chunk_path), download_dir, asset_name) != 0) {
            fclose(output);
            return -1;
        }

        input = fopen(chunk_path, "rb");
        if (input == NULL) {
            fclose(output);
            log_message("missing chunk %s", chunk_path);
            return -1;
        }

        while ((read_count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
            if (fwrite(buffer, 1, read_count, output) != read_count) {
                fclose(input);
                fclose(output);
                return -1;
            }
        }
        fclose(input);
    }

    fclose(output);
    return 0;
}

static void cleanup_chunks(const manifest_t *manifest, const char *download_dir) {
    unsigned long long i;

    for (i = 0; i < manifest->chunk_count; ++i) {
        char asset_name[MAX_STR];
        char chunk_path[MAX_STR];

        snprintf(asset_name, sizeof(asset_name), manifest->chunk_pattern, (int) i);
        if (join_path(chunk_path, sizeof(chunk_path), download_dir, asset_name) != 0) {
            continue;
        }
        if (!DeleteFileA(chunk_path)) {
            log_message("failed to delete chunk %s, error=%lu", chunk_path, (unsigned long) GetLastError());
        }
    }

    if (!RemoveDirectoryA(download_dir)) {
        log_message("failed to remove directory %s, error=%lu", download_dir, (unsigned long) GetLastError());
    } else {
        log_message("removed directory %s", download_dir);
    }
}

static int launch_program(const char *path) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char command[MAX_STR];
    char full_path[MAX_PATH];
    DWORD resolved;
    DWORD error_code;
    SHELLEXECUTEINFOA exec_info;

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    memset(full_path, 0, sizeof(full_path));
    resolved = GetFullPathNameA(path, sizeof(full_path), full_path, NULL);
    if (resolved == 0 || resolved >= sizeof(full_path)) {
        log_message("failed to resolve full path for %s", path);
        return -1;
    }

    log_message("launch target: %s", full_path);
    snprintf(command, sizeof(command), "\"%s\"", full_path);

    if (CreateProcessA(full_path, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }

    error_code = GetLastError();
    log_message("CreateProcess failed for %s, error=%lu", full_path, (unsigned long) error_code);

    memset(&exec_info, 0, sizeof(exec_info));
    exec_info.cbSize = sizeof(exec_info);
    exec_info.fMask = SEE_MASK_NOCLOSEPROCESS;
    exec_info.lpFile = full_path;
    exec_info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&exec_info)) {
        error_code = GetLastError();
        log_message("ShellExecuteEx failed for %s, error=%lu", full_path, (unsigned long) error_code);
        return -1;
    }

    if (exec_info.hProcess != NULL) {
        CloseHandle(exec_info.hProcess);
    }
    return 0;
}

int main(int argc, char **argv) {
    char package_id[MAX_PATH];
    char download_dir[MAX_PATH] = "patch";
    char merged_path[MAX_PATH];
    manifest_t manifest;
    unsigned long long i;

    log_message("ChunkInstaller %s", INSTALLER_VERSION);

    if (argc >= 2) {
        strncpy(package_id, argv[1], sizeof(package_id) - 1);
        package_id[sizeof(package_id) - 1] = '\0';
        log_message("mode=package package_id=%s", package_id);

        if (download_manifest_with_fallback(DEFAULT_REPO, DEFAULT_REF, package_id, DEFAULT_PROXY_LIST, &manifest) != 0) {
            return 1;
        }

        if (_stricmp(manifest.package_id, package_id) != 0) {
            log_message("manifest package_id mismatch");
            return 1;
        }
    } else {
        log_message("mode=default manifest.txt from repository root");
        if (download_default_manifest_with_fallback(DEFAULT_REPO, DEFAULT_REF, DEFAULT_PROXY_LIST, &manifest) != 0) {
            return 1;
        }
    }

    if (ensure_directory(download_dir) != 0) {
        return 1;
    }

    for (i = 0; i < manifest.chunk_count; ++i) {
        char asset_name[MAX_STR];
        char chunk_path[MAX_STR];

        snprintf(asset_name, sizeof(asset_name), manifest.chunk_pattern, (int) i);
        if (join_path(chunk_path, sizeof(chunk_path), download_dir, asset_name) != 0) {
            return 1;
        }
        if (download_asset_with_fallback(manifest.repo, manifest.ref, manifest.package_id, DEFAULT_PROXY_LIST, asset_name,
                                         chunk_path) != 0) {
            return 1;
        }
    }

    strncpy(merged_path, manifest.file_name, sizeof(merged_path) - 1);
    if (merge_chunks(&manifest, download_dir, merged_path) != 0) {
        return 1;
    }

    log_message("merged file created: %s", merged_path);
    cleanup_chunks(&manifest, download_dir);
    return launch_program(merged_path) == 0 ? 0 : 1;
}

#else
int main(void) {
    return 1;
}
#endif
