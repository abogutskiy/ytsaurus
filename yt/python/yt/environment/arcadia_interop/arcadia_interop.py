from __future__ import print_function

import yt.logger as yt_logger

import os
import fcntl
import time
import shutil
import logging
import subprocess

try:
    import yatest.common as yatest_common
except ImportError:
    yatest_common = None


def sudo_rmtree(path):
    subprocess.check_call(["sudo", "rm", "-rf", path])


def sudo_move(src_path, dst_path):
    subprocess.check_call(["sudo", "mv", src_path, dst_path])


def is_inside_arcadia(inside_arcadia):
    if inside_arcadia is None:
        inside_arcadia = int(yatest_common.get_param("inside_arcadia", True))
    return inside_arcadia


def search_binary_path(binary_name, build_path_dir=None):
    """
    Search for binary with given name in arcadia build_path.
    If build_path_dir is specified search in this subdirectory.
    :param binary_name: name of the binary, e.g. ytserver-all or logrotate
    :param build_path_dir: if present, subtree is yatest.common.build_path() + build_path_dir
    :return:
    """
    binary_root = yatest_common.build_path()
    if build_path_dir is not None:
        binary_root = os.path.join(binary_root, build_path_dir)
    binary_root = os.path.abspath(binary_root)

    for dirpath, _, filenames in os.walk(binary_root):
        for f in filenames:
            if f == binary_name:
                result = os.path.join(dirpath, binary_name)
                return result
    raise RuntimeError("binary {} is not found in {}".format(binary_name, binary_root))


SUDO_WRAPPER ="""#!/bin/sh

exec sudo -En {} {} {} {} "$@"
"""


def insert_sudo_wrapper(bin_dir):
    sudofixup = search_binary_path("yt-sudo-fixup")

    for binary in ["ytserver-exec", "ytserver-job-proxy", "ytserver-tools"]:
        bin_path = os.path.join(bin_dir, binary)
        orig_path = os.path.join(bin_dir, binary + ".orig")
        if not os.path.exists(bin_path):
            continue

        os.rename(bin_path, orig_path)

        with open(bin_path, "w") as trampoline:
            trampoline.write(SUDO_WRAPPER.format(sudofixup, os.getuid(), orig_path, binary))
            os.chmod(bin_path, 0o755)


def get_binary_path(path, arcadia_root, **kwargs):
    if arcadia_root is None:
        return search_binary_path(path, **kwargs)
    else:
        return os.path.join(arcadia_root, path)

PROGRAMS = [("master", "master/bin"),
            ("clock", "clock_server/bin"),
            ("timestamp-provider", "timestamp_provider/bin"),
            ("discovery", "discovery_server/bin"),
            ("node", "node/bin"),
            ("job-proxy", "job_proxy/bin"),
            ("exec", "exec/bin"),
            ("proxy", "rpc_proxy/bin"),
            ("http-proxy", "http_proxy/bin"),
            ("tools", "tools/bin"),
            ("scheduler", "scheduler/bin"),
            ("controller-agent", "controller_agent/bin"),
            ("cell-balancer", "cell_balancer/bin"),
            ("tablet-balancer", "tablet_balancer/bin"),
            ("master-cache", "master_cache/bin"),
            ("queue-agent", "queue_agent/bin")]


def prepare_yt_binaries(destination,
                        source_prefix="", arcadia_root=None, inside_arcadia=None,
                        use_from_package=False, package_dir=None, copy_ytserver_all=False, ytserver_all_suffix=None,
                        need_suid=False, component_whitelist=None):
    if use_from_package:
        if package_dir is None:
            package_dir = "yt/yt"
        ytserver_all = search_binary_path("ytserver-all", build_path_dir=package_dir)
    else:
        ytserver_all = get_binary_path("ytserver-all", arcadia_root, build_path_dir="yt")
    if copy_ytserver_all:
        ytserver_all_destination = os.path.join(destination, "ytserver-all")
        if ytserver_all_suffix is not None:
            ytserver_all_destination += "." + ytserver_all_suffix
        shutil.copy(ytserver_all, ytserver_all_destination)
        ytserver_all = ytserver_all_destination

    programs = PROGRAMS

    if component_whitelist is not None:
        programs = [(component, path) for component, path in programs if component in component_whitelist]

    for binary, server_dir in programs:
        dst_path = os.path.join(destination, "ytserver-" + binary)
        if copy_ytserver_all:
            os.link(ytserver_all, dst_path)
        else:
            os.symlink(ytserver_all, dst_path)

    if need_suid:
        insert_sudo_wrapper(destination)


def copy_binary(destination, binary_name, arcadia_root, *source_paths):
    for source_path in source_paths:
        try:
            binary_path = get_binary_path(binary_name, arcadia_root, build_path_dir=source_path)
            shutil.copy(binary_path, os.path.join(destination, binary_name))
            return
        except:
            continue
    raise RuntimeError("binary {} is not found in {}".format(binary_name, source_paths))


def copy_misc_binaries(destination, arcadia_root=None):
    copy_binary(destination, "yt_env_watcher", arcadia_root, "yt/python/yt/environment", "yt/packages/latest/yt/python/yt/environment")
    copy_binary(destination, "logrotate", arcadia_root, "infra/nanny/logrotate", "yt/packages/latest/infra/nanny/logrotate")


