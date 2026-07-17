project(
  'comic-chat-native',
  ['cpp', 'c'],
  version: '0.1.0',
  meson_version: '>=1.4.0',
  default_options: [
    # Meson's spelling maps to Clang's strict `-std=c++2c` flag.
    'cpp_std=c++26',
    'c_std=c11',
    'cpp_eh=default',
    'cpp_rtti=true',
    'warning_level=3',
    'werror=true',
    'b_ndebug=if-release',
    # Link-time optimization. ThinLTO keeps the Clang link parallel and fast
    # while recovering most of full LTO's cross-TU inlining and dead-code
    # elimination. Overridable at setup (e.g. -Db_lto=false for a fast debug
    # cycle); the deterministic goldens and sanitizer suites are unaffected
    # because LTO changes codegen, not program semantics.
    'b_lto=true',
    'b_lto_mode=thin',
  ],
)

cpp = meson.get_compiler('cpp')
if cpp.get_id() != 'clang'
  error('The Meson build requires Clang 21 or newer')
endif
if not cpp.version().version_compare('>=21.0.0')
  error('The Meson build requires Clang 21 or newer, found ' + cpp.version())
endif
add_project_arguments('-pedantic-errors', language: 'cpp')
add_project_arguments(
  '-DMBEDTLS_USER_CONFIG_FILE="comicchat/mbedtls_user_config.h"',
  language: ['c', 'cpp'],
)

# Aggressive-but-safe optimization for our own translation units (subprojects
# keep their pinned flags). Deliberately excludes anything that breaks IEEE
# semantics (no -ffast-math, load-bearing near the renderer and crypto) or
# reproducibility/portability (no -march=native; the deterministic goldens must
# stay host-independent). Section-splitting plus --gc-sections strips dead code;
# hidden visibility, no-semantic-interposition, and no-plt widen inlining and
# tighten calls in the final executable.
if get_option('optimization') in ['2', '3', 's']
  add_project_arguments(
    '-ffunction-sections',
    '-fdata-sections',
    '-fvisibility=hidden',
    '-fvisibility-inlines-hidden',
    '-fno-semantic-interposition',
    '-fno-plt',
    language: ['c', 'cpp'],
  )
  add_project_link_arguments('-Wl,--gc-sections', language: ['c', 'cpp'])
endif
if get_option('deterministic')
  add_project_arguments('-DCOMICCHAT_DETERMINISTIC_DEFAULT=1', language: 'cpp')
endif

inc = include_directories('include')
threads_dep = dependency('threads')
uv_dep = dependency(
  'libuv',
  fallback: ['libuv', 'libuv_dep'],
  include_type: 'system',
  default_options: ['werror=false', 'warning_level=0', 'build_tests=false', 'build_benchmarks=false'],
)
zlib_dep = dependency('zlib', fallback: ['zlib', 'zlib_dep'], include_type: 'system')
freetype_dep = dependency('freetype2', fallback: ['freetype2', 'freetype2_dep'], include_type: 'system')
harfbuzz_dep = dependency(
  'harfbuzz',
  fallback: 'harfbuzz',
  include_type: 'system',
  default_options: [
    'benchmark=disabled',
    'cairo=disabled',
    'chafa=disabled',
    'docs=disabled',
    'glib=disabled',
    'gobject=disabled',
    'gpu=disabled',
    'gpu_demo=disabled',
    'icu=disabled',
    'introspection=disabled',
    'png=disabled',
    'subset=disabled',
    'tests=disabled',
    'utilities=disabled',
    'warning_level=0',
    'werror=false',
  ],
)
icu_uc_dep = dependency(
  'icu-uc',
  fallback: 'icu',
  include_type: 'system',
  default_options: ['warning_level=0', 'werror=false'],
)
icu_i18n_dep = dependency(
  'icu-i18n',
  fallback: 'icu',
  include_type: 'system',
  default_options: ['warning_level=0', 'werror=false'],
)
cairo_dep = dependency('cairo', fallback: ['cairo', 'libcairo_dep'], include_type: 'system')
fontconfig_dep = dependency(
  'fontconfig',
  required: host_machine.system() != 'windows',
  include_type: 'system',
)
text_platform_deps = []
installed_comic_font = get_option('prefix') / get_option('datadir') / 'comic-chat' / 'comic.ttf'
source_raster_install_dir = get_option('datadir') / 'comic-chat' / 'source-res'
installed_source_raster = get_option('prefix') / source_raster_install_dir
core_cpp_args = ['-DCOMICCHAT_INSTALL_FONT_PATH="' + installed_comic_font + '"']
core_cpp_args += ['-DCOMICCHAT_INSTALL_SOURCE_RASTER_DIR="' + installed_source_raster + '"']
if fontconfig_dep.found()
  text_platform_deps += fontconfig_dep
  core_cpp_args += ['-DCOMICCHAT_HAS_FONTCONFIG=1']
