#include "install.h"

#ifdef _WIN32

int create_temp_output_file(file_logger_t *logger, const char *path, unsigned long long size, HANDLE *handle_out) {
    HANDLE file;
    LARGE_INTEGER file_size;

    DeleteFileA(path);
    file = CreateFileA(path,
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ,
                       NULL,
                       CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) {
        log_message(logger, "failed to create temp file %s error=%lu", path, (unsigned long) GetLastError());
        return -1;
    }
    file_size.QuadPart = (LONGLONG) size;
    if (!SetFilePointerEx(file, file_size, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
        log_message(logger, "failed to allocate temp file %s error=%lu", path, (unsigned long) GetLastError());
        CloseHandle(file);
        DeleteFileA(path);
        return -1;
    }
    if (!SetFilePointerEx(file, (LARGE_INTEGER) {0}, NULL, FILE_BEGIN)) {
        log_message(logger, "failed to reset temp file pointer %s error=%lu", path, (unsigned long) GetLastError());
        CloseHandle(file);
        DeleteFileA(path);
        return -1;
    }
    *handle_out = file;
    return 0;
}

static int write_chunk_data(download_plan_t *plan, int chunk_index, const BYTE *data, DWORD size, unsigned long long chunk_offset) {
    LARGE_INTEGER offset;
    DWORD written = 0;
    chunk_item_t *item = &plan->chunks[chunk_index];

    offset.QuadPart = (LONGLONG) (((unsigned long long) chunk_index * plan->manifest.chunk_size) + chunk_offset);
    EnterCriticalSection(&plan->file_lock);
    if (!SetFilePointerEx(plan->temp_file, offset, NULL, FILE_BEGIN) || !WriteFile(plan->temp_file, data, size, &written, NULL) ||
        written != size) {
        LeaveCriticalSection(&plan->file_lock);
        return -1;
    }
    LeaveCriticalSection(&plan->file_lock);
    item->downloaded_size = chunk_offset + size;
    return 0;
}

int compute_file_md5_hex(const char *path, char *md5_hex, size_t md5_hex_size) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE buffer[BUFFER_SIZE];
    DWORD bytes_read = 0;
    BYTE digest[16];
    DWORD digest_len = sizeof(digest);
    static const char hex_chars[] = "0123456789abcdef";
    DWORD i;
    int ok = -1;

    if (md5_hex_size < 33) {
        return -1;
    }
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
        goto cleanup;
    }
    while (ReadFile(file, buffer, sizeof(buffer), &bytes_read, NULL) && bytes_read > 0) {
        if (!CryptHashData(hash, buffer, bytes_read, 0)) {
            goto cleanup;
        }
    }
    if (GetLastError() != ERROR_SUCCESS && GetLastError() != ERROR_HANDLE_EOF) {
        goto cleanup;
    }
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0) || digest_len != sizeof(digest)) {
        goto cleanup;
    }
    for (i = 0; i < digest_len; ++i) {
        md5_hex[i * 2] = hex_chars[(digest[i] >> 4) & 0x0F];
        md5_hex[i * 2 + 1] = hex_chars[digest[i] & 0x0F];
    }
    md5_hex[32] = '\0';
    ok = 0;

cleanup:
    if (hash != 0) {
        CryptDestroyHash(hash);
    }
    if (prov != 0) {
        CryptReleaseContext(prov, 0);
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    return ok;
}
static int get_free_space_bytes(const char *path, unsigned long long *free_bytes) {
    ULARGE_INTEGER available;

    if (!GetDiskFreeSpaceExA(path, &available, NULL, NULL)) {
        return -1;
    }
    *free_bytes = (unsigned long long) available.QuadPart;
    return 0;
}

static int can_use_workspace_root(const char *path) {
    char probe_path[MAX_PATH];
    HANDLE probe;

    if (ensure_directory_recursive(path) != 0) {
        return -1;
    }
    if (join_path(probe_path, sizeof(probe_path), path, ".write-test.tmp") != 0) {
        return -1;
    }
    probe = CreateFileA(probe_path,
                        GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                        NULL);
    if (probe == INVALID_HANDLE_VALUE) {
        return -1;
    }
    CloseHandle(probe);
    DeleteFileA(probe_path);
    return 0;
}

static unsigned long long compute_required_space(const manifest_t *manifest) {
    return (manifest->total_size * 2ULL) + WORKSPACE_MARGIN_BYTES;
}

