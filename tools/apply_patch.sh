#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s /path/to/aowis-config-description.zip\n' "${0##*/}" >&2
    exit 2
fi

# Never install a patch intended for another AOWIS repository.
patch_name="${1##*/}"
case "$patch_name" in
    aowis-config-?*.zip) ;;
    *)
        printf 'Error: expected aowis-config-*.zip, got: %s\n' "$patch_name" >&2
        exit 2
        ;;
esac

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_root="$(cd -- "$script_dir/.." && pwd -P)"
if [[ "${project_root##*/}" != 'AOWIS-CONFIG' ]]; then
    printf 'Error: expected checkout directory AOWIS-CONFIG, got: %s\n' "${project_root##*/}" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    printf 'Error: python3 is required to apply and validate patch ZIPs.\n' >&2
    exit 1
fi
if [[ ! -f "$1" ]]; then
    printf 'Error: patch file not found: %s\n' "$1" >&2
    exit 1
fi

# The ZIP must contain AOWIS-CONFIG/relative/path entries. Optional deletions
# are listed as relative paths in AOWIS-CONFIG/.aowis-patch-delete.txt.
# Validate and stage everything before touching the checkout. Replacements are
# atomic per file; a patch spanning several files is not a single transaction.
python3 - "$project_root" "$1" <<'PY'
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import time
import zipfile

root = Path(sys.argv[1])
archive_path = Path(sys.argv[2])
manifest = Path('.aowis-patch-delete.txt')


def invalid(message):
    raise ValueError(message)


def relative_path(name, is_directory=False):
    prefix = root.name + '/'
    if not name.startswith(prefix):
        invalid(f'Invalid patch path (must start with {prefix}): {name!r}')
    value = name[len(prefix):]
    if value == '' and is_directory:
        return None
    if (not value or '\\' in value or '\x00' in value or
            '\r' in value or '\n' in value):
        invalid(f'Invalid patch path: {name!r}')
    if is_directory:
        if not value.endswith('/'):
            invalid(f'Invalid directory path: {name!r}')
        value = value[:-1]
    elif value.endswith('/'):
        invalid(f'Invalid file path: {name!r}')
    parts = value.split('/')
    if any(part in ('', '.', '..', '.git') for part in parts):
        invalid(f'Unsafe patch path: {name!r}')
    return Path(*parts)


def check_destination(relative, for_file=False):
    current = root
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            invalid(f'Refusing symlink destination: {relative.as_posix()}')
    target = root / relative
    if for_file and target.is_dir():
        invalid(f'Expected file, found directory: {relative.as_posix()}')
    for parent in target.parents:
        if parent == root:
            break
        if parent.is_file():
            invalid(f'Parent is a file: {relative.as_posix()}')
    return target


try:
    with zipfile.ZipFile(archive_path) as archive:
        if archive.testzip() is not None:
            invalid('Patch ZIP failed integrity check')
        files = {}
        directories = set()
        for info in archive.infolist():
            relative = relative_path(info.filename, info.is_dir())
            if relative is None:
                continue
            mode = info.external_attr >> 16
            file_type = stat.S_IFMT(mode)
            expected = stat.S_IFDIR if info.is_dir() else stat.S_IFREG
            if file_type not in (0, expected):
                invalid(f'Unsupported ZIP entry type: {info.filename!r}')
            if info.is_dir():
                if relative in directories:
                    invalid(f'Duplicate directory: {info.filename!r}')
                directories.add(relative)
            else:
                if relative in files:
                    invalid(f'Duplicate file: {info.filename!r}')
                files[relative] = info

        if not files:
            invalid('Patch ZIP contains no files')
        for relative in files:
            if (relative in directories or
                    any(parent in files for parent in relative.parents)):
                invalid(f'Conflicting file paths: {relative.as_posix()}')
            if relative != manifest:
                check_destination(relative, for_file=True)
        for relative in directories:
            if (relative in files or
                    any(parent in files for parent in relative.parents)):
                invalid(f'Conflicting directory path: {relative.as_posix()}')
            check_destination(relative)

        deletions = []
        if manifest in files:
            data = archive.read(files[manifest]).decode('utf-8')
            for name in data.splitlines():
                if not name:
                    continue
                relative = relative_path(root.name + '/' + name)
                if relative == manifest or relative in files:
                    invalid(f'Deletion conflicts with replacement: {name!r}')
                if any(relative in entry.parents for entry in files):
                    invalid(f'Deletion conflicts with child entry: {name!r}')
                target = check_destination(relative, for_file=True)
                if target not in deletions:
                    deletions.append(target)

        if len(files) == 1 and manifest in files and not deletions:
            invalid('Patch ZIP contains no replacements or deletions')

        with tempfile.TemporaryDirectory(prefix='.aowis-config-patch-', dir=root.parent) as temporary:
            staging = Path(temporary)
            for relative, info in files.items():
                if relative == manifest:
                    continue
                destination = staging / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as source, destination.open('wb') as output:
                    shutil.copyfileobj(source, output, length=1024 * 1024)
                permissions = (info.external_attr >> 16) & 0o777
                if permissions:
                    destination.chmod(permissions)

            changed_directories = set()
            for relative in files:
                if relative == manifest:
                    continue
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                os.replace(staging / relative, destination)
                now = time.time_ns()
                os.utime(destination, ns=(now, now))
                changed_directories.update(
                    parent for parent in destination.parents
                    if parent == root or root in parent.parents
                )
            for destination in deletions:
                destination.unlink(missing_ok=True)
                changed_directories.update(
                    parent for parent in destination.parents
                    if parent == root or root in parent.parents
                )
            for directory in sorted(changed_directories, key=lambda item: len(item.parts), reverse=True):
                now = time.time_ns()
                os.utime(directory, ns=(now, now))

    print(f'Applied {archive_path} with current file and directory timestamps.')
except (OSError, ValueError, zipfile.BadZipFile, UnicodeError) as error:
    print(f'Error: {error}', file=sys.stderr)
    sys.exit(1)
PY

printf 'Refreshing config.zip...\n'
if ! bash "$project_root/tools/zip_config_project.sh"; then
    printf 'Error: patch applied, but refreshing config.zip failed.\n' >&2
    exit 1
fi
