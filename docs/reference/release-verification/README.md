# Release verification fixtures

These files are test fixtures for `docs/reference/verifying-a-release.md`. They are signed with a test key
(`test-key.pub`, id `B1DC2FE8001CB6C7`) that is **not one of the pinned keys**. They exist only to show each check pass
and fail.

- They are not a release. An installer must never accept them, and a manifest consumer must refuse them: with the real
  pinned keys every set here is refused at step 2 as `not pinned`.
- The pinned keys never sign anything but real releases. No file here is signed by one.
- The manifests carry the real v5.5.1 file list, hashes and sizes so that step 8 can be run against the real files.

| Directory | What it is |
| --- | --- |
| `good/` | The example manifest and its hashed-mode signature by the test key |
| `legacy-ed/` | The same manifest, signed in the legacy `Ed` mode by the test key |
| `unpinned-key/` | The same manifest, signed by a different throwaway key |
| `same-version-other-bytes/` | A different manifest with the same `manifest_version`, signed by the test key |