int choose_workspace_root(const manifest_t *manifest, char *workspace_root, size_t workspace_root_size,
                                 unsigned long long *free_bytes_out, unsigned long long *required_bytes_out) {
    char candidate[MAX_PATH];
    DWORD drives;
    unsigned long long required = compute_required_space(manifest);
    char drive_root[4] = "C:\\";
    int pass;
    int i;

    *required_bytes_out = required;
    drives = GetLogicalDrives();
    for (pass = 0; pass < 3; ++pass) {
        for (i = 0; i < 26; ++i) {
            unsigned long long free_bytes = 0;
            UINT drive_type;

            if ((drives & (1u << i)) == 0) {
                continue;
            }

            if ((pass == 0 && i < 3) || (pass == 1 && i != 2) || (pass == 2 && i >= 3)) {
                continue;
            }

            drive_root[0] = (char) ('A' + i);
            drive_type = GetDriveTypeA(drive_root);
            if (drive_type != DRIVE_FIXED && drive_type != DRIVE_REMOVABLE) {
                continue;
            }
            if (get_free_space_bytes(drive_root, &free_bytes) != 0 || free_bytes < required) {
                continue;
            }

            if (checked_format(candidate, sizeof(candidate), "%s%s", drive_root, WORKSPACE_DIR_NAME) != 0) {
                continue;
            }
            if (can_use_workspace_root(candidate) != 0) {
                continue;
            }

            safe_copy(workspace_root, workspace_root_size, candidate);
            *free_bytes_out = free_bytes;
            return 0;
        }
    }
    return -1;
}
static void chunk_set_state(download_plan_t *plan, int idx, chunk_state_t state, DWORD status_code, DWORD error_code,
                            unsigned long long downloaded_size, const char *detail) {
    unsigned long long finished = 0;
    unsigned long long failed = 0;
    unsigned long long bytes = 0;
    unsigned long long total = plan->manifest.chunk_count;
    unsigned long long i;
    chunk_item_t *item;
    EnterCriticalSection(&plan->chunk_lock);
    item = &plan->chunks[idx];
    item->state = state;
    item->status_code = status_code;
    item->last_error = error_code;
    item->downloaded_size = downloaded_size;
    if (state == CHUNK_DOWNLOADING && item->started_tick == 0) {
        item->started_tick = GetTickCount64();
    }
    if (state == CHUNK_DONE || state == CHUNK_FAILED) {
        item->finished_tick = GetTickCount64();
    }
    if (detail != NULL) {
        safe_copy(item->detail, sizeof(item->detail), detail);
    } else if (state == CHUNK_DONE || state == CHUNK_DOWNLOADING) {
        format_speed(item->downloaded_size,
                     item->started_tick,
                     item->finished_tick ? item->finished_tick : GetTickCount64(),
                     item->detail,
                     sizeof(item->detail));
    }

    for (i = 0; i < total; ++i) {
        bytes += plan->chunks[i].downloaded_size;
        if (plan->chunks[i].state == CHUNK_DONE) {
            finished++;
        } else if (plan->chunks[i].state == CHUNK_FAILED) {
            failed++;
        }
    }
    LeaveCriticalSection(&plan->chunk_lock);

    log_message(plan->logger,
                "chunk %s state=%s attempts=%d status=%lu error=%lu bytes=%llu expected=%llu detail=%s",
                item->name,
                chunk_state_name(state),
                item->attempts,
                (unsigned long) status_code,
                (unsigned long) error_code,
                downloaded_size,
                item->expected_size,
                item->detail);
    ui_update_patch(plan->ui, finished, failed, bytes);
}

static void ui_chunk_progress(void *ctx, const char *label, unsigned long long downloaded, unsigned long long total, int done) {
    chunk_progress_ctx_t *progress = (chunk_progress_ctx_t *) ctx;
    download_plan_t *plan = progress->plan;
    unsigned long long finished = 0;
    unsigned long long failed = 0;
    unsigned long long bytes = 0;
    unsigned long long i;
    chunk_item_t *item;

    (void) label;
    (void) done;
    EnterCriticalSection(&plan->chunk_lock);
    item = &plan->chunks[progress->chunk_index];
    item->state = CHUNK_DOWNLOADING;
    item->downloaded_size = downloaded;
    if (item->started_tick == 0) {
        item->started_tick = GetTickCount64();
    }
    if (total > 0) {
        item->expected_size = total;
    }
    format_speed(item->downloaded_size, item->started_tick, GetTickCount64(), item->detail, sizeof(item->detail));
    for (i = 0; i < plan->manifest.chunk_count; ++i) {
        bytes += plan->chunks[i].downloaded_size;
        if (plan->chunks[i].state == CHUNK_DONE) {
            finished++;
        } else if (plan->chunks[i].state == CHUNK_FAILED) {
            failed++;
        }
    }
    LeaveCriticalSection(&plan->chunk_lock);
    ui_update_patch(plan->ui, finished, failed, bytes);
}

