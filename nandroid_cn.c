#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "libcrecovery/common.h"

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include <sys/vfs.h>

#include "extendedcommands.h"
#include "recovery_settings.h"
#include "nandroid.h"
#include "mounts.h"

#include "flashutils/flashutils.h"
#include <libgen.h>

#ifdef NEED_SELINUX_FIX
#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/android.h>
#endif

void nandroid_generate_timestamp_path(char* backup_path) {
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL) {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        sprintf(backup_path, "/sdcard/clockworkmod/backup/%ld", tp.tv_sec);
    } else {
        strftime(backup_path, PATH_MAX, "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
    }
}

static void ensure_directory(const char* dir) {
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s && chmod 777 %s", dir, dir);
    __system(tmp);
}

static int print_and_error(const char* message) {
    ui_print("%s\n", message);
    return 1;
}

static int nandroid_backup_bitfield = 0;
#define NANDROID_FIELD_DEDUPE_CLEARED_SPACE 1
static int nandroid_files_total = 0;
static int nandroid_files_count = 0;
static void nandroid_callback(const char* filename) {
    if (filename == NULL)
        return;
    const char* justfile = basename(filename);
    char tmp[PATH_MAX];
    strcpy(tmp, justfile);
    if (tmp[strlen(tmp) - 1] == '\n')
        tmp[strlen(tmp) - 1] = '\0';
    tmp[ui_get_text_cols() - 1] = '\0';
    nandroid_files_count++;
    ui_increment_frame();
    ui_nice_print("%s\n", tmp);
    if (!ui_was_niced() && nandroid_files_total != 0)
        ui_set_progress((float)nandroid_files_count / (float)nandroid_files_total);
    if (!ui_was_niced())
        ui_delete_line();
}

static void compute_directory_stats(const char* directory) {
    char tmp[PATH_MAX];
    char count_text[100];

    // reset file count if we ever return before setting it
    nandroid_files_count = 0;
    nandroid_files_total = 0;

    sprintf(tmp, "find %s | %s wc -l > /tmp/dircount", directory, strcmp(directory, "/data") == 0 && is_data_media() ? "grep -v /data/media |" : "");
    __system(tmp);

    FILE* f = fopen("/tmp/dircount", "r");
    if (f == NULL)
        return;

    if (fgets(count_text, sizeof(count_text), f) == NULL) {
        fclose(f);
        return;
    }

    size_t len = strlen(count_text);
    if (count_text[len - 1] == '\n')
        count_text[len - 1] = '\0';

    fclose(f);
    nandroid_files_total = atoi(count_text);
    ui_reset_progress();
    ui_show_progress(1, 0);
}

typedef void (*file_event_callback)(const char* filename);
typedef int (*nandroid_backup_handler)(const char* backup_path, const char* backup_file_image, int callback);

static int mkyaffs2image_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd %s ; mkyaffs2image . %s.img ; exit $?", backup_path, backup_file_image);

    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute mkyaffs2image.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = '\0';
        if (callback)
            nandroid_callback(tmp);
    }

    return __pclose(fp);
}

static int do_tar_compress(char* command, int callback) {
    char buf[PATH_MAX];

    set_perf_mode(1);
    FILE *fp = __popen(command, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar command!\n");
        set_perf_mode(0);
        return -1;
    }

    while (fgets(buf, PATH_MAX, fp) != NULL) {
        buf[PATH_MAX - 1] = '\0';
        if (callback)
            nandroid_callback(buf);
    }

    set_perf_mode(0);
    return __pclose(fp);
}

static int tar_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
#ifdef BOARD_RECOVERY_USE_BBTAR
    sprintf(tmp, "cd $(dirname %s) ; touch %s.tar ; set -o pipefail ; (tar cv --exclude=data/data/com.google.android.music/files/* %s $(basename %s) | split -a 1 -b 1000000000 /proc/self/fd/0 %s.tar.) 2> /proc/self/fd/1 ; exit $?", backup_path, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path, backup_file_image);
#else
    sprintf(tmp, "cd $(dirname %s) ; touch %s.tar ; set -o pipefail ; (tar -csv --exclude=data/data/com.google.android.music/files/* %s $(basename %s) | split -a 1 -b 1000000000 /proc/self/fd/0 %s.tar.) 2> /proc/self/fd/1 ; exit $?", backup_path, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path, backup_file_image);
#endif

    return do_tar_compress(tmp, callback);
}

static int tar_gzip_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
#ifdef BOARD_RECOVERY_USE_BBTAR
    sprintf(tmp, "cd $(dirname %s) ; touch %s.tar.gz ; set -o pipefail ; (tar cv --exclude=data/data/com.google.android.music/files/* %s $(basename %s) | pigz -c | split -a 1 -b 1000000000 /proc/self/fd/0 %s.tar.gz.) 2> /proc/self/fd/1 ; exit $?", backup_path, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path, backup_file_image);
