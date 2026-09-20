/*
 * Android's vold_prepare_subdirs treats an absent security.selinux xattr as
 * fatal.  Anbox runs with SELinux disabled, so no such xattrs can exist even
 * though the directory layout, ownership, and modes are still required.
 *
 * This helper mirrors Android 15's prepare operation without label handling.
 * The destroy operation is delegated to the unmodified Android binary.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum {
  AID_ROOT = 0,
  AID_SYSTEM = 1000,
  AID_CACHE = 2001,
  STORAGE_FLAG_DE = 1,
  STORAGE_FLAG_CE = 2,
};

static void log_error(const char *format, ...) {
  va_list args;
  va_start(args, format);
  fputs("vold_prepare_subdirs_anbox: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
}

static bool prepare_dir(mode_t mode, uid_t uid, gid_t gid,
                        const char *format, ...) {
  char path[PATH_MAX];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(path, sizeof(path), format, args);
  va_end(args);
  if (length < 0 || (size_t)length >= sizeof(path)) {
    log_error("directory path is too long");
    return false;
  }

  if (mkdir(path, mode) != 0 && errno != EEXIST) {
    log_error("mkdir(%s) failed: %s", path, strerror(errno));
    return false;
  }

  struct stat st;
  if (lstat(path, &st) != 0) {
    log_error("lstat(%s) failed: %s", path, strerror(errno));
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    log_error("%s exists but is not a directory", path);
    return false;
  }
  if (chown(path, uid, gid) != 0) {
    log_error("chown(%s, %u, %u) failed: %s", path, uid, gid,
              strerror(errno));
    return false;
  }
  if (chmod(path, mode) != 0) {
    log_error("chmod(%s, %04o) failed: %s", path, mode, strerror(errno));
    return false;
  }

  fprintf(stderr,
          "vold_prepare_subdirs_anbox: prepared %s mode=%04o uid=%u gid=%u "
          "(SELinux disabled; label operation omitted)\n",
          path, mode, uid, gid);
  return true;
}

static bool prepare_apex_subdirs(const char *misc_path) {
  if (!prepare_dir(0711, AID_ROOT, AID_ROOT, "%s/apexdata", misc_path))
    return false;

  DIR *directory = opendir("/data/misc/apexdata");
  if (!directory) {
    log_error("opendir(/data/misc/apexdata) failed: %s", strerror(errno));
    return false;
  }

  bool result = true;
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (entry->d_name[0] == '.')
      continue;
    if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN)
      continue;
    if (!prepare_dir(0771, AID_ROOT, AID_SYSTEM, "%s/apexdata/%s",
                     misc_path, entry->d_name)) {
      result = false;
      break;
    }
  }
  closedir(directory);
  return result;
}

static bool prepare_common_misc(const char *misc_path) {
  if (!prepare_dir(0700, AID_ROOT, AID_ROOT, "%s/vold", misc_path) ||
      !prepare_dir(0700, AID_ROOT, AID_ROOT, "%s/storaged", misc_path) ||
      !prepare_dir(0700, AID_ROOT, AID_ROOT, "%s/rollback", misc_path))
    return false;

  /* Upstream deliberately ignores failures for apexrollback. */
  prepare_dir(0700, AID_ROOT, AID_ROOT, "%s/apexrollback", misc_path);
  /* Upstream currently also ignores the return value of this call. */
  prepare_apex_subdirs(misc_path);
  return true;
}

static bool prepare_subdirs(const char *uuid, unsigned int user, int flags) {
  char data[PATH_MAX];
  if (uuid[0])
    snprintf(data, sizeof(data), "/mnt/expand/%s", uuid);
  else
    strcpy(data, "/data");

  if (flags & STORAGE_FLAG_DE) {
    if (!prepare_dir(0771, AID_SYSTEM, AID_SYSTEM, "%s/user_de/%u", data,
                     user) ||
        !prepare_dir(0771, AID_SYSTEM, AID_SYSTEM,
                     "%s/misc_de/%u/sdksandbox", data, user))
      return false;

    if (!uuid[0]) {
      char misc_de[PATH_MAX];
      snprintf(misc_de, sizeof(misc_de), "/data/misc_de/%u", user);
      if (!prepare_common_misc(misc_de) ||
          !prepare_dir(0771, AID_SYSTEM, AID_SYSTEM,
                       "/data/misc/profiles/cur/%u", user) ||
          !prepare_dir(0700, AID_SYSTEM, AID_SYSTEM,
                       "/data/vendor_de/%u/fpdata", user) ||
          !prepare_dir(0700, AID_SYSTEM, AID_SYSTEM,
                       "/data/vendor_de/%u/facedata", user))
        return false;
    }
  }

  if (flags & STORAGE_FLAG_CE) {
    if (!prepare_dir(0771, AID_SYSTEM, AID_SYSTEM, "%s/user/%u", data,
                     user) ||
        !prepare_dir(0771, AID_SYSTEM, AID_SYSTEM,
                     "%s/misc_ce/%u/sdksandbox", data, user))
      return false;

    if (!uuid[0]) {
      char misc_ce[PATH_MAX];
      snprintf(misc_ce, sizeof(misc_ce), "/data/misc_ce/%u", user);
      if (!prepare_common_misc(misc_ce) ||
          !prepare_dir(0770, AID_SYSTEM, AID_CACHE, "%s/checkin", misc_ce) ||
          !prepare_dir(0700, AID_SYSTEM, AID_SYSTEM,
                       "/data/system_ce/%u/backup", user) ||
          !prepare_dir(0700, AID_SYSTEM, AID_SYSTEM,
                       "/data/system_ce/%u/backup_stage", user) ||
          !prepare_dir(0700, AID_SYSTEM, AID_SYSTEM,
                       "/data/vendor_ce/%u/facedata", user))
        return false;
    }
  }
  return true;
}

static bool valid_number(const char *value) {
  if (!value[0] || strlen(value) >= 7)
    return false;
  for (const char *p = value; *p; ++p)
    if (*p < '0' || *p > '9')
      return false;
  return true;
}

static bool valid_uuid(const char *value) {
  if (strlen(value) >= 40)
    return false;
  for (const char *p = value; *p; ++p) {
    const bool valid = (*p >= '0' && *p <= '9') ||
                       (*p >= 'a' && *p <= 'f') ||
                       (*p >= 'A' && *p <= 'F') || *p == '-' || *p == '_';
    if (!valid)
      return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 5 || !valid_uuid(argv[2]) || !valid_number(argv[3]) ||
      !valid_number(argv[4])) {
    log_error("usage: %s [ prepare | destroy ] <volume_uuid> <user_id> "
              "<flags>", argv[0]);
    return 255;
  }

  if (strcmp(argv[1], "destroy") == 0) {
    argv[0] = "/system/bin/vold_prepare_subdirs.anbox-real";
    execv(argv[0], argv);
    log_error("failed to execute original destroy helper: %s", strerror(errno));
    return 255;
  }
  if (strcmp(argv[1], "prepare") != 0) {
    log_error("unknown operation: %s", argv[1]);
    return 255;
  }

  const unsigned long user = strtoul(argv[3], NULL, 10);
  const int flags = (int)strtol(argv[4], NULL, 10);
  return prepare_subdirs(argv[2], (unsigned int)user, flags) ? 0 : 255;
}
