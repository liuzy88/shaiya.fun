#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#define MAX_STR 2048
#define BUFFER_SIZE 65536
#define DEFAULT_REPO "liuzy88/shaiya.fun"
#define DEFAULT_REF "main"

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

static void log_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    fputc('\n', stdout);
    va_end(args);
}

static int read_line(FILE *fp, char *buffer, size_t size) {
    if (fgets(buffer, (int) size, fp) == NULL) {
        return 0;
    }
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 1;
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

static int load_manifest(const char *path, manifest_t *manifest) {
    FILE *fp = fopen(path, "rb");
    char line[MAX_STR];

    memset(manifest, 0, sizeof(*manifest));
    if (fp == NULL) {
        log_message("failed to open manifest %s", path);
        return -1;
    }

    while (read_line(fp, line, sizeof(line))) {
        char *eq = strchr(line, '=');
        if (eq == NULL || eq == line) {
            continue;
        }
        *eq = '\0';
        manifest_set_value(manifest, line, eq + 1);
    }
    fclose(fp);

    if (manifest->repo[0] == '\0' || manifest->ref[0] == '\0' || manifest->package_id[0] == '\0' ||
        manifest->file_name[0] == '\0' ||
        manifest->chunk_pattern[0] == '\0' || manifest->chunk_count == 0) {
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
             "https://raw.githubusercontent.com/%s/%s/%s/%s",
             repo,
             ref,
             package_id,
             asset_name);
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

    memset(&parts, 0, sizeof(parts));
    memset(wide_url, 0, sizeof(wide_url));
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wide_url, (int) (sizeof(wide_url) / sizeof(wide_url[0])));

    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD) host_size;
    parts.lpszHostName = host;
    parts.dwUrlPathLength = (DWORD) path_size;
    parts.lpszUrlPath = path;
    parts.dwExtraInfoLength = (DWORD) (path_size - 1);
    parts.lpszExtraInfo = path + wcslen(path);

    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts)) {
        return -1;
    }

    host[parts.dwHostNameLength] = L'\0';
    path[parts.dwUrlPathLength] = L'\0';
    if (parts.dwExtraInfoLength > 0) {
        path[parts.dwUrlPathLength + parts.dwExtraInfoLength] = L'\0';
    }
    *port = parts.nPort;
    *secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return 0;
}

static int download_to_file(const char *url, const char *output_path) {
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    wchar_t host[MAX_STR];
    wchar_t path[MAX_STR];
    INTERNET_PORT port = 0;
    int secure = 0;
    FILE *fp = NULL;
    BYTE buffer[BUFFER_SIZE];
    DWORD bytes_read = 0;
    int ok = -1;

    if (split_url(url, host, MAX_STR, path, MAX_STR, &port, &secure) != 0) {
        log_message("invalid url: %s", url);
        return -1;
    }

    session = WinHttpOpen(L"ChunkInstaller/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
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

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        goto cleanup;
    }

    fp = fopen(output_path, "wb");
    if (fp == NULL) {
        goto cleanup;
    }

    do {
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read)) {
            goto cleanup;
        }
        if (bytes_read > 0 && fwrite(buffer, 1, bytes_read, fp) != bytes_read) {
            goto cleanup;
        }
    } while (bytes_read > 0);

    ok = 0;

cleanup:
    if (fp != NULL) {
        fclose(fp);
    }
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
        DeleteFileA(output_path);
    }
    return ok;
}

static int download_asset_with_fallback(const char *repo, const char *ref, const char *package_id, const char *proxy_list,
                                        const char *asset_name, const char *output_path) {
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_STR];
    int proxy_index = 0;

    build_asset_url(repo, ref, package_id, asset_name, origin_url, sizeof(origin_url));
    while (extract_proxy(proxy_list, proxy_index++, proxy, sizeof(proxy)) == 0) {
        build_request_url(proxy, origin_url, request_url, sizeof(request_url));
        log_message("downloading %s via %s", asset_name, proxy);
        if (download_to_file(request_url, output_path) == 0) {
            return 0;
        }
    }

    log_message("failed to download %s", asset_name);
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

static int launch_program(const char *path) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char command[MAX_STR];

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    snprintf(command, sizeof(command), "\"%s\"", path);

    if (!CreateProcessA(path, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        log_message("failed to launch %s", path);
        return -1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

int main(int argc, char **argv) {
    char package_id[MAX_PATH];
    char manifest_path[MAX_PATH] = "manifest.txt";
    char download_dir[MAX_PATH] = "patch";
    char merged_path[MAX_PATH];
    manifest_t manifest;
    unsigned long long i;

    if (argc < 2) {
        log_message("usage: install.exe exe-<full-md5>");
        return 1;
    }

    strncpy(package_id, argv[1], sizeof(package_id) - 1);
    package_id[sizeof(package_id) - 1] = '\0';

    if (download_asset_with_fallback(DEFAULT_REPO, DEFAULT_REF, package_id, "https://ghfast.top/;https://ghproxy.cc/;DIRECT",
                                     "manifest.txt", manifest_path) != 0) {
        return 1;
    }

    if (load_manifest(manifest_path, &manifest) != 0) {
        return 1;
    }

    if (_stricmp(manifest.package_id, package_id) != 0) {
        log_message("manifest package_id mismatch");
        return 1;
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
        if (download_asset_with_fallback(manifest.repo, manifest.ref, manifest.package_id, manifest.proxy_list, asset_name,
                                         chunk_path) != 0) {
            return 1;
        }
    }

    strncpy(merged_path, manifest.file_name, sizeof(merged_path) - 1);
    if (merge_chunks(&manifest, download_dir, merged_path) != 0) {
        return 1;
    }

    log_message("merged file created: %s", merged_path);
    return launch_program(merged_path) == 0 ? 0 : 1;
}

#else
int main(void) {
    return 1;
}
#endif