#else
    sprintf(tmp, "cd $(dirname %s) ; touch %s.tar.gz ; set -o pipefail ; (tar -csv --exclude=data/data/com.google.android.music/files/* %s $(basename %s) | pigz -c | split -a 1 -b 1000000000 /proc/self/fd/0 %s.tar.gz.) 2> /proc/self/fd/1 ; exit $?", backup_path, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path, backup_file_image);
#endif

    return do_tar_compress(tmp, callback);
}

static int tar_dump_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
#ifdef BOARD_RECOVERY_USE_BBTAR
    sprintf(tmp, "cd $(dirname %s); set -o pipefail ; tar cv --exclude=data/data/com.google.android.music/files/* %s $(basename %s) 2> /dev/null | cat", backup_path, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path);
#else
    sprintf(tmp, "cd $(dirname %s); set -o pipefail ; tar -csv --exclude=data/data/com.google.android.music/files/* %s $(basename %s) 2> /dev/null | cat", backup_path, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path);
#endif

    return __system(tmp);
}

void nandroid_dedupe_gc(const char* blob_dir) {
    char backup_dir[PATH_MAX];
    strcpy(backup_dir, blob_dir);
    char *d = dirname(backup_dir);
    strcpy(backup_dir, d);
    strcat(backup_dir, "/backup");
    ui_print("Freeing space...\n");
    char tmp[PATH_MAX];
    sprintf(tmp, "dedupe gc %s $(find %s -name '*.dup')", blob_dir, backup_dir);
    __system(tmp);
    ui_print("Done freeing space.\n");
}

static int dedupe_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    char blob_dir[PATH_MAX];
    strcpy(blob_dir, backup_file_image);
    char *d = dirname(blob_dir);
    strcpy(blob_dir, d);
    d = dirname(blob_dir);
    strcpy(blob_dir, d);
    d = dirname(blob_dir);
    strcpy(blob_dir, d);
    strcat(blob_dir, "/blobs");
    ensure_directory(blob_dir);

    if (!(nandroid_backup_bitfield & NANDROID_FIELD_DEDUPE_CLEARED_SPACE)) {
        nandroid_backup_bitfield |= NANDROID_FIELD_DEDUPE_CLEARED_SPACE;
        nandroid_dedupe_gc(blob_dir);
    }

    sprintf(tmp, "dedupe c %s %s %s.dup %s", backup_path, blob_dir, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "./media" : "");

    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute dedupe.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = '\0';
        if (callback)
            nandroid_callback(tmp);
    }

    return __pclose(fp);
}

static void build_configuration_path(char *path_buf, const char *file) {
    //sprintf(path_buf, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), file);
    sprintf(path_buf, "%s/%s", get_primary_storage_path(), file);
}

static nandroid_backup_handler default_backup_handler = tar_compress_wrapper;
static char forced_backup_format[5] = "";
void nandroid_force_backup_format(const char* fmt) {
    strcpy(forced_backup_format, fmt);
}
static void refresh_default_backup_handler() {
    char fmt[5];
    if (strlen(forced_backup_format) > 0) {
        strcpy(fmt, forced_backup_format);
    } else {
        char path[PATH_MAX];

        build_configuration_path(path, NANDROID_BACKUP_FORMAT_FILE);
        ensure_path_mounted(path);
        FILE* f = fopen(path, "r");
        if (NULL == f) {
            default_backup_handler = tar_compress_wrapper;
            return;
        }
        fread(fmt, 1, sizeof(fmt), f);
        fclose(f);
    }
    fmt[3] = '\0';
    if (0 == strcmp(fmt, "dup"))
        default_backup_handler = dedupe_compress_wrapper;
    else if (0 == strcmp(fmt, "tgz"))
        default_backup_handler = tar_gzip_compress_wrapper;
    else if (0 == strcmp(fmt, "tar"))
        default_backup_handler = tar_compress_wrapper;
    else
        default_backup_handler = tar_compress_wrapper;
}

unsigned nandroid_get_default_backup_format() {
    refresh_default_backup_handler();
    if (default_backup_handler == dedupe_compress_wrapper) {
        return NANDROID_BACKUP_FORMAT_DUP;
    } else if (default_backup_handler == tar_gzip_compress_wrapper) {
        return NANDROID_BACKUP_FORMAT_TGZ;
    } else {
        return NANDROID_BACKUP_FORMAT_TAR;
    }
}

static nandroid_backup_handler get_backup_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
    const MountedVolume *mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
        return default_backup_handler;
    }

    if (strlen(forced_backup_format) > 0)
        return default_backup_handler;

    // Disable tar backups of yaffs2 by default
    char prefer_tar[PROPERTY_VALUE_MAX];
    property_get("ro.cwm.prefer_tar", prefer_tar, "false");
    if (strcmp("yaffs2", mv->filesystem) == 0 && strcmp("false", prefer_tar) == 0) {
        return mkyaffs2image_wrapper;
    }

    return default_backup_handler;
}

