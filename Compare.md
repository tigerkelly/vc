# vc vs git — Comparison

## Architecture

| Aspect | vc | git |
|---|---|---|
| Storage format | Each commit is a **zip snapshot** of all staged files | Delta-compressed object database (blobs, trees, commits) |
| Efficiency | Every commit stores full file copies — large repos get large fast | Stores only changes — very space-efficient |
| History navigation | Reconstruct any version by replaying zips newest-first | Direct object lookup — instant access to any commit |
| Index | Simple flat text file listing tracked files + checksums | Binary packed format with stat caching |

## Branching

| Aspect | vc | git |
|---|---|---|
| Branch storage | Each branch has its own `index` and `HEAD_COMMIT` pointer | Branches are just pointers into the shared object graph |
| Switching branches | Replays zip archives to restore files | Swaps the working tree by diffing object trees |
| Merge | Basic merge by zip overlay — no 3-way merge | Full 3-way merge with ancestor tracking, rebasing, cherry-pick |
| Detached HEAD | Not supported | Fully supported |

## Remote Operations

| Aspect | vc | git |
|---|---|---|
| Protocol | Custom TCP daemon (`vcd`) on port 9876 | git:// SSH, HTTPS — widely supported |
| Authentication | vcd username + SHA-256 password | SSH keys, OAuth tokens, PAT |
| Hosting | Self-hosted `vcd` only | GitHub, GitLab, Bitbucket, self-hosted |
| Partial clone | Not supported — always full `.vc/` tree | Shallow clones, sparse checkout, partial clone |
| Push/pull model | Pushes entire `.vc/` directory every time | Sends only missing objects (pack protocol) |

## Missing git Features

These exist in git but not in vc:

- **Rebase** — no linear history rewriting
- **Cherry-pick** — no single-commit transplant
- **Submodules / subtrees** — no nested repo support
- **Signed commits** — no GPG signing
- **Hooks** — no pre-commit, post-commit scripts
- **Blame** — no per-line authorship
- **Bisect** — no binary search through history
- **Stash** — vc has basic stash but no stash branching
- **Reflog** — no recovery from accidental resets
- **Worktrees** — no multiple working directories
- **LFS** — no large file storage
- **Tags with messages** — vc tags are just name pointers
- **Network transparency** — no HTTP/HTTPS transport

## What vc Does That git Doesn't

- **Archive command** — rotate old commits to a separate archive directory
- **Integrated vcd daemon** — no separate git server setup needed, single binary
- **GTK3 GUI (`vcg`)** — built-in desktop interface
- **Simpler mental model** — commits are just zip files you can open with any zip tool

## Summary

Git is a production version control system built for scale, collaboration,
and distributed workflows. vc is a lightweight personal tool — simpler to
understand, easier to self-host on a LAN, and the zip-based storage means
you can inspect or recover any commit with nothing but `unzip`. The tradeoff
is storage efficiency, merge sophistication, and ecosystem support.
