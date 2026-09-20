# Build Android Image

For Anbox Reboxed we're using a minimal customized version of Android but otherwise
base all our work of a recent release of the [Android Open Source Project](https://source.android.com/).

To rebuild the Android image you need first fetch all relevant sources. This
will take quite a huge amount of your disk space (~40GB). AOSP recommends at
least 100GB of free disk space. Have a look at [their](https://source.android.com/source/requirements.html) pages too.

In general for building the Anbox Reboxed Andorid image the instructions found on [the pages
from the AOSP project](https://source.android.com/source/requirements.html) apply.
We will not describe again here of how to build the Android system in general but
only focus on the steps required for Anbox Reboxed.

## Fetch all relevant sources

First setup a new workspace where you will download all sources too.

```
$ mkdir $HOME/anbox-work
```

Now initialize the repository by download the manifest and start fetching
the sources:

```
$ cd $HOME/anbox-work
$ repo init -u https://github.com/anbox/platform_manifests.git -b anbox
$ repo sync -j4
```

This will take quite some time depending on the speed of your internet connection.

## Build Android

When all sources are successfully downloaded you can start building Android itself.

The public devalpha product builds x86/x86_64 applications without ARM
translation. Berberis and its source-preparation tooling are not published.
Do not enable ANBOX_ENABLE_LOCAL_BERBERIS in a public checkout.

This page also contains inherited legacy AOSP instructions; use
[public-build.md](public-build.md) for the current host prerequisites.
An Android 15 build requires a separately obtained matching AOSP tree and
platform signing setup.

Firstly initialize the environment with the ```envsetup.sh``` script.

```
$ . build/envsetup.sh
```

Then initialize the build using ```lunch```.

```
$ lunch anbox_x86_64-userdebug
```

The complete list of supported build targets:

 * anbox_x86_64-userdebug
 * anbox_armv7a_neon-userdebug
 * anbox_arm64-userdebug

Now build everything with

```
$ make -j8
```

The Anbox Reboxed product build includes `ReboxedBridge` in
`/system/priv-app/ReboxedBridge/ReboxedBridge.apk`. Soong signs it through
`certificate: "platform"`, using the same product platform certificate as the
rest of the image. The product also installs the Bridge privileged-permission
and `android.uid.system` allowlists under `/system/etc/permissions`.

Once the build is done we need to take the results and create an image file
suitable for Anbox Reboxed.

```
$ cd $HOME/anbox-work/vendor/anbox
$ scripts/create-package.sh \
    $PWD/../../out/target/product/x86_64/ramdisk.img \
    $PWD/../../out/target/product/x86_64/system.img
```

`create-package.sh` verifies the preload layout and compares the APK signing
certificate with `framework-res.apk` before it packs the image. A missing app,
wrong mode or owner, missing allowlist, or certificate mismatch stops the
build.

(replace `x86_64` with your target architecture)

This will create an *android.img* file in the current directory.

With this, you are now able to use your custom image within the Anbox Reboxed runtime.

## Run Anbox Reboxed with self build android.img

If you have Anbox Reboxed installed on your system you need to stop it first. If you used
the installer script and the snap you can do this via

```
$ initctl stop anbox
$ sudo systemctl stop snap.anbox.container-manager
```

It is important that you stop both, the container manager and the session manager.

Once both services are stopped you can start the container manager with your
custom android.img file by running

```
$ datadir=$HOME/anbox-data
$ mkdir -p $datadir/rootfs
$ sudo anbox-reboxed container-manager \
    --android-image=/path/to/android.img \
    --data-path=$datadir
```

This will start the container manager and setup the container rootfs inside the
specified data path.

```
$ ls -alh $HOME/anbox-data
total 20K
drwxrwxr-x  5 ubuntu  ubuntu  4,0K Feb 22 08:04 .
drwxrwxr-x 16 ubuntu  ubuntu  4,0K Feb 22 08:04 ..
drwxr-xr-x  2 100000  100000 4,0K Feb 22 08:04 cache
drwxr-xr-x  2 100000  100000 4,0K Feb 22 08:04 data
drwxr-xr-x  2 root    root   4,0K Feb 22 08:04 rootfs
```

**NOTE:** If you look into the $HOME/anbox-data/rootfs directory you won't see
anything as the container manager spawns up a private mount namespace which
prevents anything from the outside to see its mount points.

The *cache* and *data* directories are bind-mounted into the rootfs at *rootfs/data*
and *rootfs/cache*.
