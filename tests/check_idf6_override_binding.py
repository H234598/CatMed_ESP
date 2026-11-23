#!/usr/bin/env python3
"""Check the override build's reviewed inputs, including rejected mutations."""
import importlib.util
import os
from pathlib import Path
import shutil
# The test invokes Git and Ninja directly with fixed argument vectors.
import subprocess  # nosec B404
import sys
import tempfile
from unittest.mock import patch

def require(condition: bool, message: str = "Override binding expectations failed") -> None:
    if not condition:
        raise AssertionError(message)


def git_executable() -> str:
    executable = shutil.which("git")
    if executable is None:
        raise RuntimeError("Git is required for the override binding checks")
    return os.path.abspath(executable)


ESP32C3 = "esp32c3"
ESP32C6 = "esp32c6"


repo = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo / "tools"))
spec = importlib.util.spec_from_file_location("bridge_compat", repo / "tools/prepare_iot_bridge_compat.py")
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)
build = Path(os.environ["KATMED_IDF_BUILD"]).resolve()
selected_target = os.environ["KATMED_IDF_TARGET"]
require(selected_target in (ESP32C3, ESP32C6))
root = build / "katmed-overrides"
idf = Path(os.environ["IDF_PATH"])
original_read = Path.read_bytes

def lock_for(target):
    return root.parent / f"override-{target.removeprefix('esp32')}.lock"



def rejects(path: Path, replacement: bytes, target: str = selected_target) -> None:
    def changed_read(item):
        return replacement if item == path else original_read(item)
    with patch.object(Path, "read_bytes", changed_read):
        try:
            compat.verify_override_end_state(root, idf, target, lock_for(target))
        except ValueError:
            return
    raise AssertionError(f"Modified input accepted: {path}")


for target in (selected_target,):
    compat.verify_override_end_state(root, idf, target, lock_for(target))
    lock = repo / "firmware" / f"dependencies.{target}.lock"
    rejects(lock, original_read(lock) + b" ", target)
    print(target, "valid and changed original lock rejected")

for name in compat.OVERRIDE_TREE_HASHES:
    manifest = root / name / "idf_component.yml"
    rejects(manifest, original_read(manifest) + b" ")
    print(name, "changed complete tree rejected")

lwip = idf / "components/lwip/lwip"
# The argv selects a fixed SDK object from the fixture checkout.
source = lwip / "src/core/ipv4/ip4_napt.c"
old = subprocess.check_output([git_executable(), "-C", str(lwip), "show", "HEAD:src/core/ipv4/ip4_napt.c"])  # nosec B603
rejects(source, old)
print("unpatched lwIP state rejected")

with patch.object(compat, "_inventory_hash", wraps=compat._inventory_hash) as inventory:
    try:
        compat.verify_override_end_state(root, idf, "esp32s3", lock_for("esp32s3"))
    except ValueError:
        pass
    else:
        raise AssertionError("Unsupported target accepted")
    require(not inventory.called)
print("unsupported target rejected before inventory")

original_command = subprocess.check_output

def changed_diff(args, *a, **kw):
    result = original_command(args, *a, **kw)
    if args[-3:] == ["diff", "--binary", "--abbrev=7"]:
        return result + b"unexpected"
    return result

with patch.object(compat.subprocess, "check_output", changed_diff):
    try:
        compat.verify_override_end_state(root, idf, selected_target, lock_for(selected_target))
    except ValueError as error:
        require("diff" in str(error))
    else:
        raise AssertionError("Unexpected lwIP diff accepted")
print("unexpected lwIP diff rejected")

with tempfile.TemporaryDirectory(prefix="override-inventory-") as tmp:
    tree = Path(tmp)
    (tree / "a").write_bytes(b"a")
    before = compat._inventory_hash(tree)
    (tree / "b").write_bytes(b"b")
    require(compat._inventory_hash(tree) != before)
    (tree / "b").unlink()
    (tree / "a").unlink()
    require(compat._inventory_hash(tree) != before)
    (tree / "link").symlink_to("missing")
    try:
        compat._inventory_hash(tree)
    except ValueError:
        pass
    else:
        raise AssertionError("Symlink accepted")