int nandroid_backup_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char name[PATH_MAX];
    char tmp[PATH_MAX];
    strcpy(name, basename(mount_point));

    struct stat file_info;
    build_configuration_path(tmp, NANDROID_HIDE_PROGRESS_FILE);
    ensure_path_mounted(tmp);
    int callback = stat(tmp, &file_info) != 0;

    ui_print("正在备份%s...\n", name);
    if (0 != (ret = ensure_path_mounted(mount_point) != 0)) {
        ui_print("不能挂载%s!\n", mount_point);
        return ret;
    }
    compute_directory_stats(mount_point);
    scan_mounted_volumes();
    Volume *v = volume_for_path(mount_point);
    const MountedVolume *mv = NULL;
    if (v != NULL)
        mv = find_mounted_volume_by_mount_point(v->mount_point);

    if (strcmp(backup_path, "-") == 0)
        sprintf(tmp, "/proc/self/fd/1");
    else if (mv == NULL || mv->filesystem == NULL)
        sprintf(tmp, "%s/%s.auto", backup_path, name);
    else
        sprintf(tmp, "%s/%s.%s", backup_path, name, mv->filesystem);
    nandroid_backup_handler backup_handler = get_backup_handler(mount_point);

    if (backup_handler == NULL) {
        ui_print("Error finding an appropriate backup handler.\n");
        return -2;
    }
    ret = backup_handler(mount_point, tmp, callback);
#ifdef NEED_SELINUX_FIX
    if (0 != ret || strcmp(backup_path, "-") == 0) {
        LOGI("跳过selinux context备份!\n");
    }
    else if (0 == strcmp(mount_point, "/data") ||
                0 == strcmp(mount_point, "/system") ||
                0 == strcmp(mount_point, "/cache"))
    {
        ui_print("备份selinux context...\n");
        sprintf(tmp, "%s/%s.context", backup_path, name);
        if (bakupcon_to_file(mount_point, tmp) < 0)
            LOGE("备份selinux context出错!\n");
        else
            ui_print("备份selinux context完毕.\n");
    }
#endif
    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    if (0 != ret) {
        ui_print("备份%s时出错!\n", mount_point);
        return ret;
    }
    ui_print("备份%s完毕.\n", name);
    return 0;
}

int nandroid_backup_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists before attempting anything...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

    // see if we need a raw backup (mtd)
    char tmp[PATH_MAX];
    int ret;
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        const char* name = basename(root);
        if (strcmp(backup_path, "-") == 0)
            strcpy(tmp, "/proc/self/fd/1");
        else
            sprintf(tmp, "%s/%s.img", backup_path, name);

        ui_print("正在备份%s镜像...\n", name);
        if (0 != (ret = backup_raw_partition(vol->fs_type, vol->blk_device, tmp))) {
            ui_print("备份%s镜像时出错!", name);
            return ret;
        }

        ui_print("备份%s镜像完毕.\n", name);
        return 0;
    }

    return nandroid_backup_partition_extended(backup_path, root, 1);
}

int nandroid_backup(const char* backup_path) {
    nandroid_backup_bitfield = 0;
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    refresh_default_backup_handler();

    if (ensure_path_mounted(backup_path) != 0) {
        return print_and_error("Can't mount backup path.\n");
    }

    Volume* volume;
    if (is_data_media_volume_path(backup_path))
        volume = volume_for_path("/data");
    else
        volume = volume_for_path(backup_path);
    if (NULL == volume)
        return print_and_error("Unable to find volume for backup path.\n");
    int ret;
    struct statfs sfs;
    struct stat s;
    if (NULL != volume) {
        if (0 != (ret = statfs(volume->mount_point, &sfs)))
            return print_and_error("Unable to stat backup path.\n");
        uint64_t bavail = sfs.f_bavail;
        uint64_t bsize = sfs.f_bsize;
        uint64_t sdcard_free = bavail * bsize;
        uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
        ui_print("SD Card space free: %lluMB\n", sdcard_free_mb);
        if (sdcard_free_mb < 150)
            ui_print("There may not be enough free space to complete backup... continuing...\n");
    }
    char tmp[PATH_MAX];
    ensure_directory(backup_path);

    if (0 != (ret = nandroid_backup_partition(backup_path, "/boot")))
        return ret;

    if (0 != (ret = nandroid_backup_partition(backup_path, "/recovery")))
        return ret;

    Volume *vol = volume_for_path("/wimax");
    if (vol != NULL && 0 == stat(vol->blk_device, &s)) {
        char serialno[PROPERTY_VALUE_MAX];
        ui_print("正在备份WiMAX...\n");
        serialno[0] = 0;
        property_get("ro.serialno", serialno, "");
        sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);
        ret = backup_raw_partition(vol->fs_type, vol->blk_device, tmp);
        if (0 != ret)
            return print_and_error("Error while dumping WiMAX image!\n");
    }

    if (0 != (ret = nandroid_backup_partition(backup_path, "/system")))
        return ret;

    if (volume_for_path("/preload") != NULL)
    {
        if (0 != (ret = nandroid_backup_partition(backup_path, "/preload")))
            return ret;
    }

    if (0 != (ret = nandroid_backup_partition(backup_path, "/data")))
        return ret;

    if (has_datadata()) {
        if (0 != (ret = nandroid_backup_partition(backup_path, "/datadata")))
            return ret;
    }

    if (0 != stat(get_android_secure_path(), &s)) {
        ui_print("没发现.android_secure目录. 跳过备份安装到扩展卡上的程序.\n");
    } else {
        if (0 != (ret = nandroid_backup_partition_extended(backup_path, get_android_secure_path(), 0)))
            return ret;
    }

    if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/cache", 0)))
        return ret;

    vol = volume_for_path("/sd-ext");
    if (vol == NULL || 0 != stat(vol->blk_device, &s)) {
        LOGI("没有sd-ext分区,跳过.\n");
    } else {
        if (0 != ensure_path_mounted("/sd-ext"))
            LOGI("Could not mount sd-ext. sd-ext backup may not be supported on this device. Skipping backup of sd-ext.\n");
        else if (0 != (ret = nandroid_backup_partition(backup_path, "/sd-ext")))
            return ret;
    }

    ui_print("创建md5校验文件...\n");
    sprintf(tmp, "nandroid-md5.sh %s", backup_path);
    if (0 != (ret = __system(tmp))) {
        ui_print("创建md5校验文件出错!\n");
        return ret;
    }

    sprintf(tmp, "cp /tmp/recovery.log %s/recovery.log", backup_path);
    __system(tmp);

    char base_dir[PATH_MAX];
    strcpy(base_dir, backup_path);
    char *d = dirname(base_dir);
    strcpy(base_dir, d);
    d = dirname(base_dir);
    strcpy(base_dir, d);

    sprintf(tmp, "chmod -R 777 %s ; chmod -R u+r,u+w,g+r,g+w,o+r,o+w %s ; chmod u+x,g+x,o+x %s/backup ; chmod u+x,g+x,o+x %s/blobs", backup_path, base_dir, base_dir, base_dir);
    __system(tmp);

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\n备份完毕!\n");
    return 0;
}

