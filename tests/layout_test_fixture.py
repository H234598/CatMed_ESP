"""Offline support for the public firmware-layout regression."""
import builtins
from contextlib import ExitStack
import importlib.abc
import importlib.util
import io
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


def load_public(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BlockLiveImports(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname.split('.')[0] in {'serial', 'esptool'}:
            raise AssertionError('NO_IO: live module import')
        return None


class BuildIdContractNoIO(unittest.TestCase):
    def forbidden(self, *args, **kwargs):
        self.blocked.append((args, kwargs))
        raise AssertionError('NO_IO: operation outside synthetic fixture')

    def setUp(self):
        self.stack = ExitStack()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.addCleanup(self.stack.close)
        self.work = Path(self.temp.name).resolve()
        self.cfg = self.work / 'esptool.cfg'
        self.cfg.write_text('')
        (self.work / 'outside-link').symlink_to(self.work.parent / 'outside-synthetic')
        (self.work / 'inside-link').symlink_to(self.cfg)
        self.stack.enter_context(mock.patch.dict(os.environ, {'ESPTOOL_CFGFILE': str(self.cfg)}))
        # Load the pure parser before filesystem restrictions, without opening devices.
        import serial  # noqa: F401
        import esptool.bin_image  # noqa: F401
        import esptool.cmds  # noqa: F401
        self.allowed_files = set()
        self.blocked = []
        self.install_guards()

    def safe_path(self, path):
        if isinstance(path, int):
            return self.forbidden(path)
        try:
            raw = Path(os.fsdecode(path))
            if ".." in raw.parts:
                return self.forbidden(path)
            path = Path(os.path.abspath(raw))
        except (TypeError, ValueError):
            return self.forbidden(path)
        if path != self.work and self.work not in path.parents:
            if path not in self.allowed_files or stat.S_ISLNK(self.original_lstat(path).st_mode):
                return self.forbidden(path)
            return path
        current = self.work
        for part in path.relative_to(self.work).parts:
            current = current / part
            try:
                info = self.original_lstat(current)
            except FileNotFoundError:
                break
            if stat.S_ISLNK(info.st_mode):
                return self.forbidden(path)
        return path

    def in_work(self, path):
        path = self.safe_path(path)
        return path == self.work or self.work in path.parents

    def checked(self, original, *, open_kind=None):
        def call(path, *args, **kwargs):
            self.safe_path(path)
            if kwargs.get("dir_fd") is not None:
                return self.forbidden(path)
            if open_kind == "file":
                mode = args[0] if args else kwargs.get("mode", "r")
                if any(flag in mode for flag in "wax+") and not self.in_work(path):
                    return self.forbidden(path)
            if open_kind == "os":
                flags = args[0] if args else kwargs.get("flags", 0)
                if flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC) and not self.in_work(path):
                    return self.forbidden(path)
            return original(path, *args, **kwargs)
        return call

    def checked_mutation(self, original):
        def call(path, *args, **kwargs):
            if not self.in_work(path) or any(key.endswith("dir_fd") and value is not None
                                              for key, value in kwargs.items()):
                return self.forbidden(path)
            return original(path, *args, **kwargs)
        return call

    def install_guards(self):
        self.original_lstat = os.lstat
        for obj, name, kind in ((builtins, "open", "file"), (io, "open", "file"),
                                (os, "stat", None), (os, "lstat", None),
                                (os, "scandir", None), (os, "listdir", None), (os, "readlink", None),
                                (os, "open", "os")):
            self.stack.enter_context(mock.patch.object(obj, name, self.checked(getattr(obj, name), open_kind=kind)))
        for name in ("mkdir", "rmdir", "unlink", "remove", "chmod", "chown", "truncate", "utime"):
            self.stack.enter_context(mock.patch.object(os, name, self.checked_mutation(getattr(os, name))))
        # These operations have multiple paths or can escape the test's process boundary.
        for name in ("rename", "replace", "link", "symlink", "chdir", "fork", "execv", "execve", "spawnv", "spawnve"):
            self.stack.enter_context(mock.patch.object(os, name, self.forbidden))
        for obj, name in ((socket, "socket"), (socket, "create_connection"), (socket, "getaddrinfo"),
                          (subprocess, "Popen"), (os, "system"), (os, "popen")):
            self.stack.enter_context(mock.patch.object(obj, name, self.forbidden))
        self.stack.enter_context(mock.patch.object(os.path, "realpath", lambda path, **kwargs: str(self.safe_path(path))))
        self.stack.enter_context(mock.patch.object(sys, "meta_path", [BlockLiveImports(), *sys.meta_path]))
