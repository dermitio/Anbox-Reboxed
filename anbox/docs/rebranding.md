# Anbox Reboxed rename compatibility

User-facing product, window, CLI, desktop, service-description, documentation,
diagnostic, and package-description text uses **Anbox Reboxed**. The canonical
host commands are `anbox-reboxed` and `anbox-reboxed-window`.

The following remaining `anbox` identifiers are intentional compatibility
names and must not be mechanically renamed:

- C++ namespaces, include paths/guards, CMake feature variables, and `ANBOX_*`
  environment variables used by existing tooling
- the `anbox` and `anbox-window` command aliases
- `/var/lib/anbox`, `/etc/anbox`, `/usr/share/anbox`, per-user `anbox`
  configuration/state directories, and existing image/runtime layouts
- `org.anbox` D-Bus names, object paths, Android packages, and interfaces
- qemu-pipe service names, sockets, Binder-facing protocols, Android properties,
  and guest/host ABI symbols
- the `anbox0` bridge, `/run/anbox`, LXC profile/container identifiers, AppArmor
  profile names, udev rules, and systemd/desktop filenames upgraded in place
- the snap/store package name `anbox`, retained so existing snap installations
  upgrade rather than installing a second package
- desktop `Categories=Anbox` and menu category keys, retained so dynamically
  generated Android launchers continue to appear in the existing menu
- inherited copyright notices, original-project citations, upstream URLs, and
  literal historical backup paths

The transactional installer detects the legacy executable, installs canonical
binaries, replaces the old commands and icon name with compatibility symlinks,
updates service/desktop registrations in place, and records a rename notice.
Its uninstall path never removes shared Android images, user data,
configuration state, or migration backups.
