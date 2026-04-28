#include "install.h"

#ifdef _WIN32

int split_url(const char *url, wchar_t *host, size_t host_size, wchar_t *path, size_t path_size, INTERNET_PORT *port,
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
    wchar_t wide_buffer[256];
    DWORD size = sizeof(wide_buffer);

    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, wide_buffer, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        buffer[0] = '\0';
        return -1;
    }
    WideCharToMultiByte(CP_UTF8, 0, wide_buffer, -1, buffer, (int) buffer_size, NULL, NULL);
    buffer[buffer_size - 1] = '\0';
    return 0;
}

int download_response(file_logger_t *logger, const char *url, http_buffer_t *response, int api_mode,
                             progress_callback_t progress_cb, void *progress_ctx, const char *progress_label) {
    static const wchar_t *api_headers =
        L"User-Agent: shaiya-installer/1.0\r\n"
        L"Accept: application/vnd.github.raw\r\n";
    static const wchar_t *raw_headers =
        L"User-Agent: shaiya-installer/1.0\r\n";
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    wchar_t host[MAX_STR];
    wchar_t path[MAX_STR];
    INTERNET_PORT port = 0;
    int secure = 0;
    BYTE buffer[BUFFER_SIZE];
    DWORD bytes_read = 0;
    DWORD status_code_size;
    DWORD content_length_dword = sizeof(DWORD);
    DWORD content_length_value = 0;
    size_t capacity = 0;
    int ok = -1;

    memset(response, 0, sizeof(*response));
    status_code_size = sizeof(response->status_code);
    if (split_url(url, host, MAX_STR, path, MAX_STR, &port, &secure) != 0) {
        log_message(logger, "invalid url: %s", url);
        return -1;
    }

    log_message(logger, "request url: %s", url);
    log_message(logger, "request host=%ls path=%ls", host, path);
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
    if (!WinHttpSendRequest(request, api_mode ? api_headers : raw_headers, (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
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
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &content_length_value,
                             &content_length_dword,
                             WINHTTP_NO_HEADER_INDEX)) {
        response->content_length = 0;
    } else {
        response->content_length = (unsigned long long) content_length_value;
    }
    log_message(logger, "response status=%lu content-type=%s", (unsigned long) response->status_code,
                response->content_type[0] != '\0' ? response->content_type : "(unknown)");
    if (response->status_code != 200) {
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
                    goto cleanup;
                }
                response->data = new_data;
                capacity = new_capacity;
            }
            memcpy(response->data + response->size, buffer, bytes_read);
            response->size += bytes_read;
            if (progress_cb != NULL) {
                progress_cb(progress_ctx, progress_label, (unsigned long long) response->size, response->content_length, 0);
            }
        }
    } while (bytes_read > 0);

    if (response->data == NULL) {
        response->data = (char *) malloc(1);
        if (response->data == NULL) {
            goto cleanup;
        }
    }
    response->data[response->size] = '\0';
    if (progress_cb != NULL) {
        progress_cb(progress_ctx, progress_label, (unsigned long long) response->size, response->content_length, 1);
    }
    log_message(logger, "response bytes=%lu", (unsigned long) response->size);
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
        log_message(logger, "request failed for %s", url);
        free(response->data);
        response->data = NULL;
        response->size = 0;
    }
    return ok;
}

#endif
