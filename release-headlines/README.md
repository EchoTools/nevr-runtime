# Release headlines

A PR that changes what players see adds `release-headlines/<issue-or-topic>.txt`: one line, one plain sentence a player would understand, about that change only. The release cut joins the files added since the previous tag, in file-name order, into the release body's player line, and the fixed "Fixes and internal changes." stands when it finds none. A file that exists at the previous tag is never read, so a sentence already released cannot come back, and a new change must use a new file name (a reused name is dropped, not refused). This is the news-fragment pattern of towncrier's `newsfragments/` and changesets' `.changeset/`. The cut writes one JSON line (`"event":"headlines"`) to its log naming the files it used and the line it built.

A file name starts with a letter or digit, continues with letters, digits, `.`, `_` or `-`, and ends in `.txt`; this README is the only other file the directory holds. A line is 1 to 200 characters (a full stop is added when it ends in none of `.`, `!`, `?`), and the joined line is at most 600.

A PR that adds a fragment also deletes every fragment already released, the ones present at the newest `vX.Y.Z` tag, listed by `git ls-tree -r --name-only <newest tag> -- release-headlines/`. A fragment left behind is harmless, because a path present at the previous tag is never read; the deletion is housekeeping, not a gate.