int nandroid_dump(const char* partition) {
    // silence our ui_print statements and other logging
    ui_set_log_stdout(0);

    nandroid_backup_bitfield = 0;
    refresh_default_backup_handler();

    // override our default to be the basic tar dumper
    default_backup_handler = tar_dump_wrapper;

    if (strcmp(partition, "boot") == 0) {
        Volume *vol = volume_for_path("/boot");
        // make sure the volume exists before attempting anything...
        if (vol == NULL || vol->fs_type == NULL)
            return 1;
        char cmd[PATH_MAX];
        sprintf(cmd, "cat %s", vol->blk_device);
        return __system(cmd);
        // return nandroid_backup_partition("-", "/boot");
    }

    if (strcmp(partition, "recovery") == 0) {
        return __system("set -o pipefail ; dump_image recovery /proc/self/fd/1 | cat");
    }

    if (strcmp(partition, "data") == 0) {
        return nandroid_backup_partition("-", "/data");
    }

    if (strcmp(partition, "system") == 0) {
        return nandroid_backup_partition("-", "/system");
    }

    return 1;
}

typedef int (*nandroid_restore_handler)(const char* backup_file_image, const char* backup_path, int callback);

static int unyaffs_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd %s ; unyaffs %s ; exit $?", backup_path, backup_file_image);
    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("不能正确执行unyaffs命令.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = '\0';
        if (callback)
            nandroid_callback(tmp);
    }

    return __pclose(fp);
}

static int do_tar_extract(char* command, int callback) {
    char buf[PATH_MAX];

    set_perf_mode(1);
    FILE *fp = __popen(command, "r");
    if (fp == NULL) {
        ui_print("不能正确执行tar命令.\n");
        set_perf_mode(0);
        return -1;
    }

    while (fgets(buf, PATH_MAX, fp) != NULL) {
        buf[PATH_MAX - 1] = '\0';
        if (callback)
            nandroid_callback(buf);
    }

    set_perf_mode(0);
    return __pclose(fp);
}

static int tar_gzip_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
#ifdef BOARD_RECOVERY_USE_BBTAR
    sprintf(tmp, "cd $(dirname %s) ; set -o pipefail ; cat %s* | pigz -d -c | tar xv ; exit $?", backup_path, backup_file_image);
#else
    sprintf(tmp, "cd $(dirname %s) ; set -o pipefail ; cat %s* | pigz -d -c | tar -xsv ; exit $?", backup_path, backup_file_image);
#endif

    return do_tar_extract(tmp, callback);
}

static int tar_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
#ifdef BOARD_RECOVERY_USE_BBTAR
    sprintf(tmp, "cd $(dirname %s) ; set -o pipefail ; cat %s* | tar xv ; exit $?", backup_path, backup_file_image);
#else
    sprintf(tmp, "cd $(dirname %s) ; set -o pipefail ; cat %s* | tar -xsv ; exit $?", backup_path, backup_file_image);
#endif

    return do_tar_extract(tmp, callback);
}

