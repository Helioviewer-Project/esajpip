# EJ backup before OS upgrade

The accepted decoder history is on `opj-evidence`. This backup branch also
contains the unaccepted prepared-key experiment and compact verification
artifacts under `backup-evidence/`.

Recovery refs:

- `refs/heads/opj-evidence`: `897fa12a044a2a241ddc0361aed988a1c0f7a5fa`
- `refs/tags/backup/pre-os-20261027/client-original`: `52dfaee4fd03663eefe5871d3b562feea776fb1c`
- `refs/tags/backup/pre-os-20261027/client-rewritten`: `0ff4cc6f1a371f736c1ccfce15fc541aef22af1e`
- `refs/tags/backup/pre-os-20261027/opj-original`: `00659ed8d5d7d1c6136adae947c2ed6edf952776`

Fetch the backup refs from origin to recover the histories. The rewritten
parent tag is the intended parent of the accepted OPJ history. The existing
upstream client branch is unchanged.

The prepared-key experiment is not part of the accepted branch. Its key work
reduction is verified, but decode timings are mixed. Refer to its report before
choosing to retain it.

This is an EJ backup. JHV working-tree changes, large datasets, binaries and
full local build/log directories are not included.