endif
platform_deps = []
if host_machine.system() == 'windows'
  platform_deps += cpp.find_library('crypt32', required: true)
  platform_deps += cpp.find_library('ole32', required: true)
  platform_deps += cpp.find_library('shell32', required: true)
endif

# Keep the cryptographic implementation identical to the Windows port. The
# repository submodule is pinned to the upstream mbedTLS 3.6.7 commit; compiling
# it here avoids a distro silently substituting an older ABI.
mbedtls_include = include_directories('../third_party/mbedtls/include')
mbedtls_library_dir = '../third_party/mbedtls/library/'
mbedtls_crypto_sources = files(
  mbedtls_library_dir + 'aes.c',
  mbedtls_library_dir + 'aesni.c',
  mbedtls_library_dir + 'aesce.c',
  mbedtls_library_dir + 'aria.c',
  mbedtls_library_dir + 'asn1parse.c',
  mbedtls_library_dir + 'asn1write.c',
  mbedtls_library_dir + 'base64.c',
  mbedtls_library_dir + 'bignum.c',
  mbedtls_library_dir + 'bignum_core.c',
  mbedtls_library_dir + 'bignum_mod.c',
  mbedtls_library_dir + 'bignum_mod_raw.c',
  mbedtls_library_dir + 'block_cipher.c',
  mbedtls_library_dir + 'camellia.c',
  mbedtls_library_dir + 'ccm.c',
  mbedtls_library_dir + 'chacha20.c',
  mbedtls_library_dir + 'chachapoly.c',
  mbedtls_library_dir + 'cipher.c',
  mbedtls_library_dir + 'cipher_wrap.c',
  mbedtls_library_dir + 'constant_time.c',
  mbedtls_library_dir + 'cmac.c',
  mbedtls_library_dir + 'ctr_drbg.c',
  mbedtls_library_dir + 'des.c',
  mbedtls_library_dir + 'dhm.c',
  mbedtls_library_dir + 'ecdh.c',
  mbedtls_library_dir + 'ecdsa.c',
  mbedtls_library_dir + 'ecjpake.c',
  mbedtls_library_dir + 'ecp.c',
  mbedtls_library_dir + 'ecp_curves.c',
  mbedtls_library_dir + 'ecp_curves_new.c',
  mbedtls_library_dir + 'entropy.c',
  mbedtls_library_dir + 'entropy_poll.c',
  mbedtls_library_dir + 'error.c',
  mbedtls_library_dir + 'gcm.c',
  mbedtls_library_dir + 'hkdf.c',
  mbedtls_library_dir + 'hmac_drbg.c',
  mbedtls_library_dir + 'lmots.c',
  mbedtls_library_dir + 'lms.c',
  mbedtls_library_dir + 'md.c',
  mbedtls_library_dir + 'md5.c',
  mbedtls_library_dir + 'memory_buffer_alloc.c',
  mbedtls_library_dir + 'nist_kw.c',
  mbedtls_library_dir + 'oid.c',
  mbedtls_library_dir + 'padlock.c',
  mbedtls_library_dir + 'pem.c',
  mbedtls_library_dir + 'pk.c',
  mbedtls_library_dir + 'pk_ecc.c',
  mbedtls_library_dir + 'pk_wrap.c',
  mbedtls_library_dir + 'pkcs12.c',
  mbedtls_library_dir + 'pkcs5.c',
  mbedtls_library_dir + 'pkparse.c',
  mbedtls_library_dir + 'pkwrite.c',
  mbedtls_library_dir + 'platform.c',
  mbedtls_library_dir + 'platform_util.c',
  mbedtls_library_dir + 'poly1305.c',
  mbedtls_library_dir + 'psa_crypto.c',
  mbedtls_library_dir + 'psa_crypto_aead.c',
  mbedtls_library_dir + 'psa_crypto_cipher.c',
  mbedtls_library_dir + 'psa_crypto_client.c',
  mbedtls_library_dir + 'psa_crypto_driver_wrappers_no_static.c',
  mbedtls_library_dir + 'psa_crypto_ecp.c',
  mbedtls_library_dir + 'psa_crypto_ffdh.c',
  mbedtls_library_dir + 'psa_crypto_hash.c',
  mbedtls_library_dir + 'psa_crypto_mac.c',
  mbedtls_library_dir + 'psa_crypto_pake.c',
  mbedtls_library_dir + 'psa_crypto_rsa.c',
  mbedtls_library_dir + 'psa_crypto_random.c',
  mbedtls_library_dir + 'psa_crypto_se.c',
  mbedtls_library_dir + 'psa_crypto_slot_management.c',
  mbedtls_library_dir + 'psa_crypto_storage.c',
  mbedtls_library_dir + 'psa_its_file.c',
  mbedtls_library_dir + 'psa_util.c',
  mbedtls_library_dir + 'ripemd160.c',
  mbedtls_library_dir + 'rsa.c',
  mbedtls_library_dir + 'rsa_alt_helpers.c',
  mbedtls_library_dir + 'sha1.c',
  mbedtls_library_dir + 'sha256.c',
  mbedtls_library_dir + 'sha512.c',
  mbedtls_library_dir + 'sha3.c',
  mbedtls_library_dir + 'threading.c',
  mbedtls_library_dir + 'timing.c',
  mbedtls_library_dir + 'version.c',
  mbedtls_library_dir + 'version_features.c',
)
mbedtls_x509_sources = files(
  mbedtls_library_dir + 'pkcs7.c',
  mbedtls_library_dir + 'x509.c',
  mbedtls_library_dir + 'x509_create.c',
  mbedtls_library_dir + 'x509_crl.c',
  mbedtls_library_dir + 'x509_crt.c',
  mbedtls_library_dir + 'x509_csr.c',
  mbedtls_library_dir + 'x509write.c',
  mbedtls_library_dir + 'x509write_crt.c',
  mbedtls_library_dir + 'x509write_csr.c',
)
mbedtls_tls_sources = files(
  mbedtls_library_dir + 'debug.c',
  mbedtls_library_dir + 'mps_reader.c',
  mbedtls_library_dir + 'mps_trace.c',
  mbedtls_library_dir + 'net_sockets.c',
  mbedtls_library_dir + 'ssl_cache.c',
  mbedtls_library_dir + 'ssl_ciphersuites.c',
  mbedtls_library_dir + 'ssl_client.c',
  mbedtls_library_dir + 'ssl_cookie.c',
  mbedtls_library_dir + 'ssl_debug_helpers_generated.c',
  mbedtls_library_dir + 'ssl_msg.c',
  mbedtls_library_dir + 'ssl_ticket.c',
  mbedtls_library_dir + 'ssl_tls.c',
  mbedtls_library_dir + 'ssl_tls12_client.c',
  mbedtls_library_dir + 'ssl_tls12_server.c',
  mbedtls_library_dir + 'ssl_tls13_keys.c',
  mbedtls_library_dir + 'ssl_tls13_server.c',
  mbedtls_library_dir + 'ssl_tls13_client.c',
  mbedtls_library_dir + 'ssl_tls13_generic.c',
)
mbedcrypto = static_library(
  'comicchat-mbedcrypto',
  mbedtls_crypto_sources,
  include_directories: [mbedtls_include, inc],
  c_args: ['-w'],
  override_options: ['warning_level=0', 'werror=false'],
  install: false,
)
mbedcrypto_dep = declare_dependency(
  link_with: mbedcrypto,
  include_directories: [mbedtls_include, inc],
)
mbedx509 = static_library(
  'comicchat-mbedx509',
  mbedtls_x509_sources,
  include_directories: [mbedtls_include, inc],
  dependencies: mbedcrypto_dep,
  c_args: ['-w'],
  override_options: ['warning_level=0', 'werror=false'],
  install: false,
)
mbedx509_dep = declare_dependency(
  link_with: mbedx509,
  dependencies: mbedcrypto_dep,
  include_directories: [mbedtls_include, inc],
)
mbedtls = static_library(
  'comicchat-mbedtls',
  mbedtls_tls_sources,
  include_directories: [mbedtls_include, inc],
  dependencies: [mbedx509_dep, mbedcrypto_dep],
  c_args: ['-w'],
  override_options: ['warning_level=0', 'werror=false'],
  install: false,
)
mbedtls_dep = declare_dependency(
  link_with: mbedtls,
  dependencies: [mbedx509_dep, mbedcrypto_dep],
  include_directories: [mbedtls_include, inc],
)

