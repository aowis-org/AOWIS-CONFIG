# Operations, portability and secret safety

This document describes the **native POSIX implementation in v0.4.0**. It is
not a deployment guide for unfinished Windows/WASM storage backends.

## What is stored where?

A caller chooses an application/service **owner**. Normal native user roots
resolve to `${XDG_CONFIG_HOME}/aowis/<owner>` when `XDG_CONFIG_HOME` is
absolute, otherwise `${HOME}/.config/aowis/<owner>`; explicit owner roots are
also possible. For example:

```text
~/.config/aowis/gui/wireguard/
  .lock
  <uuidv7>.json
```

Each record contains a versioned envelope, opaque revision token, and a JSON
object payload. The WireGuard payload includes the **complete** `.conf` text,
including `PrivateKey`. No original source path is stored. The file is a
*configuration record*, **not** a runnable `wg-quick` `.conf` file; use
`WireGuardProfiles::exportText()` for an explicit export.

Owners and collections are independent. The library does **not** create a
world-writable `aowis/shared/` directory, automatically sync devices, or store
DVMDB project data in the configuration root.

## Backup and transfer

Copy the **entire owning directory** when the application is stopped or not
writing. Include its JSON records, not merely the human-readable names.
Example (review destination and access controls first):

```bash
# For a native GUI using the default XDG location:
mkdir -p /path/to/private-backup/aowis
cp -a "$HOME/.config/aowis/gui" /path/to/private-backup/aowis/
```

If you use a different `XDG_CONFIG_HOME` or an explicit service owner root,
copy **that** configured location instead. The example is not a general-purpose
backup script: confirm the right root, ensure the destination is private and
protect the transport. Restoring into an existing live owner directory can
race with writes or create revision/conflict surprises; stop the owner and
restore its complete tree as a unit. Reopen the store after restore and verify
that profile names and IDs match expectations.

**A copied configuration directory is a secret-bearing backup.** Encrypt it
when storing or transferring it outside a trusted local environment. Source
`0700`/`0600` permissions are access restrictions, not encryption at rest.
Ensure the destination filesystem and permissions are suitable before loading
secrets. The current store rejects insecure owner directories/files rather
than silently repairing them.

**Portability limit:** external paths, script hooks or other directives inside
an imported WireGuard file may not work on the destination. A network endpoint
may be unreachable. DVMDB repositories/working copies, maps, caches and logs
are *not* in this tree. Copying settings is not cloning the whole installation.

## Security model

| Guarantee / constraint | Current behavior |
| --- | --- |
| Stored owner and collection | Restrictive POSIX directory permissions (`0700`) |
| Profile, lock and temporary files | Restrictive POSIX file permissions (`0600`) |
| Unsafe filesystem paths | Symlinks/traversal and unexpected file types rejected |
| Normal writes | Temp file, `fsync`, atomic per-record publication, directory `fsync` |
| Multi-process edits | Advisory collection locking plus expected revision token |
| Private keys | Stored *inside* managed JSON; never meant for logging |
| At-rest encryption | **Not provided** |
| Same-user processes / privileged administrators | POSIX mode bits alone do **not** isolate secrets from them |
| True sudden power-loss durability | **Not established by existing tests** |
| Cross-record transaction | **Not provided** |

A future export or browser integration must not downgrade secret handling
without an explicit design decision. The library does not run imported hook
commands; AOWIS-NETWORK must enforce its own runtime directive policy.

## Failure and recovery guidance

| Symptom / `ErrorCode` | Interpretation and safe response |
| --- | --- |
| `invalid_owner_id`, `invalid_collection`, `invalid_root` | Caller supplied an invalid owner/collection/path; check identifiers and explicit roots |
| `missing_environment`, `unsupported_platform` | No usable platform-specific root/backend; choose a supported platform or explicit native root |
| `permission_denied`, `unsafe_path` | Ownership, mode, symlink (at or below the owner directory) or path-type rejection; inspect the path instead of bypassing the check |
| `lock_timeout` | Another process held the collection lock past the timeout (hung instance, stalled filesystem); nothing changed, retry later |
| `invalid_json`, `invalid_wireguard_profile` | Bad payload or blocking WireGuard validation; show diagnostics and let the user correct input |
| `unsupported_schema`, `corrupt_object` | Store contains unknown or invalid records. Listings report them in `unreadable` instead of failing; preserve a backup, then remove `corrupt_object` records with `eraseCorrupt`/`eraseUnreadable`. `unsupported_schema` records belong to a newer release and are never removed by these calls |
| `not_found`, `conflict` | Object was removed/changed or supplied revision is stale; re-list and re-read before retrying |
| `profile_in_use`, `reference_unavailable` | Delete blocked by service references or missing authoritative catalogue; repair/reload the caller's reference lookup |
| `io_failure`, `object_too_large` | Filesystem/sync or size limit; keep original content, report failure, re-read after uncertain publication |

A successful update returns a **new** revision token. If an I/O or sync error
occurs *after publication*, the write may already have taken effect. Re-read
the object and compare its contents before offering another save. Never assume
an error guarantees that nothing changed.

An interrupted pre-publication write may leave a `.tmp-<uuidv7>` file. The
store checks and removes recognized orphan temporary files on the next
`open()` or write; readers ignore them. This is **not** a migration system or a general file repair
utility. Avoid hand-editing or deleting object records while the application
is using the collection; the envelope and revisions are significant.

## Operational checklist

- [ ] Confirm the correct owner/root and keep each application's store separate.
- [ ] Protect both live files and backups; never publish exported private keys.
- [ ] Treat `conflict` as a reload/compare event, not a forced overwrite.
- [ ] Coordinate service-reference changes with WireGuard profile deletion.
- [ ] Do not treat `Running` as a proven peer handshake or reachability.
- [ ] Test copied roots from a *stopped* owner before considering a migration complete.

For implementation details and limits, see [Storage API](storage-api.md) and
[WireGuard profiles](wireguard-profiles.md).
