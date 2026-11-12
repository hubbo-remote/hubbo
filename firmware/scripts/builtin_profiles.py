# PlatformIO pre-build script: the built-in device profiles (the repo's
# profiles/*.json) as a generated .cpp in the build directory, by core's own
# generator (core/tools/embed_profiles.py, which leaves an unchanged file
# alone, so nothing recompiles), so the firmware and hub-sim embed the same
# bytes. It defines hubbo::config::kBuiltinProfileFiles, which core's
# builtinProfiles() reads. Nothing generated lands in git.
import os
import subprocess
import sys

Import("env")  # noqa: F821  (SCons)

PROJECT = env.subst("$PROJECT_DIR")  # noqa: F821
REPO = os.path.normpath(os.path.join(PROJECT, ".."))
TOOL = os.path.join(REPO, "core", "tools", "embed_profiles.py")
PROFILES = os.path.join(REPO, "profiles")
GEN_DIR = os.path.join(env.subst("$BUILD_DIR"), "profiles_gen")  # noqa: F821

os.makedirs(GEN_DIR, exist_ok=True)
if subprocess.call([sys.executable, TOOL, PROFILES, os.path.join(GEN_DIR, "builtin_profiles.cpp")]) != 0:
    sys.stderr.write("\n*** builtin profiles: %s failed\n\n" % TOOL)
    env.Exit(1)  # noqa: F821

# Built here, before the library finder adds core's headers for our sources.
env.Append(CPPPATH=[os.path.join(REPO, "core", "include")])  # noqa: F821
env.BuildSources(os.path.join("$BUILD_DIR", "profiles_obj"), GEN_DIR)  # noqa: F821
