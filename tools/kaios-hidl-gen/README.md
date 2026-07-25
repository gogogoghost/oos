# KaiOS hidl-gen host build

This builds `kaiostech/hidl-gen` as a Debian host executable without an
Android source checkout or Soong.

## Debian dependencies

```sh
sudo apt install cmake g++ bison flex libssl-dev \
  android-libbase-dev android-libutils-dev android-liblog-dev
```

## Build

From the repository root:

```sh
cmake -S cmake/kaios-hidl-gen -B build-kaios-hidl-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-kaios-hidl-host -j2
```

The executable is `build-kaios-hidl-host/hidl-gen`.

The CMake files leave `sources/kaios-hidl-gen` unchanged. They use a
build-directory grammar copy to bridge the 2019 KaiOS grammar and Debian 13's
Bison/Flex APIs.

## Generate Composer headers

The interface sources already checked out by this project are enough for a
Composer 2.3 header-generation pass:

```sh
mkdir -p generated/composer-2.3
build-kaios-hidl-host/hidl-gen \
  -o generated/composer-2.3 -L c++-headers \
  -r android.hardware:sources/aosp-hardware-interfaces \
  -r android.hidl:sources/aosp-system-libhidl/transport \
  android.hardware.graphics.composer@2.3
```

For a C++ program which includes the generated Composer 2.3 headers, generate
the inherited Composer and graphics-common package versions too.