print("extra, missing and symlink entries rejected")

# Exercise real aliases only on tiny task-local fixtures, never on registry locks.
with tempfile.TemporaryDirectory(prefix="override-lock-") as tmp:
    base = Path(tmp)
    original_c3 = base / "source-c3.lock"
    original_c6 = base / "source-c6.lock"
    original_c3.write_bytes(b"c3")
    original_c6.write_bytes(b"c6")
    task = base / "task"
    task.mkdir()
    candidate = task / "override-c3.lock"
    originals = (original_c3, original_c6)
    compat._validate_override_lock(candidate, ESP32C3, task, originals)
    candidate.write_bytes(b"own")
    compat._validate_override_lock(candidate, ESP32C3, task, originals)
    candidate.unlink()
    for original in originals:
        for kind in ("symlink", "hardlink"):
            if kind == "symlink":
                candidate.symlink_to(original)
            else:
                os.link(original, candidate)
            try:
                compat._validate_override_lock(candidate, ESP32C3, task, originals)
            except ValueError:
                pass
            else:
                raise AssertionError(f"{kind} to original lock accepted")
            candidate.unlink()
    for wrong in (original_c3, original_c6, task / "other.lock", base / "override-c3.lock"):
        try:
            compat._validate_override_lock(wrong, ESP32C3, task, originals)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Wrong lock path accepted: {wrong}")
    alias = base / "alias"
    alias.symlink_to(task, target_is_directory=True)
    try:
        compat._validate_override_lock(alias / "override-c3.lock", ESP32C3, alias, originals)
    except ValueError:
        pass
    else:
        raise AssertionError("Symlink lock parent accepted")
print("lock target, both originals, symlink, hardlink and parent alias cases PASS")

for target in (ESP32C3, ESP32C6):
    for original_target in (ESP32C3, ESP32C6):
        original = repo / "firmware" / f"dependencies.{original_target}.lock"
        with patch.object(compat, "_inventory_hash", wraps=compat._inventory_hash) as inventory:
            try:
                compat.verify_override_end_state(root, idf, target, original)
            except ValueError:
                pass
            else:
                raise AssertionError("Caller accepted original lock as override output")
            require(not inventory.called)
print("caller rejects both original lock paths for both targets before inventory")

idf_real = idf.resolve()
mbedtls = idf_real / "components/mbedtls/mbedtls"
original_status = compat._git_status
for directory, entry in ((idf_real, "M  sdk-staged"), (idf_real, "?? sdk-untracked"),
                         (lwip.resolve(), "M  src/core/ipv4/ip4_napt.c"),
                         (mbedtls, "?? unexpected-input")):
    def changed_status(path, directory=directory, entry=entry):
        result = original_status(path)
        return result | {entry} if path == directory else result
    with patch.object(compat, "_git_status", changed_status):
        try:
            compat._validate_sdk_worktree(idf)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Unexpected SDK status accepted: {entry}")
with patch.object(Path, "is_symlink", lambda path: False if path == mbedtls / "framework" else os.path.islink(path)):
    try:
        compat._validate_sdk_worktree(idf)
    except ValueError:
        pass
    else:
        raise AssertionError("Missing framework link accepted")
original_check_output = subprocess.check_output
def changed_submodule(args, *a, **kw):
    if "submodule" in args:
        return "+deadbeef unexpected-submodule\n"
    return original_check_output(args, *a, **kw)
with patch.object(compat.subprocess, "check_output", changed_submodule):
    try:
        compat._validate_sdk_worktree(idf)
    except ValueError:
        pass
    else:
        raise AssertionError("Changed submodule accepted")
print("staged/untracked SDK, framework and changed submodule rejected")

