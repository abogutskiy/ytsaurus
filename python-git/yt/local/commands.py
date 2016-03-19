from configs_provider import ConfigsProviderFactory

from yt.environment import YTEnv
from yt.environment.init_cluster import initialize_world
from yt.wrapper.common import generate_uuid
from yt.wrapper.client import Yt
from yt.common import YtError, require, update
import yt.yson as yson
import yt.json as json
import yt.wrapper as yt

import os
import signal
import errno
import logging
import shutil
import socket

logger = logging.getLogger("Yt.local")

class YTEnvironment(YTEnv):
    def __init__(self, master_config, scheduler_config, node_config, proxy_config):
        super(YTEnvironment, self).__init__()
        self._master_config = master_config
        self._scheduler_config = scheduler_config
        self._node_config = node_config
        self._proxy_config = proxy_config

    def modify_master_config(self, config):
        update(config, self._load_config(self._master_config))

    def modify_node_config(self, config):
        update(config, self._load_config(self._node_config))

    def modify_scheduler_config(self, config):
        update(config, self._load_config(self._scheduler_config))

    def modify_proxy_config(self, config):
        update(config, self._load_config(self._proxy_config, is_proxy_config=True))

    @staticmethod
    def _load_config(path, is_proxy_config=False):
        if path is None:
            return {}
        with open(path) as f:
            if not is_proxy_config:
                return yson.load(f)
            else:
                return json.load(f)

def get_root_path(path=None):
    if path is not None:
        return path
    else:
        return os.environ.get("YT_LOCAL_ROOT_PATH", os.getcwd())

def _get_bool_from_env(name, default=False):
    value = os.environ.get(name, None)
    if value is None:
        return default
    try:
        value = int(value)
    except:
        return default
    return value == 1

def _get_attributes_from_local_dir(local_path):
    meta_file_path = os.path.join(local_path, ".meta")
    if os.path.isfile(meta_file_path):
        with open(meta_file_path) as f:
            try:
                meta = yson.load(f)
            except yson.YsonError:
                logger.exception("Failed to load meta file {0}, meta will not be processed".format(meta_file_path))
                return {}
            return meta.get("attributes", {})
    return {}

def _create_map_node_from_local_dir(local_path, dest_path, client):
    attributes = _get_attributes_from_local_dir(local_path)
    client.create("map_node", dest_path, attributes=attributes, ignore_existing=True)

def _create_node_from_local_file(local_filename, dest_filename, client):
    if not os.path.isfile(local_filename + ".meta"):
        logger.warning("Found file {0} without meta info, skipping".format(file))
        return

    with open(local_filename + ".meta") as f:
        try:
            meta = yson.load(f)
        except yson.YsonError:
            logger.exception("Failed to load meta file for table {0}, skipping".format(local_filename))
            return

        if meta["type"] != "table":
            logger.warning("Found file {0} with currently unsupported type {1}" \
                           .format(file, meta["type"]))
            return

        if "format" not in meta:
            logger.warning("Found table {0} with unspecified format".format(local_filename))
            return

        with open(local_filename) as table_file:
            client.write_table(dest_filename, table_file, format=meta["format"])

        attributes = meta.get("attributes", {})
        for key in attributes:
            client.set_attribute(dest_filename, key, attributes[key])

def _synchronize_cypress_with_local_dir(local_cypress_dir, client):
    cypress_path_prefix = "//"

    local_cypress_dir = os.path.abspath(local_cypress_dir)
    require(os.path.exists(local_cypress_dir),
            lambda: YtError("Local Cypress directory does not exist"))

    root_attributes = _get_attributes_from_local_dir(local_cypress_dir)
    for key in root_attributes:
        client.set_attribute("/", key, root_attributes[key])

    for root, dirs, files in os.walk(local_cypress_dir):
        rel_path = os.path.abspath(root)[len(local_cypress_dir)+1:]  # +1 to skip last /
        for dir in dirs:
            _create_map_node_from_local_dir(os.path.join(root, dir),
                                            os.path.join(cypress_path_prefix, rel_path, dir),
                                            client)
        for file in files:
            if file.endswith(".meta"):
                continue
            _create_node_from_local_file(os.path.join(root, file),
                                         os.path.join(cypress_path_prefix, rel_path, file),
                                         client)

