import subprocess
Import("env")

try:
    git_hash = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=env["PROJECT_DIR"]
    ).decode().strip()
    dirty = subprocess.call(
        ["git", "diff", "--quiet", "HEAD"],
        cwd=env["PROJECT_DIR"]
    )
    if dirty != 0:
        git_hash += "-dirty"
except Exception:
    git_hash = "unknown"

env.Append(CPPDEFINES=[("GIT_HASH", env.StringifyMacro(git_hash))])