core = static_library(
  'comicchat-core',
  [
    'src/layout.cpp',
    'src/crypto_runtime.cpp',
    'src/config.cpp',
    'src/net/connection_engine.cpp',
    'src/net/dcc_transfer_engine.cpp',
    'src/net/ircv3.cpp',
    'src/net/private_config.cpp',
    'src/net/sts_policy_store.cpp',
    'src/net/native_session.cpp',
    'src/net/sts_session.cpp',
    'src/scheduler.cpp',
    'src/text.cpp',
    'src/assets.cpp',
    'src/avatar_assets.cpp',
    'src/source_raster.cpp',
    'src/sound.cpp',
    'src/balloon.cpp',
    'src/render.cpp',
    'src/comic_page.cpp',
    'src/page.cpp',
    'src/memory.cpp',
  ],
  include_directories: inc,
  cpp_args: core_cpp_args,
  dependencies: [
    threads_dep,
    uv_dep,
    mbedtls_dep,
    zlib_dep,
    freetype_dep,
    harfbuzz_dep,
    icu_uc_dep,
    icu_i18n_dep,
    cairo_dep,
    text_platform_deps,
    platform_deps,
  ],
  install: false,
)
core_dep = declare_dependency(
  link_with: core,
  include_directories: inc,
  dependencies: [
    threads_dep,
    uv_dep,
    mbedtls_dep,
    zlib_dep,
    freetype_dep,
    harfbuzz_dep,
    icu_uc_dep,
    icu_i18n_dep,
    cairo_dep,
    text_platform_deps,
    platform_deps,
  ],
)

