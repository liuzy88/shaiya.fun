#include "install.h"

#ifdef _WIN32

file_logger_t g_logger;
ui_state_t g_ui;

static int parse_jobs_arg(const char *value) {
    int jobs = atoi(value);
    if (jobs <= 0) {
        jobs = DEFAULT_JOBS;
    }
    if (jobs > 16) {
        jobs = 16;
    }
    return jobs;
}

int main(int argc, char **argv) {
    manifest_t manifest;
    download_plan_t plan;
    char package_id[MAX_PATH] = "";
    char workspace_root[MAX_PATH] = "";
    char temp_file_path[MAX_PATH] = "";
    char merged_path[MAX_PATH];
    char actual_md5[33];
    unsigned long long free_bytes = 0;
    unsigned long long required_bytes = 0;
    int jobs = DEFAULT_JOBS;
    int i;

    memset(&plan, 0, sizeof(plan));
    plan.temp_file = INVALID_HANDLE_VALUE;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--jobs") == 0 && i + 1 < argc) {
            jobs = parse_jobs_arg(argv[++i]);
        } else if (argv[i][0] != '-') {
            safe_copy(package_id, sizeof(package_id), argv[i]);
        }
    }
    if (init_logger(&g_logger) != 0) {
        g_logger.file = INVALID_HANDLE_VALUE;
    }
    init_ui(&g_ui, jobs);
    log_message(&g_logger, "ChunkInstaller %s", INSTALLER_VERSION);
    log_message(&g_logger, "log file: %s", g_logger.path[0] ? g_logger.path : "(disabled)");

    if (package_id[0] != '\0') {
        log_message(&g_logger, "mode=package package_id=%s jobs=%d", package_id, jobs);
        ui_print_line("%s==> 1/4 [META]%s resolving...", ui_color_stage(), ui_reset());
        ui_print_line("    %ssource:%s package %s", ui_color_dim(), ui_reset(), package_id);
        if (download_manifest_with_fallback(&g_logger, DEFAULT_REPO, DEFAULT_REF, package_id, DEFAULT_PROXY_LIST, &manifest) != 0) {
            ui_print_line("%s==> 1/4 [META]%s error", ui_color_err(), ui_reset());
            close_logger(&g_logger);
            return 1;
        }
        if (_stricmp(manifest.package_id, package_id) != 0) {
            log_message(&g_logger, "manifest package_id mismatch");
            ui_print_line("%s==> 1/4 [META]%s package_id mismatch", ui_color_err(), ui_reset());
            close_logger(&g_logger);
            return 1;
        }
    } else {
        log_message(&g_logger, "mode=default jobs=%d", jobs);
        ui_print_line("%s==> 1/4 [META]%s resolving...", ui_color_stage(), ui_reset());
        ui_print_line("    %ssource:%s manifest.txt", ui_color_dim(), ui_reset());
        if (download_default_manifest_with_fallback(&g_logger, DEFAULT_REPO, DEFAULT_REF, DEFAULT_PROXY_LIST, &manifest) != 0) {
            ui_print_line("%s==> 1/4 [META]%s error", ui_color_err(), ui_reset());
            close_logger(&g_logger);
            return 1;
        }
    }
    ui_print_line("%s==> 1/4 [META]%s done", ui_color_ok(), ui_reset());
    ui_print_blank();
    ui_print_line("%s==> 2/4 [PART]%s inspecting...", ui_color_stage(), ui_reset());
    ui_print_line("    %spackage:%s %s", ui_color_dim(), ui_reset(), manifest.package_id);
    ui_print_line("    %starget :%s %s", ui_color_dim(), ui_reset(), manifest.file_name);
    ui_print_line("    %schunks :%s %llu", ui_color_dim(), ui_reset(), manifest.chunk_count);
    ui_print_line("    %ssplit  :%s %llu bytes", ui_color_dim(), ui_reset(), manifest.chunk_size);
    ui_print_line("    %stotal  :%s %llu bytes", ui_color_dim(), ui_reset(), manifest.total_size);
    if (choose_workspace_root(&manifest, workspace_root, sizeof(workspace_root), &free_bytes, &required_bytes) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s insufficient disk space", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }
    ui_print_line("    %swork   :%s %s", ui_color_dim(), ui_reset(), workspace_root);
    log_message(&g_logger, "workspace selected: %s free=%llu required=%llu", workspace_root, free_bytes, required_bytes);

    if (join_path(temp_file_path, sizeof(temp_file_path), workspace_root, manifest.package_id) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s error building temp path", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }
    if (join_path(merged_path, sizeof(merged_path), workspace_root, manifest.file_name) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s error building output path", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }

    if (ensure_directory_recursive(workspace_root) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s error creating workspace", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }
    if (create_temp_output_file(&g_logger, temp_file_path, manifest.total_size, &plan.temp_file) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s error preparing temp file", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }

    plan.manifest = manifest;
    plan.jobs = jobs;
    plan.chunks = (chunk_item_t *) calloc((size_t) manifest.chunk_count, sizeof(chunk_item_t));
    plan.ui = &g_ui;
    plan.logger = &g_logger;
    safe_copy(plan.temp_file_path, sizeof(plan.temp_file_path), temp_file_path);
    InitializeCriticalSection(&plan.chunk_lock);
    InitializeCriticalSection(&plan.file_lock);
    if (plan.chunks == NULL) {
        CloseHandle(plan.temp_file);
        cleanup_temp_file(&g_logger, temp_file_path);
        close_logger(&g_logger);
        return 1;
    }
    for (i = 0; (unsigned long long) i < manifest.chunk_count; ++i) {
        plan.chunks[i].index = i;
        if (checked_format(plan.chunks[i].name, sizeof(plan.chunks[i].name), manifest.chunk_pattern, i) != 0) {
            ui_print_line("%s==> 2/4 [PART]%s invalid chunk name", ui_color_err(), ui_reset());
            CloseHandle(plan.temp_file);
            cleanup_temp_file(&g_logger, temp_file_path);
            free(plan.chunks);
            DeleteCriticalSection(&plan.chunk_lock);
            DeleteCriticalSection(&plan.file_lock);
            close_logger(&g_logger);
            return 1;
        }
        plan.chunks[i].state = CHUNK_QUEUED;
        plan.chunks[i].expected_size =
            (i == (int) manifest.chunk_count - 1)
                ? (manifest.total_size - (manifest.chunk_size * (unsigned long long) i))
                : manifest.chunk_size;
        safe_copy(plan.chunks[i].detail, sizeof(plan.chunks[i].detail), "queued");
    }

    ui_print_line("%s==> 2/4 [PART]%s downloading...", ui_color_stage(), ui_reset());
    ui_begin_patch(&g_ui, plan.chunks, manifest.chunk_count, manifest.total_size, jobs);
    if (run_parallel_downloads(&plan) != 0) {
        ui_finish_patch(&g_ui);
        ui_print_line("%s==> 2/4 [PART]%s error", ui_color_err(), ui_reset());
        CloseHandle(plan.temp_file);
        cleanup_temp_file(&g_logger, temp_file_path);
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    ui_finish_patch(&g_ui);
    ui_print_line("%s==> 2/4 [PART]%s done", ui_color_ok(), ui_reset());

    ui_print_blank();
    CloseHandle(plan.temp_file);
    plan.temp_file = INVALID_HANDLE_VALUE;
    ui_print_line("%s==> 3/4 [VERIFY]%s validating...", ui_color_stage(), ui_reset());
    if (compute_file_md5_hex(temp_file_path, actual_md5, sizeof(actual_md5)) != 0) {
        ui_print_line("%s==> 3/4 [VERIFY]%s error", ui_color_err(), ui_reset());
        cleanup_temp_file(&g_logger, temp_file_path);
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    if (_stricmp(actual_md5, manifest.md5) != 0) {
        log_message(&g_logger, "md5 mismatch expected=%s actual=%s", manifest.md5, actual_md5);
        ui_print_line("%s==> 3/4 [VERIFY]%s checksum mismatch", ui_color_err(), ui_reset());
        cleanup_temp_file(&g_logger, temp_file_path);
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    if (!MoveFileExA(temp_file_path, merged_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        log_message(&g_logger, "failed to finalize output %s error=%lu", merged_path, (unsigned long) GetLastError());
        ui_print_line("%s==> 3/4 [VERIFY]%s error finalizing output", ui_color_err(), ui_reset());
        cleanup_temp_file(&g_logger, temp_file_path);
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    ui_print_line("%s==> 3/4 [VERIFY]%s done", ui_color_ok(), ui_reset());
    ui_print_line("    %soutput :%s %s", ui_color_dim(), ui_reset(), merged_path);

    ui_print_blank();
    ui_print_line("%s==> 4/4 [RUN]%s launching...", ui_color_stage(), ui_reset());
    if (launch_program(&g_logger, merged_path) != 0) {
        ui_print_line("%s==> 4/4 [RUN]%s error", ui_color_err(), ui_reset());
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    ui_print_line("%s==> 4/4 [RUN]%s done", ui_color_ok(), ui_reset());

    free(plan.chunks);
    DeleteCriticalSection(&plan.chunk_lock);
    DeleteCriticalSection(&plan.file_lock);
    close_logger(&g_logger);
    return 0;
}

#else
int main(void) {
    return 1;
}
#endif