# Use the resolved build graph and a real pending Ninja consumer. No SDK files
# are modified: the SDK negative case injects an unexpected git status response.
compat.verify_override_graph(root, selected_target, lock_for(selected_target), build)
partition = build / 'partition_table/partition-table.bin'
backup = build / 'partition_table/partition-table.bin.binding-test-backup'
require(partition.is_file() and not backup.exists())
ninja = os.environ.get('KATMED_NINJA', 'ninja')
lock = lock_for(selected_target)
manifest = root / 'lvgl__lvgl/idf_component.yml'
lock_bytes, manifest_bytes = lock.read_bytes(), manifest.read_bytes()
original_partition = partition.read_bytes()

def consume(environment=None):
    # Ninja comes from the configured build tools; all switches are fixed.
    return subprocess.run([ninja, '-C', str(build), 'partition_table_bin'],  # nosec B603
                          env=environment, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)

try:
    partition.rename(backup)
    for subject in ('lock', 'override', 'sdk'):
        env = os.environ.copy()
        with tempfile.TemporaryDirectory(prefix='katmed-sdk-status-') as tmp:
            if subject == 'lock':
                lock.write_bytes(lock_bytes + b'\ntarget: invalid\n')
            elif subject == 'override':
                manifest.write_bytes(manifest_bytes + b'\n')
            else:
                shim = Path(tmp) / 'git'
                real_git = git_executable()
                shim.write_text('#!/bin/sh\ncase " $* " in *" status "*) printf "?? injected-sdk-change\\n"; exit 0;; esac\nexec "' + real_git + '" "$@"\n')
                shim.chmod(0o700)
                env['PATH'] = tmp + os.pathsep + env.get('PATH', '')
            result = consume(env)
            require(result.returncode != 0 and not partition.exists(), (subject, result.stdout[-2000:]))
            lock.write_bytes(lock_bytes)
            manifest.write_bytes(manifest_bytes)
            print(subject, 'rejected before pending partition consumer')
    result = consume()
    require(result.returncode == 0 and partition.is_file(), result.stdout[-2000:])
    require(partition.read_bytes() == original_partition)
    print('restored partition consumer passed with identical bytes')
finally:
    lock.write_bytes(lock_bytes)
    manifest.write_bytes(manifest_bytes)
    if backup.exists():
        if partition.exists():
            partition.unlink()
        backup.rename(partition)

# Kill only our child after one real locked fetch. An interrupted private
# staging tree must not prevent reuse of the same destination on retry.
def check_component_fetcher_rebinding() -> None:
    from idf_component_tools.sources.fetcher import ComponentFetcher

    with tempfile.TemporaryDirectory(prefix='katmed-fetch-recovery-') as tmp:
        scratch = Path(tmp)
        pending_root = scratch / 'katmed-overrides'
        pending_lock = scratch / ('override-' + selected_target.removeprefix('esp32') + '.lock')
        sys.stdout.flush()
        sys.stderr.flush()
        pid = os.fork()
        if pid == 0:
            download = ComponentFetcher.download
            count = [0]
            def interrupted(fetcher, *args, **kwargs):
                count[0] += 1
                if count[0] == 2:
                    os._exit(73)
                return download(fetcher, *args, **kwargs)
            with patch.object(ComponentFetcher, 'download', interrupted):
                compat.prepare_override_root(pending_root, idf, selected_target, pending_lock)
            os._exit(74)
        _, status = os.waitpid(pid, 0)
        assert os.waitstatus_to_exitcode(status) == 73 and not pending_root.exists()
        assert list(scratch.glob('.katmed-overrides-*'))
        compat.prepare_override_root(pending_root, idf, selected_target, pending_lock)
        with patch.object(ComponentFetcher, 'download', side_effect=AssertionError('unexpected fetch')):
            compat.prepare_override_root(pending_root, idf, selected_target, pending_lock)


check_component_fetcher_rebinding()
print('interrupted fetch recovered in the same build path')