# Compile the exact transport API surface consumed by each legacy MFC adapter.
# The same client-local sources are objects in their chat.mak files, so both
# Clang and MSVC reject drift.
static_library(
  'comicchat-adapter-api-compile',
  '../v2.5-beta-1-modern/tests/transport_adapter_api_compile.cpp',
  include_directories: inc,
  install: false,
)
install_data('assets/fonts/comic.ttf', install_dir: get_option('datadir') / 'comic-chat')
install_data(
  'assets/source-raster/chat.ico',
  'assets/source-raster/chatdoc.ico',
  'assets/source-raster/room.ico',
  'assets/source-raster/ruleset.ico',
  'assets/source-raster/avatar.ico',
  'assets/source-raster/backgd.ico',
  'assets/source-raster/ratings.ico',
  'assets/source-raster/whisper.ico',
  'assets/source-raster/notif.ico',
  'assets/source-raster/tosrv.ico',
  'assets/source-raster/tonet.ico',
  'assets/source-raster/balloons.bmp',
  'assets/source-raster/tiki2.bmp',
  'assets/source-raster/toolbar.bmp',
  'assets/source-raster/tabbar.bmp',
  'assets/source-raster/member.bmp',
  'assets/source-raster/oldnew.bmp',
  'assets/source-raster/connect.bmp',
  'assets/source-raster/stopped.bmp',
  'assets/source-raster/inactive.bmp',
  'assets/source-raster/active.bmp',
  'assets/source-raster/texttool.bmp',
  'assets/source-raster/usertool.bmp',
  install_dir: source_raster_install_dir,
)

subdir('assets/icons')

if get_option('frontend')
  sdl_dep = dependency(
    'sdl3',
    fallback: ['sdl3', 'sdl3_dep'],
    include_type: 'system',
    default_options: ['warning_level=0', 'werror=false'],
  )
  executable(
    'comic-chat',
    'src/app.cpp',
    cpp_args: [
      '-DCOMICCHAT_INSTALL_MODERN_ICON_DIR="' + installed_modern_icon_dir + '"',
      '-DCOMICCHAT_VERSION="' + meson.project_version() + '"',
    ],
    dependencies: [core_dep, sdl_dep],
    install: true,
  )
endif

