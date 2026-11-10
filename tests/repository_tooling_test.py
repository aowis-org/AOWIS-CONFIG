#!/usr/bin/env python3
"""Black-box tests for AOWIS-CONFIG patch and source snapshot scripts."""

import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest
import zipfile


SOURCE_ROOT = Path(__file__).resolve().parents[1]


class RepositoryToolingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='aowis-config-tool-test-')
        self.addCleanup(self.temporary.cleanup)
        self.parent = Path(self.temporary.name)
        self.root = self.parent / 'AOWIS-CONFIG'
        (self.root / 'tools').mkdir(parents=True)
        for name in ('apply_patch.sh', 'zip_config_project.sh'):
            source = SOURCE_ROOT / 'tools' / name
            target = self.root / 'tools' / name
            shutil.copy2(source, target)
            target.chmod(0o755)
        (self.root / 'README.md').write_text('old\n', encoding='utf-8')

    def run_tool(self, name, *args, success=True):
        result = subprocess.run(
            ['bash', str(self.root / 'tools' / name), *[str(arg) for arg in args]],
            capture_output=True,
            text=True,
            check=False,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def make_patch(self, entries, filename='aowis-config-test.zip'):
        patch = self.parent / filename
        with zipfile.ZipFile(patch, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, value in entries.items():
                if isinstance(value, tuple):
                    content, permissions = value
                    info = zipfile.ZipInfo(name)
                    info.create_system = 3
                    info.external_attr = (stat.S_IFREG | permissions) << 16
                    archive.writestr(info, content)
                else:
                    archive.writestr(name, value)
        return patch

    def test_snapshot_excludes_build_git_and_junk(self):
        (self.root / '.git').mkdir()
        (self.root / '.git' / 'secret').write_text('git secret')
        (self.root / 'build-linux-tests').mkdir()
        (self.root / 'build-linux-tests' / 'object.o').write_text('object')
        (self.root / '__pycache__').mkdir()
        (self.root / '__pycache__' / 'cache.pyc').write_text('bytecode')
        (self.root / '.aowis-patch-delete.txt').write_text('README.md\n')
        (self.root / 'source.cpp').write_text('source\n')

        self.run_tool('zip_config_project.sh')
        snapshot = self.parent / 'config.zip'
        with zipfile.ZipFile(snapshot) as archive:
            names = set(archive.namelist())
            self.assertIn('AOWIS-CONFIG/source.cpp', names)
            self.assertIn('AOWIS-CONFIG/tools/apply_patch.sh', names)
            self.assertIn('AOWIS-CONFIG/README.md', names)
            self.assertFalse(any('/.git/' in entry for entry in names))
            self.assertFalse(any('/build-linux-tests/' in entry for entry in names))
            self.assertFalse(any('/__pycache__/' in entry for entry in names))
            self.assertFalse(any(entry.endswith('.aowis-patch-delete.txt') for entry in names))

    def test_apply_replacement_addition_deletion_and_refresh(self):
        (self.root / 'obsolete.cpp').write_text('delete me\n')
        old = self.root / 'README.md'
        os.utime(old, (1_600_000_000, 1_600_000_000))
        patch = self.make_patch({
            'AOWIS-CONFIG/README.md': 'new\n',
            'AOWIS-CONFIG/include/new.h': ('#pragma once\n', 0o644),
            'AOWIS-CONFIG/.aowis-patch-delete.txt': 'obsolete.cpp\n',
        })
        self.run_tool('apply_patch.sh', patch)
        self.assertEqual(old.read_text(), 'new\n')
        self.assertGreater(old.stat().st_mtime, 1_600_000_000)
        self.assertEqual((self.root / 'include/new.h').read_text(), '#pragma once\n')
        self.assertFalse((self.root / 'obsolete.cpp').exists())
        self.assertFalse((self.root / '.aowis-patch-delete.txt').exists())
        with zipfile.ZipFile(self.parent / 'config.zip') as archive:
            names = archive.namelist()
            self.assertIn('AOWIS-CONFIG/include/new.h', names)
            self.assertNotIn('AOWIS-CONFIG/obsolete.cpp', names)
            self.assertEqual(archive.read('AOWIS-CONFIG/README.md'), b'new\n')

    def test_deletion_only_patch(self):
        (self.root / 'obsolete.cpp').write_text('delete me\n')
        patch = self.make_patch({
            'AOWIS-CONFIG/.aowis-patch-delete.txt': 'obsolete.cpp\n',
        })
        self.run_tool('apply_patch.sh', patch)
        self.assertFalse((self.root / 'obsolete.cpp').exists())
        self.assertTrue((self.parent / 'config.zip').is_file())

    def test_executable_permissions_from_patch(self):
        patch = self.make_patch({'AOWIS-CONFIG/tools/example.sh': ('#!/bin/sh\n', 0o755)})
        self.run_tool('apply_patch.sh', patch)
        self.assertTrue((self.root / 'tools/example.sh').stat().st_mode & stat.S_IXUSR)

    def test_wrong_filename_and_root_are_rejected(self):
        wrong_name = self.make_patch({'AOWIS-CONFIG/README.md': 'wrong\n'}, 'aowis-network-test.zip')
        self.run_tool('apply_patch.sh', wrong_name, success=False)
        wrong_root = self.make_patch({'OTHER/README.md': 'wrong\n'})
        self.run_tool('apply_patch.sh', wrong_root, success=False)
        self.assertEqual((self.root / 'README.md').read_text(), 'old\n')
        self.assertFalse((self.parent / 'config.zip').exists())

    def test_traversal_rejected_before_any_writes(self):
        patch = self.make_patch({
            'AOWIS-CONFIG/README.md': 'replacement\n',
            'AOWIS-CONFIG/../outside.txt': 'escape',
        })
        self.run_tool('apply_patch.sh', patch, success=False)
        self.assertEqual((self.root / 'README.md').read_text(), 'old\n')
        self.assertFalse((self.parent / 'outside.txt').exists())

    def test_refuses_existing_symlink(self):
        outside = self.parent / 'outside'
        outside.mkdir()
        (self.root / 'linked').symlink_to(outside, target_is_directory=True)
        patch = self.make_patch({'AOWIS-CONFIG/linked/unwanted.txt': 'malicious'})
        self.run_tool('apply_patch.sh', patch, success=False)
        self.assertFalse((outside / 'unwanted.txt').exists())

    def test_refuses_zip_symlink(self):
        patch = self.parent / 'aowis-config-symlink.zip'
        with zipfile.ZipFile(patch, 'w') as archive:
            info = zipfile.ZipInfo('AOWIS-CONFIG/linked')
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(info, '../outside')
        self.run_tool('apply_patch.sh', patch, success=False)
        self.assertFalse((self.root / 'linked').exists())

    def test_deletion_rejects_symlink_and_conflicts(self):
        target = self.root / 'obsolete'
        target.write_text('original')
        patch = self.make_patch({
            'AOWIS-CONFIG/obsolete': 'replacement',
            'AOWIS-CONFIG/.aowis-patch-delete.txt': 'obsolete\n',
        })
        self.run_tool('apply_patch.sh', patch, success=False)
        self.assertEqual(target.read_text(), 'original')
        (self.root / 'linked').symlink_to(target)
        patch = self.make_patch({
            'AOWIS-CONFIG/README.md': 'replacement',
            'AOWIS-CONFIG/.aowis-patch-delete.txt': 'linked\n',
        })
        self.run_tool('apply_patch.sh', patch, success=False)
        self.assertEqual((self.root / 'README.md').read_text(), 'old\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