# Supposed to be used in core YT components only.
def prepare_yt_environment(destination, artifact_components=None, **kwargs):
    if "package_dir" in kwargs:
        assert artifact_components is None

    artifact_components = artifact_components or {}
    bin_dir = os.path.join(destination, "bin")
    lock_path = os.path.join(destination, "lock")
    prepared_path = os.path.join(destination, "prepared")

    try:
        lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR)
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except IOError:
        while not os.path.exists(prepared_path):
            time.sleep(0.1)
        return bin_dir

    # Compute which components should be taken from trunk and which from artifacts.

    all_components = [program for program, _ in PROGRAMS]

    trunk_components = set(program for program, _ in PROGRAMS)
    for artifact_name, components in artifact_components.items():
        for component in components:
            if component not in all_components:
                raise RuntimeError("Unknown artifact component {}; known components are {}".format(
                                   component, all_components))
            if component not in trunk_components:
                raise RuntimeError("Artifact component {} is specified multiple times".format(component))
            trunk_components.remove(component)

    if not os.path.exists(bin_dir):
        os.makedirs(bin_dir)

        if trunk_components:
            if "package_dir" not in kwargs:
                prepare_yt_binaries(bin_dir,
                                    component_whitelist=trunk_components,
                                    use_from_package=True,
                                    package_dir="yt/yt/packages/tests_package",
                                    ytserver_all_suffix="trunk",
                                    **kwargs)
            else:
                prepare_yt_binaries(bin_dir,
                                    component_whitelist=trunk_components,
                                    ytserver_all_suffix="trunk",
                                    **kwargs)

        for version, components in artifact_components.items():
            prepare_yt_binaries(bin_dir,
                                component_whitelist=components,
                                use_from_package=True,
                                package_dir="yt/packages/{}".format(version),
                                ytserver_all_suffix=version,
                                **kwargs)

        copy_misc_binaries(bin_dir, arcadia_root=kwargs.get("arcadia_root"))

    if yatest_common is not None:
        yt_logger.LOGGER = logging.getLogger()
        yt_logger.LOGGER.setLevel(logging.DEBUG)
        if yatest_common.context.test_stderr:
            yt_logger.LOGGER.addHandler(logging.StreamHandler())
        yt_logger.set_formatter(yt_logger.BASIC_FORMATTER)

    with open(prepared_path, "w"):
        pass

    return bin_dir


# Absolute paths to binaries must be provided for the shutil.copy.
def collect_cores(pids, working_directory, binaries, logger=None):
    cores_path = os.path.join(working_directory, "cores")
    if not os.path.isdir(cores_path):
        os.makedirs(cores_path)

    has_core_files = False
    for pid in pids:
        core_file = yatest_common.cores.recover_core_dump_file(
            # Temporarily collect all cores since problem with core file names.
            # yatest_common.binary_path("yp/server/master/bin/ypserver-master"),
            "*",
            # Process working directory.
            working_directory,
            pid)
        if core_file is not None:
            if logger is not None:
                logger.info("Core file found: " + core_file)
            try:
                shutil.move(core_file, cores_path)
            except IOError:
                # Ignore errors (it can happen for foreign cores).
                pass
            has_core_files = True

    if not has_core_files:
        if logger is not None:
            logger.debug(
                "No core files found (working_directory: %s, pids: %s)",
                working_directory,
                str(pids))
    else:
        # Save binaries.
        for binary in binaries:
            shutil.copy(binary, cores_path)


def save_sandbox(sandbox_path, output_subpath):
    if yatest_common is None:
        return

    output_path = os.path.join(yatest_common.output_path(), output_subpath)
    if output_path == sandbox_path:
        return

    if os.environ.get("YT_OUTPUT") is not None and sandbox_path.startswith(os.environ.get("YT_OUTPUT")):
        return

    # Do not copy sandbox if it stored in output ram drive and consistent with output_subpath.
    if yatest_common.output_ram_drive_path() is not None and \
            sandbox_path.startswith(yatest_common.output_ram_drive_path()) and \
            sandbox_path.strip("/").endswith(output_subpath.strip("/")):
        return

    os.makedirs(output_path)
    sudo_move(sandbox_path, output_path)


def get_gdb_path():
    if yatest_common is None:
        return "gdb"
    else:
        return yatest_common.gdb_path()


def remove_runtime_data(working_directory):
    runtime_data_paths = []
    for root, dirs, files in os.walk(working_directory):
        if os.path.basename(root) == "runtime_data":
            runtime_data_paths.append(os.path.join(working_directory, root))

    yt_logger.info("Removing runtime data (paths: {})".format(", ".join(runtime_data_paths)))

    for path in runtime_data_paths:
        failed = False
        try:
            shutil.rmtree(path, ignore_errors=True)
        except IOError:
            yt_logger.exception("Failed to remove {} without sudo".format(path))
            failed = True

        if failed:
            try:
                sudo_rmtree(path)
            except subprocess.CalledProcessError:
                yt_logger.exception("Failed to remove {} with sudo".format(path))
