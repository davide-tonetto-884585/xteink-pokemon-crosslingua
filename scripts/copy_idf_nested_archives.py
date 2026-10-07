"""
PlatformIO post: script: also copy NESTED ESP-IDF archives when pioarduino
rebuilds the framework for `custom_sdkconfig`.

pioarduino 55.03.37 rebuilds every IDF component with this project's
sdkconfig and copies the archives over the prebuilt Arduino libs, but its
`idf_lib_copy` only looks one directory deep. The vendored mbedTLS archives
live at `esp-idf/mbedtls/mbedtls/library/` (libmbedcrypto.a, libmbedx509.a and
the SSL libmbedtls.a, shipped as libmbedtls_2.a), so they were compiled and
then thrown away: the firmware linked the rebuilt mbedTLS port (esp-tls, the
CA bundle callback) against the stock crypto archives. The two halves disagree,
and every RSA certificate check failed with PK verify 0x4290 - so on X3/X4 no
HTTPS server with an RSA chain (Google Translate, Gemini, ...) could be
reached. ECDSA-only chains (api.github.com) happened to work.

Newer pioarduino releases walk the tree recursively (copy_idf_component_archives).
This mirrors that walk, including its naming of duplicate basenames (libfoo.a,
libfoo_2.a, ...), and runs right before the platform's own copy, which then
rewrites the top-level archives with identical files. It is a no-op for builds
that do not rebuild the IDF (no `esp-idf` folder in the build directory).
"""

import os
import shutil
from pathlib import Path

Import("env")  # noqa: F821


def copy_nested_idf_archives(source, target, env):
    lib_src = Path(env.subst("$PROJECT_BUILD_DIR")) / env["PIOENV"] / "esp-idf"
    if not lib_src.is_dir():
        return
    libs_pkg = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    lib_dst = Path(libs_pkg) / env.BoardConfig().get("build.mcu") / "lib"
    if not lib_dst.is_dir():
        return

    copied_names = {}
    nested = 0
    for folder in sorted(lib_src.iterdir()):
        if not folder.is_dir():
            continue
        for root, dirs, files in os.walk(folder, topdown=True):
            dirs.sort()
            files.sort()
            for filename in files:
                if not filename.endswith(".a"):
                    continue
                copied_names[filename] = copied_names.get(filename, 0) + 1
                count = copied_names[filename]
                dst_name = filename if count == 1 else f"{filename[:-2]}_{count}.a"
                if Path(root) != folder:
                    shutil.copyfile(Path(root) / filename, lib_dst / dst_name)
                    nested += 1
    print(f"*** Copied {nested} nested IDF archives (e.g. vendored mbedTLS) to Arduino framework ***")


env.AddPreAction("checkprogsize", copy_nested_idf_archives)  # noqa: F821