static int download_chunk_to_target(download_plan_t *plan, int chunk_index, const char *asset_name, int *attempts_out,
                                    DWORD *status_out, DWORD *error_out, unsigned long long *size_out) {
    file_logger_t *logger = plan->logger;
    const manifest_t *manifest = &plan->manifest;
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_PROXY];
    int proxy_index = 0;
    int attempts = 0;
    int ok = -1;

    if (build_raw_asset_url(manifest->repo, manifest->ref, manifest->package_id, asset_name, origin_url, sizeof(origin_url)) != 0) {
        log_message(logger, "chunk asset url is too long for %s", asset_name);
        return -1;
    }
    while (extract_proxy(manifest->proxy_list[0] ? manifest->proxy_list : DEFAULT_PROXY_LIST, proxy_index++, proxy, sizeof(proxy)) == 0) {
        HINTERNET session = NULL;
        HINTERNET connect = NULL;
        HINTERNET request = NULL;
        wchar_t host[MAX_STR];
        wchar_t path[MAX_STR];
        INTERNET_PORT port = 0;
        int secure = 0;
        BYTE buffer[BUFFER_SIZE];
        DWORD bytes_read = 0;
        DWORD status_code_size = sizeof(DWORD);
        DWORD content_length_dword = sizeof(DWORD);
        DWORD content_length_value = 0;
        DWORD status_code = 0;
        unsigned long long downloaded = 0;
        chunk_progress_ctx_t progress_ctx;

        attempts++;
        if (build_request_url(proxy, origin_url, request_url, sizeof(request_url)) != 0) {
            log_message(logger, "chunk request url is too long for %s via %s", asset_name, proxy);
            continue;
        }
        log_message(logger, "downloading %s via %s", asset_name, proxy);

        if (split_url(request_url, host, MAX_STR, path, MAX_STR, &port, &secure) != 0) {
            continue;
        }
        session = WinHttpOpen(L"ChunkInstaller/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == NULL) {
            goto request_cleanup;
        }
        connect = WinHttpConnect(session, host, port, 0);
        if (connect == NULL) {
            goto request_cleanup;
        }
        request = WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0);
        if (request == NULL) {
            goto request_cleanup;
        }
        if (!WinHttpSendRequest(request, L"User-Agent: shaiya-installer/1.0\r\n", (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, NULL)) {
            goto request_cleanup;
        }
        if (!WinHttpQueryHeaders(request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status_code,
                                 &status_code_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            goto request_cleanup;
        }
        *status_out = status_code;
        if (status_code != 200) {
            goto request_cleanup;
        }
        if (WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                &content_length_value,
                                &content_length_dword,
                                WINHTTP_NO_HEADER_INDEX)) {
            plan->chunks[chunk_index].expected_size = (unsigned long long) content_length_value;
        }

        progress_ctx.plan = plan;
        progress_ctx.chunk_index = chunk_index;
        while (WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0) {
            if (write_chunk_data(plan, chunk_index, buffer, bytes_read, downloaded) != 0) {
                goto request_cleanup;
            }
            downloaded += bytes_read;
            ui_chunk_progress(&progress_ctx, asset_name, downloaded, plan->chunks[chunk_index].expected_size, 0);
        }
        ui_chunk_progress(&progress_ctx, asset_name, downloaded, plan->chunks[chunk_index].expected_size, 1);
        *attempts_out = attempts;
        *error_out = 0;
        *size_out = downloaded;
        ok = 0;

request_cleanup:
        if (request != NULL) {
            WinHttpCloseHandle(request);
        }
        if (connect != NULL) {
            WinHttpCloseHandle(connect);
        }
        if (session != NULL) {
            WinHttpCloseHandle(session);
        }
        if (ok == 0) {
            return 0;
        }
    }

    *attempts_out = attempts;
    *error_out = GetLastError();
    *size_out = 0;
    return -1;
}

