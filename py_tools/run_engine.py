import os
import subprocess
import sys


def main():
    if len(sys.argv) < 2:
        print("Usage: python py_tools/run_engine.py <config.yaml>")
        return 1
    config = sys.argv[1]
    exe = os.path.join(os.path.dirname(__file__), "..", "bin", "hft_engine")
    exe = os.path.abspath(exe)
    if not os.path.isfile(exe):
        print(f"Binary not found: {exe}")
        return 1

    env = os.environ.copy()
    # Ensure current repo is visible to Python imports
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    env["PYTHONPATH"] = repo_root + os.pathsep + env.get("PYTHONPATH", "")

    return subprocess.call([exe, config], env=env)


if __name__ == "__main__":
    raise SystemExit(main())