static int dedupe_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    char blob_dir[PATH_MAX];
    strcpy(blob_dir, backup_file_image);
    char *bd = dirname(blob_dir);
    strcpy(blob_dir, bd);
    bd = dirname(blob_dir);
    strcpy(blob_dir, bd);
    bd = dirname(blob_dir);
    sprintf(tmp, "dedupe x %s %s/blobs %s; exit $?", backup_file_image, bd, backup_path);

    char path[PATH_MAX];
    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("不能正确执行dedupe命令.\n");
        return -1;
    }

    while (fgets(path, PATH_MAX, fp) != NULL) {
        if (callback)
            nandroid_callback(path);
    }

    return __pclose(fp);
}

static int tar_undump_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
#ifdef BOARD_RECOVERY_USE_BBTAR
    sprintf(tmp, "cd $(dirname %s) ; tar xv ", backup_path);
#else
    sprintf(tmp, "cd $(dirname %s) ; tar -xsv ", backup_path);
#endif

    return __system(tmp);
}

static nandroid_restore_handler get_restore_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("无法定位备份卷.\n");
        return NULL;
    }
    scan_mounted_volumes();
    const MountedVolume *mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("无法找到已经挂载的: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
        return tar_extract_wrapper;
    }

    // Disable tar backups of yaffs2 by default
    char prefer_tar[PROPERTY_VALUE_MAX];
    property_get("ro.cwm.prefer_tar", prefer_tar, "false");
    if (strcmp("yaffs2", mv->filesystem) == 0 && strcmp("false", prefer_tar) == 0) {
        return unyaffs_wrapper;
    }

    return tar_extract_wrapper;
}

