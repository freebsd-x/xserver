# ── core/xserver — standalone compositor build ────────────────────────────────
# This file supports standalone development and CI builds of xserver alone.
# For the full FreeBSD-X platform build use the root meson.build instead.
#
# Usage:
#   cd core/xserver
#   meson setup build && meson compile -C build
#
# The compositor source groups, shaders, and Wayland-protocol generation are now
# shared with the root build via build_common/meson.build (subdir'd below) — a
# single source of truth, so adding a compositor source / shader / protocol is a
# one-file edit there. Only the external dependency lists and the executable()/
# test() definitions below are still per-file (they differ by target and change
# rarely); keep those roughly aligned with build/meson.build.

project('xserver', ['c', 'cpp'],
  version         : '0.1.0',
  default_options : ['warning_level=3', 'cpp_std=c++23', 'c_std=c11'],
)

libdrm_dep    = dependency('libdrm',   required : true)
gbm_dep       = dependency('gbm',      required : true)
egl_dep       = dependency('egl',      required : true)
glesv2_dep    = dependency('glesv2',   required : true)
libseat_dep   = dependency('libseat',  required : true)
libinput_dep  = dependency('libinput', required : true)
libudev_dep   = dependency('libudev',  required : true)
vulkan_dep    = dependency('vulkan',   required : true)
wayland_server_dep    = dependency('wayland-server',    required : true)
wayland_protocols_dep = dependency('wayland-protocols', version  : '>=1.15', required : true)
freetype_dep  = dependency('freetype2',  required : true)
harfbuzz_dep  = dependency('harfbuzz',   required : true)
fontconfig_dep= dependency('fontconfig', required : true)
libpng_dep    = dependency('libpng16',   required : true)
thread_dep    = dependency('threads')
xkbcommon_dep = dependency('xkbcommon', required : true)

cpp = meson.get_compiler('cpp')
libutil_dep    = cpp.find_library('util',    required : true)
libpam_dep     = cpp.find_library('pam',     required : true)
libcrypto_dep  = cpp.find_library('crypto',  required : true)
libarchive_dep = cpp.find_library('archive', required : true)
libexecinfo_dep = cpp.find_library('execinfo', required : true)  # backtrace() for crash_handler

glslc    = find_program('glslc', required : true)
glsl_gen = generator(glslc,
  output    : '@PLAINNAME@.spv',
  arguments : ['@INPUT@', '-o', '@OUTPUT@'],
)

# Compositor sources, shaders, and Wayland-protocol generation come from the
# shared fragment (single source of truth) — see build_common/meson.build. It
# exports compositor_core_srcs, xbus_service_srcs, compositor_shader_files, and
# wayland_protocol_srcs. Requires wayland_protocols_dep (above).
subdir('build_common')

compiled_shaders = glsl_gen.process(compositor_shader_files)

inc_dir = include_directories(
  'include',
  '../../include',
  '../../sdk/xrender/include',
  '../../sdk/xkit/include',
  '../../core/wayland-bridge/include',
)

executable('xserver',
  compositor_core_srcs + xbus_service_srcs + ['src/main.cc'],
  wayland_protocol_srcs,
  compiled_shaders,
  include_directories : inc_dir,
  dependencies : [
    libdrm_dep, gbm_dep, egl_dep, glesv2_dep,
    libseat_dep, libinput_dep, libudev_dep,
    vulkan_dep, wayland_server_dep,
    freetype_dep, harfbuzz_dep, fontconfig_dep, libpng_dep,
    thread_dep, xkbcommon_dep,
    libpam_dep, libcrypto_dep, libarchive_dep, libutil_dep, libexecinfo_dep,
  ],
  install : true,
)

# Tiny setuid-root PAM helper: lets the unprivileged compositor verify a
# password without running the whole display server as root. install_mode makes
# `sudo meson install` set it setuid root (owner root, mode 4755) automatically —
# exactly how the base system ships su/login — so no manual chmod is needed.
executable('xauthcheck',
  ['src/xauthcheck.cc'],
  dependencies : [libpam_dep],
  install      : true,
  install_mode : ['rwsr-xr-x', 'root', 'root'],
)

# xsessiond: unified seat + session daemon (phase 1 = authentication). Runs as
# root so the unprivileged compositor can verify a password over a socket
# instead of a per-check setuid exec. Standalone: libpam only. See
# docs/xsessiond-design.md.
xsessiond_exe = executable('xsessiond',
  ['src/xsessiond.cc'],
  include_directories : inc_dir,   # needs xserver/xsessiond_protocol.h
  dependencies : [libpam_dep, libseat_dep],   # libseat: opt-in phase-2b client mode
  install      : true,
)

# Smoke test: spawn the real xsessiond on a scratch socket and drive the real
# client (protocol, single-process concurrency, auth/session gating, device
# SCM_RIGHTS brokering). Plain exit-code test (no gtest). Positive PAM is the
# live-seat tier (scripts/xsessiond-verify.sh), not this. Daemon path = argv[1].
test('xsessiond smoke',
  executable('xsessiond_smoke_test',
    'tests/smoke/xsessiond_daemon_test.cc',
    'src/xsessiond_client.cc',
    'src/xsessiond_device.cc',
    include_directories : inc_dir,
    dependencies        : [libseat_dep],
  ),
  args    : [xsessiond_exe],
  suite   : 'smoke',
  timeout : 30,
)

# ── Unit tests ────────────────────────────────────────────────────────────────
gtest_dep = dependency('gtest', main : true, required : false)
if gtest_dep.found()

  # IPC socket — SCM_RIGHTS zero-copy FD passing
  test('xserver ipc smoke',
    executable('xserver_ipc_test',
      'tests/test_ipc.cc',
      'src/ipc_socket.cc',
      'src/xbus_message_pool.cc',   # ipc_socket.cc uses XBusMessagePool
      include_directories : inc_dir,
      dependencies        : [gtest_dep, thread_dep],
    ),
    suite : 'unit',
  )

  # WindowStack — pure logic: hit testing, geometry, z-order, workspace switching
  test('xserver window_stack',
    executable('xserver_window_stack_test',
      'tests/unit/window_stack_test.cc',
      include_directories : inc_dir,
      dependencies        : [gtest_dep, vulkan_dep],
    ),
    suite : 'unit',
  )

  # Protocol structs — sizes, field layout, RenderMessage geometry round-trip
  test('xserver protocol',
    executable('xserver_protocol_test',
      'tests/unit/protocol_test.cc',
      include_directories : inc_dir,
      dependencies        : [gtest_dep],
    ),
    suite : 'unit',
  )

  # XBusMessagePool — lock-free slab allocator: acquire/release, exhaustion, RAII, concurrency
  test('xserver message_pool',
    executable('xserver_message_pool_test',
      'tests/unit/message_pool_test.cc',
      'src/xbus_message_pool.cc',
      include_directories : inc_dir,
      dependencies        : [gtest_dep, thread_dep],
    ),
    suite : 'unit',
  )

endif
