#!/usr/bin/env python3
"""Pi-side implementation for pi-dev. Run as the existing UxPlay user."""

import datetime
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import time


ROOT = Path.home() / "uxplay-dev"
MARKER = ROOT / ".managed-by-uxplay-pi-dev"
STATE = ROOT / "state"
LOCK = STATE / "lock"
SOURCE = ROOT / "src"
BUILD = ROOT / "build" / "Release"
RELEASES = ROOT / "releases"
DROPIN = Path("/etc/systemd/system/uxplay.service.d/90-uxplay-dev.conf")
HEADER = "# Managed by UxPlay pi-dev; do not edit.\n"
SERVICE = "uxplay.service"
IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}\Z")
SIMPLE_PATH = re.compile(r"/[A-Za-z0-9_./-]+\Z")
OPERATION = None


class Failure(RuntimeError):
    pass


def process_alive(pid, group=False):
    if not isinstance(pid, int) or pid <= 0:
        return False
    try:
        (os.killpg if group else os.kill)(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def operation_busy(record):
    return bool(record and (process_alive(record.get("pid")) or
                            process_alive(record.get("child_pid")) or
                            process_alive(record.get("child_group"), group=True)))


def run(args, capture=True, check=True, interactive=False):
    # Keep compiler descendants together so a lost SSH connection can stop them.
    grouped = not interactive
    process = subprocess.Popen(args, text=True, stdout=subprocess.PIPE if capture else None,
                               stderr=subprocess.PIPE if capture else None,
                               start_new_session=grouped)
    try:
        if OPERATION is not None:
            OPERATION.update(child_pid=process.pid, child_group=process.pid if grouped else None)
            write_json(LOCK / "operation.json", OPERATION)
        stdout, stderr = process.communicate()
        if grouped and process_alive(process.pid, group=True):
            raise Failure("Command left a child process running; retaining protection against overlapping deployments.")
    except BaseException:
        try:
            (os.killpg if grouped else os.kill)(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                (os.killpg if grouped else os.kill)(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate(timeout=5)
        raise
    finally:
        if OPERATION is not None and not process_alive(process.pid, group=grouped):
            OPERATION.update(child_pid=None, child_group=None)
            write_json(LOCK / "operation.json", OPERATION)
    result = subprocess.CompletedProcess(args, process.returncode, stdout, stderr)
    if check and result.returncode:
        detail = (result.stderr or result.stdout or "").strip() if capture else ""
        raise Failure("Command failed: " + shlex.join(map(str, args)) +
                      ("\n" + detail if detail else ""))
    return result


def identifier(value):
    if not IDENTIFIER.fullmatch(value):
        raise Failure("Invalid deployment identifier or lock token.")
    return value


def no_symlink(path):
    """Reject symlinks in every existing component, including the final one."""
    path = Path(path)
    for part in (path, *path.parents):
        if part.is_symlink():
            raise Failure("Refusing symbolic link: " + str(part))
    return path


def write_text(path, value):
    no_symlink(path)
    temporary = path.with_name(path.name + ".tmp-" + str(os.getpid()))
    no_symlink(temporary)
    with temporary.open("x", encoding="utf-8") as stream:
        stream.write(value)
    os.replace(temporary, path)


def write_json(path, value):
    write_text(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def read_json(path, default=None):
    no_symlink(path)
    if not path.exists():
        return default
    return json.loads(path.read_text(encoding="utf-8"))


def managed(create=False):
    no_symlink(ROOT)
    if not ROOT.exists():
        if not create:
            raise Failure("Development directory is not prepared.")
        ROOT.mkdir(mode=0o700)
        MARKER.write_text("UxPlay pi-dev v1\n", encoding="utf-8")
    no_symlink(MARKER)
    if not MARKER.is_file() or MARKER.read_text() != "UxPlay pi-dev v1\n":
        raise Failure("Refusing an existing directory without the pi-dev marker: " + str(ROOT))
    for directory in (SOURCE, ROOT / "build", BUILD, RELEASES, STATE):
        no_symlink(directory)
        if create:
            directory.mkdir(mode=0o700, exist_ok=True)
        elif not directory.is_dir():
            raise Failure("Managed development directory is missing: " + str(directory))


def require_lock(token):
    identifier(token)
    managed()
    no_symlink(LOCK / "token")
    if not (LOCK / "token").is_file() or (LOCK / "token").read_text().strip() != token:
        raise Failure("This operation does not own the development lock.")


def prepare(token):
    identifier(token)
    managed(create=True)
    no_symlink(LOCK)
    try:
        LOCK.mkdir(mode=0o700)
    except FileExistsError:
        raise Failure("Another operation holds " + str(LOCK) + ". Inspect it before recovering a stale lock.")
    (LOCK / "token").write_text(token + "\n", encoding="utf-8")
    print("Development workspace prepared; lock acquired.")


def finish(token):
    identifier(token)
    managed()
    no_symlink(LOCK)
    if not LOCK.exists():
        print("No development lock remains.")
        return
    require_lock(token)
    operation = read_json(LOCK / "operation.json")
    if operation_busy(operation):
        raise Failure("A remote operation or compiler process is still running; retaining the development lock.")
    if operation is not None:
        (LOCK / "operation.json").unlink()
    (LOCK / "token").unlink()
    LOCK.rmdir()
    print("Development lock released.")


def release_path(release_id):
    return no_symlink(RELEASES / identifier(release_id))


def build(token, release_id):
    require_lock(token)
    destination = release_path(release_id)
    if destination.exists():
        raise Failure("Release already exists: " + release_id)
    metadata = read_json(SOURCE / ".pi-build.json")
    if not isinstance(metadata, dict) or metadata.get("release_id") != release_id:
        raise Failure("Source metadata is missing or names a different release.")
    if not isinstance(metadata.get("revision"), str) or not isinstance(metadata.get("dirty"), bool):
        raise Failure("Source metadata must include revision and boolean dirty fields.")
    for path in SOURCE.rglob("*"):
        if path.is_symlink():
            raise Failure("Source snapshot contains a symbolic link: " + str(path))
    stage = no_symlink(RELEASES / ("." + release_id + ".building"))
    stage.mkdir(mode=0o700)
    try:
        print("Saving this source snapshot and compiling on the Pi.", flush=True)
        with tarfile.open(stage / "source.tar.gz", "w:gz") as archive:
            archive.add(SOURCE, arcname="source")
        launcher = shutil.which("ccache") or ""
        run(["cmake", "-S", str(SOURCE), "-B", str(BUILD),
             "-DCMAKE_BUILD_TYPE=Release", "-DNO_X11_DEPS=ON",
             "-DCMAKE_C_COMPILER_LAUNCHER=" + launcher,
             "-DCMAKE_CXX_COMPILER_LAUNCHER=" + launcher], capture=False)
        run(["cmake", "--build", str(BUILD), "--parallel", "2"], capture=False)
        binary = no_symlink(BUILD / "uxplay")
        run([str(binary), "-v"], capture=False)
        shutil.copy2(binary, stage / "uxplay")
        (stage / "uxplay").chmod(0o755)
        metadata.update({
            "built_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "machine": os.uname().machine,
            "kernel": os.uname().release,
            "build_type": "Release", "no_x11_deps": True, "parallel": 2,
            "ccache": bool(launcher),
            "binary_sha256": hashlib.sha256((stage / "uxplay").read_bytes()).hexdigest(),
            "source_archive_sha256": hashlib.sha256((stage / "source.tar.gz").read_bytes()).hexdigest(),
        })
        write_json(stage / "build.json", metadata)
        os.rename(stage, destination)
    except BaseException:
        shutil.rmtree(stage)
        raise
    print("Built release " + release_id + "; the running receiver was not changed.")


def service():
    properties = "LoadState,User,ExecStart,ActiveState,SubState,MainPID,NRestarts,DropInPaths"
    output = run(["systemctl", "show", SERVICE, "--no-pager", "--property=" + properties]).stdout
    values = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
    if values.get("LoadState") != "loaded":
        raise Failure("The existing uxplay.service is not loaded.")
    username = pwd.getpwuid(os.getuid()).pw_name
    if values.get("User") not in (username, str(os.getuid())):
        raise Failure("uxplay.service must run as the current account (" + username + ").")
    raw = values.get("ExecStart", "")
    match = re.fullmatch(r"\{ path=(\S+) ; argv\[\]=(.*?) ; ignore_errors=no ; [^{}]*\}", raw)
    if not match:
        raise Failure("Cannot safely interpret this service's ExecStart; preserve its custom configuration manually.")
    command = shlex.split(match.group(2))
    if len(command) != 3 or command[1] != "-rc" or command[0] != match.group(1):
        raise Failure("Expected exactly: absolute-uxplay-path -rc absolute-config-path; custom arguments need manual integration.")
    if not all(SIMPLE_PATH.fullmatch(value) for value in (command[0], command[2])):
        raise Failure("Service executable/configuration paths contain unsupported characters.")
    binary, config = map(Path, (command[0], command[2]))
    no_symlink(binary)
    no_symlink(config)
    is_baseline = str(binary) in ("/usr/local/bin/uxplay", "/usr/local/bin/uxplay-uhf-test")
    is_release = binary.parent.parent == RELEASES and binary.name == "uxplay"
    if is_release:
        identifier(binary.parent.name)
    if not is_baseline and not is_release:
        raise Failure("Unrecognized UxPlay service executable: " + str(binary))
    if not binary.is_file() or not config.is_file():
        raise Failure("The service executable or configuration does not exist.")
    values["command"] = command
    return values


def override_text(release_id):
    release = release_path(release_id)
    if not SIMPLE_PATH.fullmatch(str(release)):
        raise Failure("The development path contains unsupported systemd characters.")
    return (HEADER + "[Service]\nExecStart=\nExecStart=" + str(release / "uxplay") +
            " -rc " + str(release / "config.conf") + "\n")


def existing_override():
    no_symlink(DROPIN)
    if not DROPIN.exists():
        return None, None
    content = DROPIN.read_text(encoding="utf-8")
    for directory in RELEASES.iterdir():
        if directory.is_dir() and IDENTIFIER.fullmatch(directory.name):
            if content == override_text(directory.name):
                return content, directory.name
    raise Failure("The existing 90-uxplay-dev.conf is not a recognized pi-dev override; it was left untouched.")


def authenticate():
    print("Authorizing the service switch.", flush=True)
    interactive = sys.stdin.isatty()
    run(["sudo", "-v"] if interactive else ["sudo", "-n", "-v"],
        capture=False, interactive=True)


def sudo(*args):
    # Keep the same controlling terminal as sudo -v so its timestamp applies.
    return run(["sudo", "-n", *map(str, args)], interactive=True)


def put_override(content):
    no_symlink(DROPIN)
    if content is None:
        # Only our caller-validated dedicated override is removed.
        sudo("rm", "-f", "--", DROPIN)
        return
    no_symlink(DROPIN.parent)
    sudo("mkdir", "-p", "--", DROPIN.parent)
    temporary = STATE / ("override-" + str(os.getpid()) + ".conf")
    remote_temporary = DROPIN.with_name(".90-uxplay-dev-" + str(os.getpid()) + ".tmp")
    no_symlink(remote_temporary)
    if remote_temporary.exists():
        raise Failure("Unexpected temporary service override exists: " + str(remote_temporary))
    write_text(temporary, content)
    try:
        sudo("install", "-m", "0644", "--", temporary, remote_temporary)
        sudo("mv", "-T", "--", remote_temporary, DROPIN)
    finally:
        temporary.unlink(missing_ok=True)


def restart():
    sudo("systemctl", "daemon-reload")
    sudo("systemctl", "restart", SERVICE)


def healthy(command, seconds=5):
    first = None
    deadline = time.monotonic() + seconds
    while True:
        state = service()
        pid = state.get("MainPID", "0")
        observed = (pid, state.get("NRestarts"))
        if state.get("ActiveState") != "active" or state.get("SubState") != "running" or pid == "0":
            raise Failure("Receiver did not remain active: " + state.get("ActiveState", "unknown"))
        if state["command"] != command:
            raise Failure("Another service override changed the effective command.")
        try:
            executable = os.readlink("/proc/" + pid + "/exe")
        except OSError as error:
            raise Failure("Cannot verify the running receiver executable: " + str(error))
        if executable != command[0]:
            raise Failure("The running receiver is not the expected binary: " + executable)
        if first is not None and observed != first:
            raise Failure("Receiver restarted during its health check.")
        first = observed
        if time.monotonic() >= deadline:
            return
        time.sleep(1)


def switch(content, expected, old_content, old_command, new_state):
    """Restore the exact prior override if any activation step fails."""
    changed = False
    try:
        changed = True
        put_override(content)
        restart()
        healthy(expected)
        write_json(STATE / "deployment.json", new_state)
    except BaseException as error:
        if changed:
            print("Switch failed; restoring the previous receiver configuration.", file=sys.stderr, flush=True)
            try:
                put_override(old_content)
                restart()
                healthy(old_command)
                print("Previous receiver restored and process health verified.", file=sys.stderr)
            except BaseException as recovery:
                raise Failure("Switch failed: " + str(error) + "\nAUTOMATIC ROLLBACK ALSO FAILED: " + str(recovery)) from error
        raise


def activate(token, release_id):
    require_lock(token)
    destination = release_path(release_id)
    metadata = read_json(destination / "build.json")
    binary = no_symlink(destination / "uxplay")
    if not metadata or not binary.is_file():
        raise Failure("This release has not been built successfully.")
    if hashlib.sha256(binary.read_bytes()).hexdigest() != metadata.get("binary_sha256"):
        raise Failure("Release executable checksum no longer matches its build.")
    current = service()
    old_content, old_id = existing_override()
    if old_id is None and Path(current["command"][0]).parent.parent == RELEASES:
        raise Failure("A development executable is selected outside the managed override; integrate that configuration manually.")
    if old_id and current["command"] != [str(release_path(old_id) / "uxplay"), "-rc", str(release_path(old_id) / "config.conf")]:
        raise Failure("Another service override supersedes the development override.")
    if old_id == release_id:
        healthy(current["command"])
        print("Release " + release_id + " is already active.")
        return
    baseline_path = STATE / "baseline.json"
    baseline = read_json(baseline_path)
    if baseline is None:
        if old_id:
            raise Failure("The existing development override has no saved baseline.")
        baseline = {"command": current["command"]}
        write_json(baseline_path, baseline)
    configuration = no_symlink(destination / "config.conf")
    if not configuration.exists():
        shutil.copy2(current["command"][2], configuration)
        configuration.chmod(0o600)
    expected = [str(binary), "-rc", str(configuration)]
    authenticate()
    print("Restarting the receiver with " + release_id + ".", flush=True)
    switch(override_text(release_id), expected, old_content, current["command"],
           {"active": release_id, "previous": old_id})
    print("Release " + release_id + " is active; process health passed. Verify AirPlay picture and sound on the iPhone.")


def stable(token):
    require_lock(token)
    current = service()
    old_content, old_id = existing_override()
    if old_content is None:
        print("The original service configuration is already selected.")
        return
    baseline = read_json(STATE / "baseline.json")
    if not baseline or not isinstance(baseline.get("command"), list):
        raise Failure("No saved baseline exists; refusing to guess its command.")
    authenticate()
    switch(None, baseline["command"], old_content, current["command"],
           {"active": None, "previous": old_id})
    print("Original service configuration restored; process health passed.")


def rollback(token):
    require_lock(token)
    deployment = read_json(STATE / "deployment.json", {})
    previous = deployment.get("previous")
    if previous is None:
        stable(token)
    else:
        activate(token, identifier(previous))


def check():
    print("Machine: " + os.uname().machine + "; kernel: " + os.uname().release)
    print("Development directory: " + str(ROOT))
    if ROOT.exists():
        managed()
        deployment = read_json(STATE / "deployment.json", {})
        print("Recorded deployment: " + json.dumps(deployment, sort_keys=True))
    for executable in ("rsync", "cmake", "c++", "pkg-config", "ccache"):
        print(executable + ": " + (shutil.which(executable) or "not installed"))
    current = service()
    print("Service: " + current.get("ActiveState", "unknown") + "/" + current.get("SubState", "unknown"))
    print("Command: " + shlex.join(current["command"]))
    print("Overrides: " + current.get("DropInPaths", ""))
    print("Free disk: " + str(shutil.disk_usage(Path.home()).free // (1024 * 1024)) + " MiB")


def logs():
    authenticate()
    result = sudo("journalctl", "-u", SERVICE, "-b", "-n", "80", "--no-pager")
    print(result.stdout, end="")


def operate(function, *args):
    """Record both the helper and its children; a live orphan keeps the lock."""
    global OPERATION
    require_lock(args[0])
    record = read_json(LOCK / "operation.json")
    if operation_busy(record):
        raise Failure("Another remote operation or compiler process is still running.")
    OPERATION = {"pid": os.getpid(), "operation": function.__name__,
                 "child_pid": None, "child_group": None}
    write_json(LOCK / "operation.json", OPERATION)
    try:
        function(*args)
    finally:
        child_alive = process_alive(OPERATION.get("child_pid")) or process_alive(OPERATION.get("child_group"), group=True)
        if not child_alive:
            (LOCK / "operation.json").unlink(missing_ok=True)
        OPERATION = None


def main(argv):
    if os.geteuid() == 0:
        raise Failure("Run this helper as the existing UxPlay user, never as root.")
    def interrupted(signum, _frame):
        raise Failure("Interrupted by signal " + str(signum))
    for name in ("SIGHUP", "SIGTERM"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), interrupted)
    commands = {"prepare": (prepare, 1), "finish": (finish, 1), "check": (check, 0),
                "build": (build, 2), "activate": (activate, 2), "rollback": (rollback, 1),
                "stable": (stable, 1), "logs": (logs, 0)}
    if not argv or argv[0] not in commands or len(argv) - 1 != commands[argv[0]][1]:
        raise Failure("Usage: pi_remote.py prepare|finish TOKEN; check; build|activate TOKEN RELEASE_ID; rollback|stable TOKEN; logs")
    function = commands[argv[0]][0]
    if argv[0] in ("build", "activate", "rollback", "stable"):
        operate(function, *argv[1:])
    else:
        function(*argv[1:])


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (Failure, OSError, ValueError) as error:
        print("pi-dev: " + str(error), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("pi-dev: interrupted", file=sys.stderr)
        sys.exit(130)
