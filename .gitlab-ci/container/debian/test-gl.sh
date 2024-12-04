#!/usr/bin/env bash
# shellcheck disable=SC2086 # we want word splitting

set -e

. .gitlab-ci/setup-test-env.sh

set -o xtrace

uncollapsed_section_start debian_setup "Base Debian system setup"

export DEBIAN_FRONTEND=noninteractive
export LLVM_VERSION="${LLVM_VERSION:=15}"

apt-get install -y libelogind0  # this interfere with systemd deps, install separately

# Ephemeral packages (installed for this script and removed again at the end)
EPHEMERAL=(
    bzip2
    ccache
    "clang-${LLVM_VERSION}"
    cmake
    dpkg-dev
    g++
    glslang-tools
    libasound2-dev
    libcap-dev
    "libclang-cpp${LLVM_VERSION}-dev"
    libdrm-dev
    libgles2-mesa-dev
    libgtest-dev
    libpciaccess-dev
    libpng-dev
    libudev-dev
    libvulkan-dev
    libwaffle-dev
    libwayland-dev
    libx11-xcb-dev
    libxcb-dri2-0-dev
    libxcb-dri3-dev
    libxcb-present-dev
    libxfixes-dev
    libxkbcommon-dev
    libxrandr-dev
    libxrender-dev
    "llvm-${LLVM_VERSION}-dev"
    make
    meson
    ocl-icd-opencl-dev
    patch
    pkgconf
    python3-distutils
    xz-utils
)

DEPS=(
    clinfo
    iptables
    kmod
    "libclang-common-${LLVM_VERSION}-dev"
    "libclang-cpp${LLVM_VERSION}"
    libcap2
    libegl1
    libepoxy0
    libfdt1
    libxcb-shm0
    ocl-icd-libopencl1
    python3-lxml
    python3-renderdoc
    python3-simplejson
    spirv-tools
    sysvinit-core
    weston
    xwayland
)

apt-get update

apt-get install -y --no-remove "${DEPS[@]}" "${EPHEMERAL[@]}" \
      $EXTRA_LOCAL_PACKAGES


. .gitlab-ci/container/container_pre_build.sh

############### Build piglit

uncollapsed_section_switch piglit "Building Piglit"

PIGLIT_OPTS="-DPIGLIT_USE_WAFFLE=ON
	     -DPIGLIT_USE_GBM=ON
	     -DPIGLIT_USE_WAYLAND=ON
	     -DPIGLIT_USE_X11=ON
	     -DPIGLIT_BUILD_GLX_TESTS=ON
	     -DPIGLIT_BUILD_EGL_TESTS=ON
	     -DPIGLIT_BUILD_WGL_TESTS=OFF
	     -DPIGLIT_BUILD_GL_TESTS=ON
	     -DPIGLIT_BUILD_GLES1_TESTS=ON
	     -DPIGLIT_BUILD_GLES2_TESTS=ON
	     -DPIGLIT_BUILD_GLES3_TESTS=ON
	     -DPIGLIT_BUILD_CL_TESTS=ON
	     -DPIGLIT_BUILD_VK_TESTS=ON
	     -DPIGLIT_BUILD_DMA_BUF_TESTS=ON" \
  . .gitlab-ci/container/build-piglit.sh

############### Build dEQP GL

uncollapsed_section_switch piglit_gl "Building dEQP for GL"

DEQP_API=GL \
DEQP_TARGET=surfaceless \
. .gitlab-ci/container/build-deqp.sh

uncollapsed_section_switch piglit_gles "Building dEQP for GLES"

DEQP_API=GLES \
DEQP_TARGET=surfaceless \
. .gitlab-ci/container/build-deqp.sh

############### Build apitrace

. .gitlab-ci/container/build-apitrace.sh

############### Build validation layer for zink

uncollapsed_section_switch vvl "Building Vulkan validation layers"

. .gitlab-ci/container/build-vulkan-validation.sh

############### Build nine tests

uncollapsed_section_switch nine "Building Nine tests"

. .gitlab-ci/container/build-ninetests.sh

############### Uninstall the build software

uncollapsed_section_switch debian_cleanup "Cleaning up base Debian system"

apt-get purge -y "${EPHEMERAL[@]}"

. .gitlab-ci/container/container_post_build.sh
