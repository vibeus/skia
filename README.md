# Vibe's Skia Fork

Skia's milestone branches receive patches, making it difficult to merge across them.

As a result, here's how Vibe's branches are managed:

- `master` simply mirros upstream's `master`, it is used to submit patches to upstream.
- Vibe's private patches are directly applied to Vibe's private milestone branch.
- Every once a while when we upgrade skia, a new private milestone branch is created and all Vibe's private
  patches are cherry-picked into the new milestone branch.
