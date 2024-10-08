import os.path
import sys

import yt.cli.yt_binary

import yt.python.yt.wrapper.bin.yt_cli_all_make.yt_admin as yt_admin

import library.python.resource


def python_yt():
    env = os.environ.copy()
    env["Y_PYTHON_ENTRY_POINT"] = ":main"
    os.execvpe(sys.argv[0], sys.argv, env=env)


def fish_hunts_crickets():
    # curses library is not bundled into arcadia python :(
    # so trying to use system python.
    script = library.python.resource.find("/fish_hunts_crickets.py")
    os.execv("/usr/bin/env", ["/usr/bin/env", "python3", "-c", script] + sys.argv[1:])


MAIN_DICT = {
    "yt": yt.cli.yt_binary.main,
    "yt-admin": yt_admin.main,
    "python-yt": python_yt,
    "fish-hunts-crickets": fish_hunts_crickets,
}


def main():
    binary_name = os.path.basename(sys.argv[0])
    main_func = MAIN_DICT.get(binary_name, None)
    if main_func is None:
        print(f"Unknown binary name: '{binary_name}'. Known names:", file=sys.stderr)
        for name in MAIN_DICT:
            print(f" - {name}", file=sys.stderr)
        exit(54)
    main_func()


if __name__ == "__main__":
    main()