if get_option('tests')
  catch2 = dependency(
    'catch2-with-main',
    fallback: ['catch2', 'catch2_with_main_dep'],
    include_type: 'system',
    default_options: ['tests=false', 'install=false', 'warning_level=0', 'werror=false'],
  )
  unit_tests = executable(
    'comicchat-tests',
    [
      'tests/layout_test.cpp',
      'tests/config_test.cpp',
      'tests/runtime_test.cpp',
      'tests/native_session_test.cpp',
      'tests/text_test.cpp',
      'tests/assets_test.cpp',
      'tests/avatar_assets_test.cpp',
      'tests/source_raster_test.cpp',
      'tests/render_test.cpp',
      'tests/comic_page_test.cpp',
      'tests/page_test.cpp',
      'tests/memory_test.cpp',
      'tests/sound_test.cpp',
    ],
    dependencies: [core_dep, catch2, cairo_dep, freetype_dep, harfbuzz_dep, icu_uc_dep, icu_i18n_dep, zlib_dep],
    cpp_args: [
      '-DCOMICCHAT_TEST_COMICART_DIR="' + meson.project_source_root() + '/../v2.5-beta-1-modern/comicart"',
      '-DCOMICCHAT_TEST_SOURCE_RASTER_DIR="' + meson.project_source_root() + '/assets/source-raster"',
    ],
  )
  headless_test_env = environment()
  headless_test_env.set('COMICCHAT_FONT_PATH', meson.project_source_root() / 'assets/fonts/comic.ttf')
  test('comicchat-headless', unit_tests, timeout: 30, env: headless_test_env)

  modern_icon_audit = find_program('../scripts/build-modern-icons.py')
  test(
    'comicchat-modern-icon-catalog',
    modern_icon_audit,
    args: ['verify'],
    workdir: meson.project_source_root() / '..',
    timeout: 30,
  )

  ircv3_tests = executable(
    'comicchat-ircv3-tests',
    'tests/ircv3_test.cpp',
    dependencies: core_dep,
    cpp_args: ['-DCOMICCHAT_IRC_FIXTURE_DIR="' + meson.project_source_root() + '/tests/fixtures/irc"'],
  )
  test('comicchat-ircv3', ircv3_tests, timeout: 30)

  sts_policy_tests = executable(
    'comicchat-sts-policy-tests',
    'tests/sts_policy_store_test.cpp',
    dependencies: core_dep,
  )
  test('comicchat-sts-policy', sts_policy_tests, timeout: 30)

  sts_session_tests = executable(
    'comicchat-sts-session-tests',
    'tests/sts_session_test.cpp',
    dependencies: core_dep,
  )
  test('comicchat-sts-session', sts_session_tests, timeout: 30)

  transport_tests = executable(
    'comicchat-transport-tests',
    'tests/transport_test.cpp',
    dependencies: [core_dep, catch2],
    cpp_args: ['-DMBEDTLS_DATA_DIR="' + meson.project_source_root() + '/../third_party/mbedtls/framework/data_files"'],
  )
  test('comicchat-transport', transport_tests, timeout: 30)

  dcc_transfer_tests = executable(
    'comicchat-dcc-transfer-tests',
    'tests/dcc_transfer_test.cpp',
    dependencies: [core_dep, catch2],
  )
  test('comicchat-dcc-transfer', dcc_transfer_tests, timeout: 30)

  # These tests exercise the UI contract shared with the native MFC client.
  # Keep them as standalone executables: linking them into comicchat-tests or
  # the frontend would introduce a second main() (and WinMain on Windows).
  # The Windows implementation includes MFC's stdafx.h and is therefore
  # covered by chat.mak below rather than by the portable Windows frontend.
  if host_machine.system() != 'windows'
    modernui_tests = executable(
      'comicchat-modernui-tests',
      [
        '../v2.5-beta-1-modern/tests/modernui_test.cpp',
        '../v2.5-beta-1-modern/modernui.cpp',
      ],
      include_directories: inc,
    )
    test('comicchat-modernui', modernui_tests, timeout: 30)
  endif

  transport_ui_bridge_tests = executable(
    'comicchat-transport-ui-bridge-tests',
    '../v2.5-beta-1-modern/tests/transport_ui_bridge_test.cpp',
    include_directories: inc,
    dependencies: core_dep,
  )
  test('comicchat-transport-ui-bridge', transport_ui_bridge_tests, timeout: 30)

  executable(
    'comicchat-avatar-oracle',
    'tools/avatar_oracle.cpp',
    dependencies: core_dep,
    install: false,
  )
endif

if get_option('benchmarks')
  benchmark_exe = executable(
    'comicchat-bench',
    'tests/bench.cpp',
    dependencies: core_dep,
  )
  test('comicchat-microbench', benchmark_exe, suite: 'perf', timeout: 30, is_parallel: false)
endif