def _is_pid_exists(pid):
    try:
        os.kill(pid, 0)
    except OSError as err:
        if err.errno == errno.ESRCH:
            return False
        elif err.errno == errno.EPERM:
            return True
        else:
            # According to "man 2 kill" possible error values are
            # (EINVAL, EPERM, ESRCH)
            raise
    return True

def _read_pids_file(pids_file_path):
    with open(pids_file_path) as f:
        return map(int, f)

def log_started_instance_info(environment, start_proxy, prepare_only):
    logger.info("Local YT {0}, id: {1}".format(
        "prepared" if prepare_only else "started",
        environment.id))
    if start_proxy:
        logger.info("Proxy address: {0}".format(environment.get_proxy_address()))

def _safe_kill(pid):
    try:
        os.killpg(pid, signal.SIGKILL)
    except OSError as err:
        if err.errno == errno.EPERM:
            logger.error("Failed to kill process with pid {0}, access denied".format(pid))
        elif err.errno == errno.ESRCH:
            logger.warning("Failed to kill process with pid {0}, process not found".format(pid))
        else:
            # According to "man 2 killpg" possible error values are
            # (EINVAL, EPERM, ESRCH)
            raise

def _initialize_world(client):
    initialize_world(client)
    # Create tablet cell
    attributes = {
        "changelog_replication_factor": 1,
        "changelog_read_quorum": 1,
        "changelog_write_quorum": 1
    }
    client.create("tablet_cell", attributes=attributes)
    # Used to automatically determine local mode from python wrapper.
    client.set("//sys/@local_mode_fqdn", socket.getfqdn())

def start(master_count=1, node_count=1, scheduler_count=1, start_proxy=True,
          master_config=None, node_config=None, scheduler_config=None, proxy_config=None,
          proxy_port=None, id=None, local_cypress_dir=None, use_proxy_from_yt_source=False,
          enable_debug_logging=False, tmpfs_path=None, port_range_start=None, fqdn=None, path=None,
          prepare_only=False):

    require(master_count >= 1, lambda: YtError("Cannot start local YT instance without masters"))

    path = get_root_path(path)
    sandbox_id = id if id is not None else generate_uuid()
    require("/" not in sandbox_id, lambda: YtError('Instance id should not contain path separator "/"'))

    environment = YTEnvironment(master_config, scheduler_config, node_config, proxy_config)

    environment.NUM_MASTERS = master_count
    environment.NUM_NONVOTING_MASTERS = 0
    environment.NUM_NODES = node_count
    environment.NUM_SCHEDULERS = scheduler_count
    environment.START_PROXY = start_proxy
    environment.START_SECONDARY_MASTER_CELLS = False
    environment.CONFIGS_PROVIDER_FACTORY = ConfigsProviderFactory

    # Enable capturing stderrs to file
    os.environ["YT_CAPTURE_STDERR_TO_FILE"] = "1"

    use_proxy_from_yt_source = use_proxy_from_yt_source or \
            _get_bool_from_env("YT_LOCAL_USE_PROXY_FROM_SOURCE")
    environment.USE_PROXY_FROM_PACKAGE = not use_proxy_from_yt_source

    sandbox_path = os.path.join(path, sandbox_id)

    # XXX(klyachin): fcntl.flock should be used to avoid concurrent start
    pids_file_path = os.path.join(sandbox_path, "pids.txt")
    # Consider instance running if "pids.txt" file exists
    if os.path.isfile(pids_file_path):
        pids = _read_pids_file(pids_file_path)
        alive_pids = filter(_is_pid_exists, pids)
        if len(pids) == 0 or len(pids) > len(alive_pids):
            for pid in alive_pids:
                logger.warning("Killing alive process (pid: {0}) from previously run instance".format(pid))
                _safe_kill(pid)
            os.remove(pids_file_path)
        else:
            raise YtError("Instance with id {0} is already running".format(sandbox_id))

    sandbox_tmpfs_path = os.path.join(tmpfs_path, sandbox_id) if tmpfs_path else None
    environment.start(sandbox_path, pids_file_path,
                      proxy_port=proxy_port,
                      enable_debug_logging=enable_debug_logging,
                      preserve_working_dir=True,
                      tmpfs_path=sandbox_tmpfs_path,
                      enable_ui=True,
                      port_range_start=port_range_start,
                      # XXX(asaitgalin): For parallel testing purposes.
                      port_locks_path=os.environ.get("YT_LOCAL_PORT_LOCKS_PATH"),
                      fqdn=fqdn,
                      prepare_only=prepare_only)

    environment.id = sandbox_id

    if not prepare_only:
        if start_proxy:
            client = Yt(proxy=environment.get_proxy_address())
        else:
            client = environment.create_native_client("driver")

        _initialize_world(client)
        if local_cypress_dir is not None:
            _synchronize_cypress_with_local_dir(local_cypress_dir, client)

    log_started_instance_info(environment, start_proxy, prepare_only)

    return environment

