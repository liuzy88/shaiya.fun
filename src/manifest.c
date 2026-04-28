#include "install.h"

#ifdef _WIN32

static int manifest_set_value(manifest_t *manifest, const char *key, const char *value) {
    if (strcmp(key, "repo") == 0) {
        safe_copy(manifest->repo, sizeof(manifest->repo), value);
    } else if (strcmp(key, "ref") == 0) {
        safe_copy(manifest->ref, sizeof(manifest->ref), value);
    } else if (strcmp(key, "package_id") == 0) {
        safe_copy(manifest->package_id, sizeof(manifest->package_id), value);
    } else if (strcmp(key, "md5") == 0) {
        safe_copy(manifest->md5, sizeof(manifest->md5), value);
    } else if (strcmp(key, "file_name") == 0) {
        safe_copy(manifest->file_name, sizeof(manifest->file_name), value);
    } else if (strcmp(key, "chunk_pattern") == 0) {
        safe_copy(manifest->chunk_pattern, sizeof(manifest->chunk_pattern), value);
    } else if (strcmp(key, "proxy_list") == 0) {
        safe_copy(manifest->proxy_list, sizeof(manifest->proxy_list), value);
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
           is_safe_output_file_name(manifest->file_name) && strcmp(manifest->chunk_pattern, "part-%03d.bin") == 0 &&
           manifest->total_size != 0 && manifest->chunk_size != 0 && manifest->chunk_count != 0;
}

static int parse_manifest_buffer(const char *buffer, size_t size, manifest_t *manifest) {
    char *copy;
    char *line_ptr;
    char *next = NULL;

    memset(manifest, 0, sizeof(*manifest));
    copy = (char *) malloc(size + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, buffer, size);
    copy[size] = '\0';

    line_ptr = strtok_s(copy, "\r\n", &next);
    while (line_ptr != NULL) {
        char *eq = strchr(line_ptr, '=');
        if (eq != NULL && eq != line_ptr) {
            *eq = '\0';
            manifest_set_value(manifest, line_ptr, eq + 1);
        }
        line_ptr = strtok_s(NULL, "\r\n", &next);
    }

    free(copy);
    return manifest_looks_valid(manifest) ? 0 : -1;
}

int extract_proxy(const char *proxy_list, int index, char *proxy, size_t proxy_size) {
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

static int build_api_asset_url(const char *repo, const char *ref, const char *package_id, const char *asset_name, char *url,
                               size_t url_size) {
    return checked_format(url, url_size, "https://api.github.com/repos/%s/contents/%s/%s?ref=%s", repo, package_id,
                          asset_name, ref);
}

static int build_api_root_manifest_url(const char *repo, const char *ref, char *url, size_t url_size) {
    return checked_format(url, url_size, "https://api.github.com/repos/%s/contents/manifest.txt?ref=%s", repo, ref);
}

int build_raw_asset_url(const char *repo, const char *ref, const char *package_id, const char *asset_name, char *url,
                        size_t url_size) {
    return checked_format(url, url_size, "https://raw.githubusercontent.com/%s/%s/%s/%s", repo, ref, package_id,
                          asset_name);
}

int build_request_url(const char *proxy, const char *origin_url, char *url, size_t url_size) {
    if (_stricmp(proxy, "DIRECT") == 0) {
        safe_copy(url, url_size, origin_url);
        return strlen(origin_url) < url_size ? 0 : -1;
    } else {
        return checked_format(url, url_size, "%s%s", proxy, origin_url);
    }
}

static int download_manifest_from_url(file_logger_t *logger, const char *url, const char *preview_label, manifest_t *manifest) {
    http_buffer_t response;
    manifest_progress_ctx_t progress_ctx;

    progress_ctx.ui = &g_ui;
    progress_ctx.label = preview_label;
    if (download_response(logger, url, &response, 1, ui_manifest_progress, &progress_ctx, "manifest.txt") != 0) {
        return -1;
    }
    log_buffer_preview(logger, preview_label, response.data, response.size);
    if (strstr(response.data, "<html") != NULL || strstr(response.data, "<!DOCTYPE html") != NULL) {
        free(response.data);
        return -1;
    }
    if (parse_manifest_buffer(response.data, response.size, manifest) != 0) {
        free(response.data);
        return -1;
    }
    log_message(logger,
                "manifest parsed: package=%s file=%s chunks=%llu chunk_size=%llu total_size=%llu",
                manifest->package_id,
                manifest->file_name,
                manifest->chunk_count,
                manifest->chunk_size,
                manifest->total_size);
    free(response.data);
    return 0;
}

int download_manifest_with_fallback(file_logger_t *logger, const char *repo, const char *ref, const char *package_id,
                                           const char *proxy_list, manifest_t *manifest) {
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_PROXY];
    int proxy_index = 0;

    if (build_api_asset_url(repo, ref, package_id, "manifest.txt", origin_url, sizeof(origin_url)) != 0) {
        log_message(logger, "manifest api url is too long");
        return -1;
    }
    while (extract_proxy(proxy_list, proxy_index++, proxy, sizeof(proxy)) == 0) {
        if (build_request_url(proxy, origin_url, request_url, sizeof(request_url)) != 0) {
            log_message(logger, "manifest request url is too long via %s", proxy);
            continue;
        }
        log_message(logger, "downloading manifest.txt via %s", proxy);
        if (download_manifest_from_url(logger, request_url, "manifest", manifest) == 0) {
            return 0;
        }
    }
    return -1;
}

int download_default_manifest_with_fallback(file_logger_t *logger, const char *repo, const char *ref, const char *proxy_list,
                                                   manifest_t *manifest) {
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_PROXY];
    int proxy_index = 0;

    if (build_api_root_manifest_url(repo, ref, origin_url, sizeof(origin_url)) != 0) {
        log_message(logger, "default manifest api url is too long");
        return -1;
    }
    while (extract_proxy(proxy_list, proxy_index++, proxy, sizeof(proxy)) == 0) {
        if (build_request_url(proxy, origin_url, request_url, sizeof(request_url)) != 0) {
            log_message(logger, "default manifest request url is too long via %s", proxy);
            continue;
        }
        log_message(logger, "downloading default manifest.txt via %s", proxy);
        if (download_manifest_from_url(logger, request_url, "default manifest", manifest) == 0) {
            return 0;
        }
    }
    return -1;
}

#endif
