# SELinux and LXC

An LXC mount namespace can hide a selinuxfs mount from the host filesystem
view, but it cannot provide a separate SELinux policy.

The upstream kernel registers one global `struct selinux_state`. Every
selinuxfs instance reads and updates that state. In particular, writing an
Android policy to a container-visible `selinuxfs/load` file changes the policy
used by all host and container processes.

Android 17 second-stage init assumes first-stage init has already loaded the
Android policy. It requires working process and file contexts to:

- authorize property service clients;
- calculate service domain transitions;
- call `setexeccon()` before starting services;
- label sockets and runtime files;
- start bootstrap APEX services.

Starting second-stage init without a loaded policy therefore fails even in
permissive mode. `androidboot.selinux=permissive` changes enforcement; it does
not remove the context and transition requirements.

## Supported direction

For true policy isolation, use a VM/microVM so Android owns its kernel SELinux
state.

For continued LXC development, the alternative is a purpose-built,
container-only Android userspace port. That port must rebuild init and
libselinux behavior consistently across property handling and service startup.
It should be treated as an explicit compatibility mode, not as SELinux.