def _is_stopped(id, path=None):
    sandbox_path = os.path.join(get_root_path(path), id)

    if not os.path.isdir(sandbox_path):
        return True

    pids_file_path = os.path.join(sandbox_path, "pids.txt")
    if os.path.isfile(pids_file_path):
        alive_pids = filter(_is_pid_exists, _read_pids_file(pids_file_path))
        if not alive_pids:
            os.remove(pids_file_path)
        return not alive_pids
    return True

def _is_exists(id, path=None):
    sandbox_path = os.path.join(get_root_path(path), id)
    return os.path.isdir(sandbox_path)

def stop(id, remove_working_dir=False, path=None):
    require(_is_exists(id, path),
            lambda: yt.YtError("Local YT with id {0} not found".format(id)))
    require(not _is_stopped(id, path),
            lambda: yt.YtError("Local YT with id {0} is already stopped".format(id)))

    pids_file_path = os.path.join(get_root_path(path), id, "pids.txt")
    for pid in _read_pids_file(pids_file_path):
        _safe_kill(pid)
    os.remove(pids_file_path)

    if remove_working_dir:
        delete(id, force=True, path=path)

def delete(id, force=False, path=None):
    require(_is_exists(id, path) or force,
            lambda: yt.YtError("Local YT with id {0} not found".format(id)))
    require(_is_stopped(id, path),
            lambda: yt.YtError("Local YT environment with id {0} is not stopped".format(id)))

    shutil.rmtree(os.path.join(get_root_path(path), id), ignore_errors=True)

def get_proxy(id, path=None):
    require(_is_exists(id, path), yt.YtError("Local YT with id {0} not found".format(id)))

    info_file_path = os.path.join(get_root_path(path), id, "info.yson")
    require(os.path.exists(info_file_path),
            lambda: yt.YtError("Information file for local YT with id {0} not found".format(id)))

    with open(info_file_path) as f:
        info = yson.load(f)
        if not "proxy" in info:
            raise yt.YtError("Local YT with id {0} does not have started proxy".format(id))

    return info["proxy"]["address"]

def list_instances(path=None):
    path = get_root_path(path)
    result = []
    for dir_ in os.listdir(path):
        full_path = os.path.join(path, dir_)
        if not os.path.isdir(full_path):
            logger.info("Found unknown object in instances root: %s", full_path)
            continue

        info_file = os.path.join(full_path, "info.yson")
        if not os.path.exists(info_file):
            logger.info("Path %s does not seem to contain valid local YT instance", full_path)
            continue

        stopped = _is_stopped(dir_, path)
        if stopped:
            result.append((dir_, "stopped", None))
        else:
            try:
                proxy_address = get_proxy(dir_, path)
            except yt.YtError:
                proxy_address = None
            result.append((dir_, "running", proxy_address))

    return result