int nandroid_restore_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    nandroid_restore_handler restore_handler = NULL;
    const char *filesystems[] = { "yaffs2", "ext2", "ext3", "ext4", "vfat", "rfs", "f2fs", "exfat", NULL };
    const char* backup_filesystem = NULL;
    Volume *vol = volume_for_path(mount_point);
    const char *device = NULL;
    if (vol != NULL)
        device = vol->blk_device;

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s.img", backup_path, name);
    struct stat file_info;
    if (strcmp(backup_path, "-") == 0) {
        if (vol)
            backup_filesystem = vol->fs_type;
        restore_handler = tar_extract_wrapper;
        strcpy(tmp, "/proc/self/fd/0");
    } else if (0 != (ret = stat(tmp, &file_info))) {
        // can't find the backup, it may be the new backup format?
        // iterate through the backup types
        //printf("couldn't find default\n");
        const char *filesystem;
        int i = 0;
        while ((filesystem = filesystems[i]) != NULL) {
            sprintf(tmp, "%s/%s.%s.img", backup_path, name, filesystem);
            if (0 == (ret = stat(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = unyaffs_wrapper;
                break;
            }
            sprintf(tmp, "%s/%s.%s.tar", backup_path, name, filesystem);
            if (0 == (ret = stat(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = tar_extract_wrapper;
                break;
            }
            sprintf(tmp, "%s/%s.%s.tar.gz", backup_path, name, filesystem);
            if (0 == (ret = stat(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = tar_gzip_extract_wrapper;
                break;
            }
            sprintf(tmp, "%s/%s.%s.dup", backup_path, name, filesystem);
            if (0 == (ret = stat(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = dedupe_extract_wrapper;
                break;
            }
            i++;
        }

        if (backup_filesystem == NULL || restore_handler == NULL) {
            ui_print("找不到%s文件. 跳过还原%s.\n", name, mount_point);
            return 0;
        } else {
            printf("Found new backup image: %s\n", tmp);
        }
    }
    // If the fs_type of this volume is "auto" or mount_point is /data
    // and is_data_media, let's revert
    // to using a rm -rf, rather than trying to do a
    // ext3/ext4/whatever format.
    // This is because some phones (like DroidX) will freak out if you
    // reformat the /system or /data partitions, and not boot due to
    // a locked bootloader.
    // Other devices, like the Galaxy Nexus, XOOM, and Galaxy Tab 10.1
    // have a /sdcard symlinked to /data/media.
    // Or of volume does not exist (.android_secure), just rm -rf.
    if (vol == NULL || 0 == strcmp(vol->fs_type, "auto"))
        backup_filesystem = NULL;
    if (0 == strcmp(vol->mount_point, "/data") && is_data_media())
        backup_filesystem = NULL;

    ensure_directory(mount_point);

    char path[PATH_MAX];
    build_configuration_path(path, NANDROID_HIDE_PROGRESS_FILE);
    ensure_path_mounted(path);
    int callback = stat(path, &file_info) != 0;

    ui_print("正在还原 %s...\n", name);
    if (backup_filesystem == NULL) {
        if (0 != (ret = format_volume(mount_point))) {
            ui_print("格式化%s时出错!\n", mount_point);
            return ret;
        }
    } else if (0 != (ret = format_device(device, mount_point, backup_filesystem))) {
        ui_print("格式化%s时出错!\n", mount_point);
        return ret;
    }

    if (0 != (ret = ensure_path_mounted(mount_point))) {
        ui_print("不能挂载%s!\n", mount_point);
        return ret;
    }

    if (restore_handler == NULL)
        restore_handler = get_restore_handler(mount_point);

    // override restore handler for undump
    if (strcmp(backup_path, "-") == 0) {
        restore_handler = tar_undump_wrapper;
    }

    if (restore_handler == NULL) {
        ui_print("Error finding an appropriate restore handler.\n");
        return -2;
    }

    if (0 != (ret = restore_handler(tmp, mount_point, callback))) {
        ui_print("还原%s时出错!\n", mount_point);
        return ret;
    }
#ifdef NEED_SELINUX_FIX
    if (strcmp(backup_path, "-") == 0) {
        LOGE("不能在undump模式恢复selinux context.\n");
    }
    else if (0 == strcmp(mount_point, "/data") ||
                0 == strcmp(mount_point, "/system") ||
                0 == strcmp(mount_point, "/cache"))
    {
        name = basename(mount_point);
        sprintf(tmp, "%s/%s.context", backup_path, name);
        ui_print("恢复selinux context...\n");
        if ((ret = restorecon_from_file(tmp)) < 0) {
            ui_print("从%s.context恢复出错,使用常规restorecon.\n", name);
            if ((ret = restorecon_recursive(mount_point, "/data/media/")) < 0) {
                LOGE("Restorecon %s 出错!\n", mount_point);
                return ret;
            }
            ui_print("selinux context恢复完毕.\n");
        }
    }
#endif

    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }

    return 0;
}

int nandroid_restore_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

    // see if we need a raw restore (mtd)
    char tmp[PATH_MAX];
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        int ret;
        const char* name = basename(root);
        ui_print("还原备份之前清除%s数据...\n", name);
        if (0 != (ret = format_volume(root))) {
            ui_print("清除%s数据时出错!", name);
            return ret;
        }

        if (strcmp(backup_path, "-") == 0)
            strcpy(tmp, backup_path);
        else
            sprintf(tmp, "%s%s.img", backup_path, root);

        ui_print("还原%s镜像...\n", name);
        if (0 != (ret = restore_raw_partition(vol->fs_type, vol->blk_device, tmp))) {
            ui_print("刷入%s镜像时出错!\n", name);
            return ret;
        }
        return 0;
    }
    return nandroid_restore_partition_extended(backup_path, root, 1);
}

int nandroid_restore(const char* backup_path, int restore_boot, int restore_system, int restore_data, int restore_cache, int restore_sdext, int restore_wimax) {
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    nandroid_files_total = 0;

    if (ensure_path_mounted(backup_path) != 0)
        return print_and_error("Can't mount backup path\n");

    struct stat s;
    char tmp[PATH_MAX];

    ensure_path_mounted("/sdcard");
    if (0 == stat("/sdcard/clockworkmod/.no_md5sum", &s))
        ui_print("跳过文件MD5检验...\n");
    else {
        ui_print("检验文件md5值...\n");
        sprintf(tmp, "cd %s && md5sum -c nandroid.md5", backup_path);
        if (0 != __system(tmp))
            return print_and_error("MD5校验失败!\n");
    }

    int ret;

    if (restore_boot && NULL != volume_for_path("/boot") && 0 != (ret = nandroid_restore_partition(backup_path, "/boot")))
        return ret;

    //struct stat s;
    Volume *vol = volume_for_path("/wimax");
    if (restore_wimax && vol != NULL && 0 == stat(vol->blk_device, &s)) {
        char serialno[PROPERTY_VALUE_MAX];

        serialno[0] = 0;
        property_get("ro.serialno", serialno, "");
        sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);

        struct stat st;
        if (0 != stat(tmp, &st)) {
            ui_print("WARNING: WiMAX partition exists, but nandroid\n");
            ui_print("         backup does not contain WiMAX image.\n");
            ui_print("         You should create a new backup to\n");
            ui_print("         protect your WiMAX keys.\n");
        } else {
            ui_print("Erasing WiMAX before restore...\n");
            if (0 != (ret = format_volume("/wimax")))
                return print_and_error("Error while formatting wimax!\n");
            ui_print("Restoring WiMAX image...\n");
            if (0 != (ret = restore_raw_partition(vol->fs_type, vol->blk_device, tmp)))
                return ret;
        }
    }

    if (restore_system && 0 != (ret = nandroid_restore_partition(backup_path, "/system")))
        return ret;

    if (volume_for_path("/preload") != NULL)
    {
        if (restore_system && 0 != (ret = nandroid_restore_partition(backup_path, "/preload")))
            return ret;
    }

    if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/data")))
        return ret;

    if (has_datadata()) {
        if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/datadata")))
            return ret;
    }

    if (restore_data && 0 != (ret = nandroid_restore_partition_extended(backup_path, get_android_secure_path(), 0)))
        return ret;

    if (restore_cache && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/cache", 0)))
        return ret;

    if (restore_sdext && 0 != (ret = nandroid_restore_partition(backup_path, "/sd-ext")))
        return ret;

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\n还原完毕!\n");
    return 0;
}

int nandroid_undump(const char* partition) {
    nandroid_files_total = 0;

    int ret;

    if (strcmp(partition, "boot") == 0) {
        Volume *vol = volume_for_path("/boot");
        // make sure the volume exists before attempting anything...
        if (vol == NULL || vol->fs_type == NULL)
            return 1;
        char cmd[PATH_MAX];
        sprintf(cmd, "cat /proc/self/fd/0 > %s", vol->blk_device);
        return __system(cmd);
        // return __system("flash_image boot /proc/self/fd/0");
    }

    if (strcmp(partition, "recovery") == 0) {
        if (0 != (ret = nandroid_restore_partition("-", "/recovery")))
            return ret;
    }

    if (strcmp(partition, "system") == 0) {
        if (0 != (ret = nandroid_restore_partition("-", "/system")))
            return ret;
    }

    if (strcmp(partition, "data") == 0) {
        if (0 != (ret = nandroid_restore_partition("-", "/data")))
            return ret;
    }

    sync();
    return 0;
}

int nandroid_usage() {
    printf("Usage: nandroid backup\n");
    printf("Usage: nandroid restore <directory>\n");
    printf("Usage: nandroid dump <partition>\n");
    printf("Usage: nandroid undump <partition>\n");
    return 1;
}

static int bu_usage() {
    printf("Usage: bu <fd> backup partition\n");
    printf("Usage: Prior to restore:\n");
    printf("Usage: echo -n <partition> > /tmp/ro.bu.restore\n");
    printf("Usage: bu <fd> restore\n");
    return 1;
}

int bu_main(int argc, char** argv) {
    load_volume_table();

    if (strcmp(argv[2], "backup") == 0) {
        if (argc != 4) {
            return bu_usage();
        }

        int fd = atoi(argv[1]);
        char* partition = argv[3];

        if (fd != STDOUT_FILENO) {
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        // fprintf(stderr, "%d %d %s\n", fd, STDOUT_FILENO, argv[3]);
        int ret = nandroid_dump(partition);
        sleep(10);
        return ret;
    } else if (strcmp(argv[2], "restore") == 0) {
        if (argc != 3) {
            return bu_usage();
        }

        int fd = atoi(argv[1]);
        if (fd != STDIN_FILENO) {
            dup2(fd, STDIN_FILENO);
            close(fd);
        }

        char partition[100];
        FILE* f = fopen("/tmp/ro.bu.restore", "r");
        if (f == NULL) {
            printf("cannot open ro.bu.restore\n");
            return bu_usage();
        }

        if (fgets(partition, sizeof(partition), f) == NULL) {
            fclose(f);
            printf("nothing to restore!\n");
            return bu_usage();
        }

        size_t len = strlen(partition);
        if (partition[len - 1] == '\n')
            partition[len - 1] = '\0';

        // fprintf(stderr, "%d %d %s\n", fd, STDIN_FILENO, argv[3]);
        return nandroid_undump(partition);
    }

    return bu_usage();
}

int nandroid_main(int argc, char** argv) {
    load_volume_table();
    char backup_path[PATH_MAX];

    if (argc > 3 || argc < 2)
        return nandroid_usage();

    if (strcmp("backup", argv[1]) == 0) {
        if (argc != 2)
            return nandroid_usage();

        nandroid_generate_timestamp_path(backup_path);
        return nandroid_backup(backup_path);
    }

    if (strcmp("restore", argv[1]) == 0) {
        if (argc != 3)
            return nandroid_usage();
        return nandroid_restore(argv[2], 1, 1, 1, 1, 1, 0);
    }

    if (strcmp("dump", argv[1]) == 0) {
        if (argc != 3)
            return nandroid_usage();
        return nandroid_dump(argv[2]);
    }

    if (strcmp("undump", argv[1]) == 0) {
        if (argc != 3)
            return nandroid_usage();
        return nandroid_undump(argv[2]);
    }

    return nandroid_usage();
}

#ifdef NEED_SELINUX_FIX
static int nochange;
static int verbose;
int bakupcon_to_file(const char *pathname, const char *filename)
{
    int ret = 0;
    struct stat sb;
    char* filecontext = NULL;
    FILE * f = NULL;
    if (lstat(pathname, &sb) < 0) {
        LOGW("bakupcon_to_file: %s not found\n", pathname);
        return -1;
    }

    if (lgetfilecon(pathname, &filecontext) < 0) {
        LOGW("bakupcon_to_file: can't get %s context\n", pathname);
        ret = 1;
    }
    else {
        if ((f = fopen(filename, "a+")) == NULL) {
            LOGE("(bakupcon_to_file)无法创建文件%s\n", filename);
            return -1;
        }
        //fprintf(f, "chcon -h %s '%s'\n", filecontext, pathname);
        fprintf(f, "%s\t%s\n", pathname, filecontext);
        fclose(f);
    }

    //skip read symlink directory
    if (S_ISLNK(sb.st_mode)) return 0;

    DIR *dir = opendir(pathname);
    // not a directory, carry on
    if (dir == NULL) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char *entryname;
        if (!strcmp(entry->d_name, ".."))
            continue;
        if (!strcmp(entry->d_name, "."))
            continue;
        if (asprintf(&entryname, "%s/%s", pathname, entry->d_name) == -1)
            continue;
        if ((is_data_media() && (strncmp(entryname, "/data/media/", 12) == 0)) ||
                strncmp(entryname, "/data/data/com.google.android.music/files/", 42) == 0 )
            continue;

        bakupcon_to_file(entryname, filename);
        free(entryname);
    }

    closedir(dir);
    return ret;
}

int restorecon_from_file(const char *filename)
{
    int ret = 0;
    FILE * f = NULL;
    if ((f = fopen(filename, "r")) == NULL)
    {
        LOGW("restorecon_from_file: can't open %s\n", filename);
        return -1;
    }

    char linebuf[4096];
    while(fgets(linebuf, 4096, f)) {
        if (linebuf[strlen(linebuf)-1] == '\n')
            linebuf[strlen(linebuf)-1] = '\0';

        char *p1, *p2;
        char *buf = linebuf;

        p1 = strtok(buf, "\t");
        p2 = strtok(NULL, "\t");
        LOGI("%s %s\n", p1, p2);
        if (lsetfilecon(p1, p2) < 0) {
            LOGW("restorecon_from_file: can't setfilecon %s\n", p1);
            ret = 1;
        }
    }
    fclose(f);
    return ret;
}

int restorecon_recursive(const char *pathname, const char *exclude)
{
    int ret = 0;
    struct stat sb;
    if (lstat(pathname, &sb) < 0) {
        LOGW("restorecon: %s not found\n", pathname);
        return -1;
    }
    if (exclude) {
        int eclen = strlen(exclude);
        if (strncmp(pathname, exclude, strlen(exclude)) == 0)
            return 0;
    }
    //if (selinux_android_restorecon(pathname, 0) < 0) {
    if (restorecon(pathname, &sb) < 0) {
        LOGW("restorecon: error restoring %s context\n", pathname);
        ret = 1;
    }

    //skip symlink dir
    if (S_ISLNK(sb.st_mode)) return 0;

    DIR *dir = opendir(pathname);
    // not a directory, carry on
    if (dir == NULL) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char *entryname;
        if (!strcmp(entry->d_name, ".."))
            continue;
        if (!strcmp(entry->d_name, "."))
            continue;
        if (asprintf(&entryname, "%s/%s", pathname, entry->d_name) == -1)
            continue;

        restorecon_recursive(entryname, exclude);
        free(entryname);
    }

    closedir(dir);
    return ret;
}

extern struct selabel_handle *sehandle;
int restorecon(const char *pathname, const struct stat *sb)
{
    char *oldcontext, *newcontext;

    if (lgetfilecon(pathname, &oldcontext) < 0) {
        fprintf(stderr, "Could not get context of %s:  %s\n",
                pathname, strerror(errno));
        return -1;
    }
    if (selabel_lookup(sehandle, &newcontext, pathname, sb->st_mode) < 0) {
        fprintf(stderr, "Could not lookup context for %s:  %s\n", pathname,
               strerror(errno));
        return -1;
    }
    if (strcmp(newcontext, "<<none>>") &&
        strcmp(oldcontext, newcontext)) {
        if (verbose)
            fprintf(stdout, "Relabeling %s from %s to %s.\n", pathname,
                    oldcontext, newcontext);
        if (!nochange) {
            if (lsetfilecon(pathname, newcontext) < 0) {
                fprintf(stderr, "Could not label %s with %s:  %s\n",
                        pathname, newcontext, strerror(errno));
                return -1;
            }
        }
    }
    freecon(oldcontext);
    freecon(newcontext);
    return 0;
}
/*
int restorecon_main(int argc, char **argv)
{
    int ch, recurse = 0;
    int i = 0;

    char *exclude = NULL , *progname = argv[0];

    do {
        ch = getopt(argc, argv, "nrRe:v");
        if (ch == EOF)
            break;
        switch (ch) {
        case 'n':
            nochange = 1;
            break;
        case 'r':
        case 'R':
            recurse = 1;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'e':
            exclude = optarg;
            break;
        default:
            printf("usage:  %s [-nrRev] pathname...\n", progname);
            return 1;
        }
    } while (1);

    argc -= optind;
    argv += optind;
    if (!argc) {
        printf("usage:  %s [-nrRev] pathname...\n", progname);
        return 1;
    }
    //sehandle = selinux_android_file_context_handle();
    //if (!sehandle) {
    //    printf("Could not load file_contexts:  %s\n",
    //            strerror(errno));
    //    return -1;
    //}
    int rc;
    struct stat sb;
    if (recurse) {
        for (i = 0; i < argc; i++) {
            rc = lstat(argv[i], &sb);
            if (rc < 0) {
                printf("Could not stat %s:  %s\n", argv[i],
                        strerror(errno));
                continue;
            }
            restorecon_recursive(argv[i], exclude);
        }
    } else {
        for (i = 0; i < argc; i++) {
            rc = lstat(argv[i], &sb);
            if (rc < 0) {
                printf("Could not stat %s:  %s\n", argv[i],
                        strerror(errno));
                continue;
            }
            restorecon(argv[i], &sb);
        }
    }

    return 0;
}*/
#endif