static DWORD WINAPI chunk_worker_thread(LPVOID param) {
    worker_ctx_t *ctx = (worker_ctx_t *) param;
    download_plan_t *plan = ctx->plan;

    for (;;) {
        LONG idx = InterlockedIncrement(&plan->next_index) - 1;
        char asset_name[64];
        int attempts = 0;
        DWORD status_code = 0;
        DWORD error_code = 0;
        unsigned long long size_out = 0;

        if ((unsigned long long) idx >= plan->manifest.chunk_count) {
            break;
        }

        if (checked_format(asset_name, sizeof(asset_name), plan->manifest.chunk_pattern, (int) idx) != 0) {
            chunk_set_state(plan, (int) idx, CHUNK_FAILED, 0, 0, 0, "asset name too long");
            InterlockedExchange(&plan->failed_flag, 1);
            continue;
        }
        chunk_set_state(plan, (int) idx, CHUNK_DOWNLOADING, 0, 0, 0, "0.00 KB/s");
        if (download_chunk_to_target(plan, (int) idx, asset_name, &attempts, &status_code, &error_code, &size_out) == 0) {
            plan->chunks[idx].attempts = attempts;
            chunk_set_state(plan, (int) idx, CHUNK_DONE, status_code, 0, size_out, NULL);
            InterlockedIncrement(&plan->finished_count);
        } else {
            plan->chunks[idx].attempts = attempts;
            chunk_set_state(plan, (int) idx, CHUNK_FAILED, status_code, error_code, size_out, "download failed");
            InterlockedExchange(&plan->failed_flag, 1);
        }
    }

    return 0;
}

int run_parallel_downloads(download_plan_t *plan) {
    HANDLE *threads;
    worker_ctx_t *contexts;
    int created = 0;
    int i;
    int result = -1;

    threads = (HANDLE *) calloc(plan->jobs, sizeof(HANDLE));
    contexts = (worker_ctx_t *) calloc(plan->jobs, sizeof(worker_ctx_t));
    if (threads == NULL || contexts == NULL) {
        free(threads);
        free(contexts);
        return -1;
    }

    for (i = 0; i < plan->jobs; ++i) {
        contexts[i].plan = plan;
        contexts[i].worker_id = i + 1;
        threads[i] = CreateThread(NULL, 0, chunk_worker_thread, &contexts[i], 0, NULL);
        if (threads[i] == NULL) {
            InterlockedExchange(&plan->failed_flag, 1);
            break;
        }
        created++;
    }

    if (created > 0) {
        WaitForMultipleObjects(created, threads, TRUE, INFINITE);
    }
    result = (created == plan->jobs && InterlockedCompareExchange(&plan->failed_flag, 0, 0) == 0) ? 0 : -1;

    for (i = 0; i < created; ++i) {
        if (threads[i] != NULL) {
            CloseHandle(threads[i]);
        }
    }
    free(threads);
    free(contexts);
    return result;
}

void cleanup_temp_file(file_logger_t *logger, const char *temp_file_path) {
    if (DeleteFileA(temp_file_path)) {
        log_message(logger, "removed temp file %s", temp_file_path);
    }
}

int launch_program(file_logger_t *logger, const char *path) {
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
        log_message(logger, "failed to resolve full path for %s", path);
        return -1;
    }

    log_message(logger, "launch target: %s", full_path);
    if (checked_format(command, sizeof(command), "\"%s\"", full_path) != 0) {
        log_message(logger, "command line too long for %s", full_path);
        return -1;
    }
    if (CreateProcessA(full_path, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }

    error_code = GetLastError();
    log_message(logger, "CreateProcess failed for %s, error=%lu", full_path, (unsigned long) error_code);

    memset(&exec_info, 0, sizeof(exec_info));
    exec_info.cbSize = sizeof(exec_info);
    exec_info.fMask = SEE_MASK_NOCLOSEPROCESS;
    exec_info.lpFile = full_path;
    exec_info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&exec_info)) {
        error_code = GetLastError();
        log_message(logger, "ShellExecuteEx failed for %s, error=%lu", full_path, (unsigned long) error_code);
        return -1;
    }
    if (exec_info.hProcess != NULL) {
        CloseHandle(exec_info.hProcess);
    }
    return 0;
}

#endif
