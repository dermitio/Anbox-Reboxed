/*
 * Android 15 installd assumes every existing app-data directory has a
 * security.selinux xattr. Anbox Reboxed deliberately runs its container with
 * SELinux disabled, where lgetfilecon() necessarily fails with ENODATA.
 *
 * This library is preloaded into installd only. Returning one stable synthetic
 * context makes installd's before/after comparison a no-op while leaving its
 * ownership, mode, project-ID, and directory-layout enforcement untouched.
 */

#include <stddef.h>

extern void *malloc(size_t size);

int lgetfilecon(const char *path, char **context) {
  (void)path;
  static const char synthetic_context[] =
      "u:object_r:anbox_unlabeled_app_data:s0";
  char *copy = (char *)malloc(sizeof(synthetic_context));
  if (!copy)
    return -1;

  for (size_t i = 0; i < sizeof(synthetic_context); ++i)
    copy[i] = synthetic_context[i];
  *context = copy;
  return (int)(sizeof(synthetic_context) - 1);
}
