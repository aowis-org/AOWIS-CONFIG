# AOWIS-CONFIG

Shared C++ configuration infrastructure for AOWIS components (planned). The
repository currently provides only the source patch and source snapshot tools.

## Initial tooling bootstrap (once only)

This repository initially has no `apply_patch.sh`. Install the first tooling
patch directly from the parent directory:

```bash
cd ~/coding/aowis/AOWIS-CONFIG
unzip -o ~/Downloads/aowis-config-tooling-bootstrap.zip -d ..
./tools/zip_config_project.sh
```

All subsequent patches use `./tools/apply_patch.sh`.

## Snapshot this repository

```bash
cd ~/coding/aowis/AOWIS-CONFIG
./tools/zip_config_project.sh
```

This writes `../config.zip` (normally `~/coding/aowis/config.zip`). It contains
an `AOWIS-CONFIG/` root and source files, but excludes Git internals, build
outputs, caches, editor files, temporary files, and the patch deletion manifest.
A new snapshot replaces the previous one **only after** ZIP creation succeeds.

## Apply a source patch

```bash
cd ~/coding/aowis/AOWIS-CONFIG
./tools/apply_patch.sh ~/Downloads/aowis-config-example.zip
```

Patches must be named `aowis-config-*.zip` and contain entries under the
`AOWIS-CONFIG/` root. The script checks the entire ZIP before modifying the
working tree, rejects unsafe paths/symlinks, stages replacements and updates
file timestamps so incremental builds notice them. It then refreshes
`../config.zip` automatically. Patches can add or replace files.

To delete files, a patch may include `AOWIS-CONFIG/.aowis-patch-delete.txt`
with one relative file path per line. Deletions are validated and the manifest
itself is not installed. Do not use the manifest for directories.

File replacements are atomic individually; the entire multi-file patch is **not**
transactional. A failed snapshot refresh does not roll back applied changes.
The patch script intentionally does not run `git add` or `git commit`.

## Verify the repository tools

```bash
python3 tests/repository_tooling_test.py
```

This runs the scripts in disposable checkouts and tests ZIP contents,
replacement, deletion, unsafe archive rejection, and snapshot refresh.
Requires Bash, Python 3, `zip`, and `unzip` for inspection.
