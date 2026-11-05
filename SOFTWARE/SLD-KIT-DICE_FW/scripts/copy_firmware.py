# SPDX-License-Identifier: MIT
# Copyright (c) 2026 OLIMEX LTD BULGARIA
#
# PlatformIO post-build step: after firmware.bin is built, copy it to
# firmware/<env>.bin next to the sources (for example firmware/dice.bin),
# so the latest image is easy to find and to flash with other tools.

Import("env")

import os
import shutil


def copy_bin(source, target, env):
    src = str(target[0])
    out_dir = os.path.join(env.subst("$PROJECT_DIR"), "firmware")
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, env.subst("$PIOENV") + ".bin")
    shutil.copyfile(src, dst)
    print("Firmware copied to %s (%d bytes)" % (os.path.relpath(dst, env.subst("$PROJECT_DIR")), os.path.getsize(dst)))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_bin)
