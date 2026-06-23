# vc — Version Control System

A lightweight version control system for Linux/Unix, similar in spirit to Git.
Each commit is a timestamped zip snapshot of the staged files, making history
easy to browse, extract, and archive without special tooling.

See [Compare.md](Compare.md) for a detailed comparison with git.

---

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [Building](#building)
- [Installation](#installation)
- [vcd — Remote Repository Daemon](#vcd--remote-repository-daemon)
  - [How it Works](#how-it-works)
  - [Server Setup](#server-setup)
  - [User Management](#user-management)
  - [Repository Management](#repository-management)
  - [Client Configuration](#client-configuration)
  - [Running as a System Service](#running-as-a-system-service)
  - [Security Model](#security-model)
- [Quick Start](#quick-start)
- [Commands](#commands)
  - [auth](#auth)
  - [add](#add)
  - [archive](#archive)
  - [branch](#branch)
  - [checkout](#checkout)
  - [clone](#clone)
  - [commit](#commit)
  - [config](#config)
  - [delete](#delete)
  - [diff](#diff)
  - [help](#help)
  - [history](#history)
  - [ignore](#ignore)
  - [info](#info)
  - [init](#init)
  - [list](#list)
  - [log](#log)
  - [merge](#merge)
  - [move](#move)
  - [moverepo](#moverepo)
  - [repos](#repos)
  - [pull](#pull)
  - [push](#push)
  - [rename](#rename)
  - [revert](#revert)
  - [show](#show)
  - [stash](#stash)
  - [status](#status)
  - [tag](#tag)
  - [untrack](#untrack)
  - [version](#version)
- [Repository Layout](#repository-layout)
- [Configuration File](#configuration-file)
- [Ignore Patterns](#ignore-patterns)
- [Index File](#index-file)
- [Commit Archives](#commit-archives)
- [Testing](#testing)
- [vcg — Graphical Interface](#vcg--graphical-interface)
  - [vcg Requirements](#vcg-requirements)
  - [Building vcg](#building-vcg)
  - [vcg Features](#vcg-features)

---

## Features

- **Zip-snapshot commits** — every commit is a timestamped `.zip` file
  containing only the staged files, plus a `MANIFEST.txt` with author,
  date, file count, and commit message.
- **Incremental commits** — only staged files are in each commit zip.
  `vc checkout --all` reconstructs the full working tree by combining
  all archives, newest version of each file wins.
- **Staging area** — `vc add` stages files; `vc commit` packages only
  what was staged. Matches the Git add → commit workflow.
- **Branching** — per-branch indexes and `HEAD_COMMIT` pointers.
  `vc branch` creates and switches in one command.
- **Merging** — `vc merge` combines another branch's files into the
  current branch with configurable conflict resolution.
- **Stash** — `vc stash` saves uncommitted work so you can switch
  branches cleanly, then restore it with `vc stash pop`.
- **Tags** — named pointers to commits for marking releases.
- **Archive** — `vc archive` moves old commit zips to `.vc/archive/`
  to keep the active history tidy while preserving everything.
- **vcd daemon** — dedicated server providing its own user and
  password system, independent of OS accounts. All push, pull,
  and clone operations use vcd. SSH/SFTP are not supported.
- **Collision-safe commit names** — two commits in the same second
  get a `_2` suffix (`YYYYMMDD_HHMMSS_2.zip`).
- **Diff** — `vc diff` shows line-by-line changes; supports terminal
  output or the meld GUI diff tool.
- **Ignore patterns** — `.vcignore` with `fnmatch` glob syntax,
  managed interactively with `vc ignore`.
- **Exit codes** — every command returns a meaningful exit code for
  use in scripts.
- **Colour status output** — staged (green), modified (yellow),
  untracked (grey), missing (red).

---

## Requirements

| Library | Linux | macOS | Windows (MSYS2) |
|---------|-------|-------|-----------------|
| `libzip` | `libzip-dev` | `brew install libzip` | `mingw-w64-x86_64-libzip` |
| `libreadline` | `libreadline-dev` | `brew install readline` | `mingw-w64-x86_64-readline` |
| `libcurses` | `libncurses-dev` | built-in | built-in |
| `libcrypt` | built-in | via Homebrew `libxcrypt` | built-in |
| GTK3 (vcg only) | `libgtk-3-dev` | `brew install gtk+3` | `mingw-w64-x86_64-gtk3` |

---

## Building

### Linux

```bash
# 1. Install dependencies
sudo bash setup.sh
# or
sudo make install-deps

# 2. Build (vc + vcd + vcg)
make

# 3. Install
sudo cp vc /usr/local/bin/
sudo cp vcd /usr/local/bin/
```

**Supported package managers:** `apt` (Debian/Ubuntu/Raspberry Pi OS),
`dnf`/`yum` (Fedora/RHEL), `pacman` (Arch).

### macOS

Requires [Homebrew](https://brew.sh).

```bash
# 1. Install dependencies
make install-deps
# which runs:
#   brew install libzip readline gtk+3 pkg-config

# 2. Build (vc + vcg — vcd is Linux-only)
make

# 3. Install
sudo cp vc /usr/local/bin/
```

> **Note:** vcd runs Linux-only. To use `vc push`/`vc pull`/`vc clone`
> from macOS, point to a Linux server running `vcd`.

### Windows (MSYS2/MinGW)

1. Install [MSYS2](https://www.msys2.org) and open the
   **MSYS2 MinGW 64-bit** terminal.

2. Install dependencies:

```bash
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-libzip
pacman -S mingw-w64-x86_64-readline
pacman -S mingw-w64-x86_64-gtk3
pacman -S mingw-w64-x86_64-pkg-config
pacman -S make
```

Or use the bundled helper:

```bash
make install-deps
```

3. Build:

```bash
make
```

This produces `vc.exe` and `vcg.exe`. The `vcd` daemon is not built
on Windows — push/pull/clone require a Linux server running `vcd`.

4. Add to PATH:

```bash
cp vc.exe ~/bin/
cp vcg/vcg.exe ~/bin/
```

Or add the build directory to your Windows `PATH` via System Properties.

### Key source files

| File | Purpose |
|---|---|
| `vc.c` / `vc.h` | Main entry point and command dispatch |
| `vcVersion.h` | Shared version number for vc, vcd, and vcg |
| `vcPlatform.h` | Cross-platform shims (Windows/macOS/Linux) |
| `vcSha256.c` / `vcSha256.h` | Pure C SHA-256 — no external dependency |
| `vcTransport.c` / `vcTransport.h` | TCP client transport (`vct_*` API) |
| `vcAuth.c` / `vcAuth.h` | Encrypted local password cache (`.vc/auth`) |
| `vcProgress.c` / `vcProgress.h` | Terminal progress bar |
| `vcd.c` / `vcd.h` | vcd daemon (Linux only) |
| `vcg/` | GTK3 GUI source directory |

### Run tests

```bash
make test
```

To run push/pull/clone tests against a live vcd server:

```bash
export VCD_TEST_HOST=192.168.0.170
export VCD_TEST_USER=kelly
export VCD_TEST_PASS=yourpassword
export VCD_TEST_REPO=users/kelly/ushell
make test
```

### Clean

```bash
make clean
```

---

## Installation

```bash
# Linux / macOS
sudo cp vc /usr/local/bin/
sudo cp vcd /usr/local/bin/     # Linux only
make vcg-install               # installs vcg to /usr/local/bin

# Windows (MSYS2)
cp vc.exe ~/bin/
cp vcg/vcg.exe ~/bin/
```

---

## vcd — Remote Repository Daemon

`vcd` is an optional server program that runs on the remote host and
handles all `vc push`, `vc pull`, and `vc clone` requests. It provides
its own user and password system completely independent of OS accounts,
and enforces private repo access at the protocol level.

### How it Works

`vcd` is a dedicated daemon listening on TCP port 9876. Clients
authenticate with a `vcd` username and password (not their OS login).
The daemon enforces who may push to or pull from each repository.
No SSH access or OS account is needed on the server.

```
Client machine              Remote server

vc push/pull  →  TCP:9876 → vcd → /var/lib/vcd/users/<username>/<reponame>/

   Auth: vcd username + SHA-256(password)
```

Passwords are hashed with SHA-256 before leaving the client and stored
with `crypt()` on the server. Plaintext passwords are never transmitted.

---

### Server Setup

#### 1. Build vcd

```bash
make vcd
# or directly:
gcc -o vcd vcd.c -lcrypt
```

#### 2. Initialise

Creates the repo root directory and places `vcd.conf` and `users.db`
alongside it (so the daemon can read them after dropping privileges)
directory. The default repo root is `/var/lib/vcd` — override it
with `--reporoot`:

```bash
# Default location
sudo vcd --init

# Custom location (e.g. home directory or external drive)
sudo vcd --init --reporoot /home/kelly/repos
sudo vcd --init --reporoot /mnt/external/vcrepos
```

Output:

```
vcd initialised.
  Config   : /ssd/repo/vcd.conf
  User DB  : /ssd/repo/users.db
  Log      : /ssd/repo/vcd.log
  Repo root: /home/kelly/repos

Next steps:
  vcd --adduser <username>   # add a vc user
  vcd --start                # start the daemon
```

The chosen repo root is saved to `vcd.conf` (alongside the repoRoot) automatically,
so `--reporoot` only needs to be specified at `--init` time. To change
it later, edit `vcd.conf` or pass `--reporoot` again with
`--start`.

#### 3. Add users

```bash
sudo vcd --adduser kelly
# Repo directory under /ssd/repo/
#   Press Enter for 'users/kelly':
# New password  : ****
# Confirm       : ****
# User 'kelly' added.
# Repo dir : /ssd/repo/users/kelly

sudo vcd --adduser alice
```

#### 4. Start the daemon

```bash
sudo vcd --start
# vcd listening on port 9876

# Custom port:
sudo vcd --start --port 12000

# Custom repo root (if not set in vcd.conf):
sudo vcd --start --reporoot /home/kelly/repos

# All options together:
sudo vcd --start --port 9876 --reporoot /srv/vcrepos
```

---

### User Management

Users are stored in `users.db` (alongside repoRoot) with a username and
password hash. Access to individual repos is controlled in each repo's
own `config.vc` — not in the user database. This means the user
database stays small no matter how many repos exist.

#### List users

```bash
sudo vcd --listusers
```

```
Username              Repo directory
--------------------  -----------------------------
  kelly               /ssd/repo/users/kelly
  alice               /ssd/repo/users/alice
  bob                 /ssd/repo/users/bob
```

#### Change a password

```bash
sudo vcd --passwd kelly
```

#### Delete a user

```bash
sudo vcd --deleteuser kelly
```

Removes the user from `users.db`. Prompts `Type 'yes' to confirm`.
Repo files in `repoRoot/users/kelly/` are **not** deleted — remove
them manually if needed:

```bash
sudo vcd --deleteuser kelly
sudo rm -rf /sas/repos/users/kelly
```

#### User database format

Located at `<repoRoot>/users.db`. Plain text, one user per line:

```
# username:crypted_password:userdir
kelly:$6$salt$cryptedhash:users/kelly
alice:$6$salt$cryptedhash:users/alice
```

Three fields: username, `crypt()` hash, and user directory relative
to `repoRoot`. Never edit by hand — use `vcd --passwd` to change
passwords and `vcd --adduser` to add users.

---

### Repository Management

Repositories live under the `repoRoot` directory in one of two layouts:

```
repoRoot/
  users/
    kelly/          ← personal repos (only kelly can push/pull)
      ushell/
      myapp/
  shared/           ← shared repos (access via allowedUsers in config.vc)
    teamapp/
```

#### Create a repository on the server

```bash
# Personal repo:
sudo vcd --initrepo users/kelly/ushell

# Shared repo:
sudo vcd --initrepo shared/teamapp
```

Alternatively, the first `vc push` from a client will offer to create
the repo automatically if it does not exist.

#### Control who can access a shared repo

Access to shared repos is controlled by `allowedUsers` in the repo's
`config.vc` on the server:

```bash
cd /var/lib/vcd/shared/teamapp
vc config --set allowedUsers alice,bob,charlie
```

Personal repos (`users/<username>/...`) are always private to the
owning user — no configuration needed.

---

### Client Configuration

Set the remote connection details using `vc config`:

```bash
vc config --set host     192.168.0.170
vc config --set vcdUser  kelly
vc config --set repo     users/kelly/ushell
```

Then push and pull as normal:

```bash
vc push
vc pull
```

`vc` will prompt for your `vcd` password on the first use. The
password is hashed with SHA-256 before being sent — it never travels
in plaintext. To save the password so you are not prompted every time:

```bash
# Answer 'y' when prompted after push/pull, or:
vc auth           # show whether a password is cached
vc auth --clear   # remove the cached password
```

The cached password is stored encrypted in `.vc/auth` (owner-readable only).

---

### Running as a System Service

#### systemd (Linux)

Create `/etc/systemd/system/vcd.service`:

```ini
[Unit]
Description=vc repository daemon
After=network.target

[Service]
Type=simple
User=root
ExecStart=/usr/local/bin/vcd --start
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable vcd
sudo systemctl start vcd
sudo systemctl status vcd
```

#### Logs

```bash
tail -f /var/log/vcd.log
```

Log format:

```
[2025-05-14 14:23:01] Connect: 192.168.0.101
[2025-05-14 14:23:01] AUTH ok: user=kelly addr=192.168.0.101
[2025-05-14 14:23:02] PUSH ok: user=kelly repo=myapp file=.vc/data/20250514_142300.zip size=9823
[2025-05-14 14:23:02] Disconnect: 192.168.0.101
```

---

### Security Model

| | vcd |
|---|---|
| Authentication | vcd username + password (not OS account) |
| Password transmission | SHA-256 hash only — plaintext never sent |
| Password storage (server) | `crypt(SHA-256(password))` — SHA-512 salt |
| Password storage (client) | XOR-encrypted in `.vc/auth` (chmod 600) |
| Filesystem access | None — vcd protocol only, no shell access |
| Access control | Personal repos: owner only. Shared repos: `allowedUsers` in config.vc |
| Private enforcement | Server enforces at protocol level |
| Transport | Plain TCP on port 9876 (LAN use) |
| users.db location | Alongside `repoRoot` (e.g. `/ssd/repo/users.db`) so daemon can read after privilege drop |

**Port:** `vcd` listens on TCP port 9876 by default. Open this port
in your firewall and close SSH port 22 if you want push/pull to use
only `vcd`:

```bash
# UFW (Ubuntu)
sudo ufw allow 9876/tcp
sudo ufw deny 22/tcp     # optional — only if SSH not needed for other things
```

**Config file:** `<repoRoot>/vcd.conf`

```
port     = 9876
repoRoot = /var/lib/vcd
userDb   = /ssd/repo/users.db
logFile  = /var/log/vcd.log
```

All config file values can be overridden on the command line:

| Flag | Config key | Default |
|------|-----------|---------|
| `--port <n>` | `port` | `9876` |
| `--reporoot <path>` | `repoRoot` | `/var/lib/vcd` |
| `--conf <file>` | — | `<repoRoot>/vcd.conf` |

---

## Quick Start

```bash
# Initialise a new repo (prompts for user, email, project, remote host)
vc init

# Stage files and make the first commit
vc add
vc status
vc commit --msg "Initial commit"

# Push to a remote vcd server
vc push
```

**Remote setup (on the server):**

```bash
sudo vcd --init --reporoot /ssd/repo
sudo vcd --adduser kelly
sudo vcd --start
```

**Clone an existing repo:**

```bash
vc clone --user kelly 192.168.0.170 users/kelly/myapp
cd myapp
vc status
```

---

## Commands

Commands are listed in alphabetical order.

---

### auth

Show or clear the locally cached vcd password.

```
vc auth              Show whether a vcd password is cached in .vc/auth
vc auth --clear      Remove the cached password
```

The password is cached automatically after the first successful push,
pull, or clone. It is stored encrypted in `.vc/auth` (permissions 0600,
owner-readable only) and is never stored in plaintext.

---

### add

Stage files for the next commit.

```
vc add [--binary=stage|ignore] [<file|directory> ...]
```

With no arguments, stages all files under the project root (respecting
`.vcignore`). With arguments, stages only the listed files or directories.
Empty directories are staged and preserved in commit archives.

**Binary file handling:**

When a binary file is encountered, `vc add` prompts whether to add it
to `.vcignore` or stage it anyway:

```
  Warning: 'myapp' is a binary file.
  Add to .vcignore?
    1) This file only  (myapp)
    2) All .bin files  (*.bin)
    n) No, stage it (don't ask again)
  Choice [1/2/n]:
```

Choosing `n` saves the answer to `.vc/binary_ok` so the same file is
not prompted again. Text files without extensions (e.g. `Makefile`,
`README`) are never flagged — only files with binary content.

The `--binary` flag skips the prompt entirely:

| Flag | Behaviour |
|---|---|
| `--binary=stage` | Silently stage all binary files |
| `--binary=ignore` | Silently add all binary files to `.vcignore` |

---

### archive

Move old commit archives to `.vc/archive/` to keep the active history tidy.

```
vc archive                   Archive all but the most recent commit
vc archive --all             Archive every commit
vc archive --keep <n>        Keep n most recent commits, archive the rest
vc archive --list            Show archived commits (same format as 'vc list')
vc archive --restore <zip>   Move a zip back to active commits
```

Archived commits are preserved and can be restored at any time.
`vc checkout --zip` and `vc checkout --tag` still work with archived zips.

**Example:**

```bash
vc archive --keep 5               # keep 5 most recent, archive the rest
vc archive --list                 # see what has been archived
vc archive --restore 20250513_142300.zip
```

---

### branch

List, create, or switch branches.

```
vc branch                   List all branches (* marks active)
vc branch <name>            Switch to existing branch, or create and switch
vc branch --delete <name>   Delete a branch
```

**Rules:**
- You must be in the **project root** to switch branches.
- Branch names may not start with `-` or contain spaces or `/`.
- You cannot delete the currently active branch.
- If there are staged files when switching, you are warned and can
  choose to proceed. The staged files remain on the outgoing branch.

---

### checkout

Restore files from commit archives without switching branches.

```
vc checkout <file>             Restore one file from its most recent commit
vc checkout --all              Restore all tracked files to most recent state
vc checkout --tag <name>       Restore project to a tagged commit
vc checkout --zip <zipfile>    Restore project from a specific zip
```

`checkout --all` scans all zips newest-first so each file gets its
most recent committed version, regardless of which commit it came from.

**Conflict handling:**

Before overwriting a working file, `vc checkout` checks whether it has
been modified since the last `vc add`. If it has, a 3-way merge is
attempted automatically:

| Situation | Output |
|---|---|
| File unchanged locally | `restored src/main.c` |
| Changed in different places | `merged src/main.c` — staged automatically |
| Changed in same place | `conflict src/main.c` — conflict markers written |
| Binary / merge error | Local kept, remote saved as `src/main.c.remote` |

Resolve conflicts by editing the file, removing the markers, then
`vc add` and `vc commit`.

---

### clone

Clone a remote vc repository into a new local directory.

```
vc clone [--user <username>] <host> <repopath> [localdir]
```

Downloads the full `.vc/` directory from the remote vcd server,
then checks out the latest commit so working files are present.
The vcd password is saved to `.vc/auth` after a successful clone.

```bash
vc clone 192.168.0.170 users/kelly/ushell
vc clone --user kelly 192.168.0.170 users/kelly/ushell
vc clone 192.168.0.170 shared/teamapp /home/pi/teamapp
cd ushell
vc status
```

---

### commit

Package staged files into a timestamped zip archive.

```
vc commit [--msg "message"]
```

Archives are named `YYYYMMDD_HHMMSS.zip`. Two commits in the same
second get a `_2` suffix. Nothing staged → exits cleanly with no archive.

If `--msg` is omitted, the commit message is prompted interactively.
An empty or whitespace-only message is rejected and re-prompted:

```
Commit message:
  Commit message cannot be empty. Please enter a description.
Commit message: Fixed login bug
```

---

### config

View or update repository configuration.

```
vc config                        Interactive — prompts for each field
vc config --show                 Display current config
vc config --set <key> <value>    Set a field non-interactively
```

| Key | Description |
|-----|-------------|
| `user` | Commit author name |
| `email` | Author email |
| `project` | Project name |
| `host` | Remote hostname or IP for `vc push` / `vc pull` / `vc clone` |
| `vcdUser` | vcd daemon username for remote operations |
| `repo` | Remote repo path (e.g. `users/kelly/ushell`) |
| `port` | vcd port (default 9876) |
| `owner` | OS login of the user who ran `vc init` — set automatically |
| `private` | `true` or `false` — if true, only the owner may push or pull |

**Examples:**

```bash
vc config --show
vc config --set host 192.168.0.170
vc config --set vcdUser kelly
vc config --set repo users/kelly/ushell
vc config --set private true    # make repo private
vc config --set private false   # make repo public
```

---

### delete

Remove files from tracking and optionally from disk.

```
vc delete <file> [<file2> ...]          Delete from disk and index (prompts)
vc delete --keep <file> [<file2> ...]   Remove from index, keep on disk
vc delete --force <file> [<file2> ...]  Delete without prompting
```

---

### diff

Show differences between working copy and last committed version.

```
vc diff <file>             Terminal diff output (default)
vc diff --diff <file>      Terminal diff output (explicit)
vc diff --meld <file>      Open in the meld GUI diff tool
```

If meld is not installed, falls back to terminal diff with install instructions.

Install meld:
```bash
sudo apt install meld          # Debian/Ubuntu
sudo dnf install meld          # Fedora
brew install --cask meld       # macOS
```

---

### help

Print a summary of all commands.

```
vc help
```

---

### history

Show all commits that included a specific file.

```
vc history <file>              All commits containing this file
vc history <file> --count <n>  Most recent n commits only
```

Searches both active and archived commits. Archived commits are
marked with `*`.

**Example output:**

```
History for: src/main.c

  Archive                 Date                      Message
  ----------------------  ------------------------  -------
  20250514_153000.zip     2025-05-14 15:30:00 UTC   Fix null pointer
  20250513_142300.zip*    2025-05-13 14:23:00 UTC   Initial commit

  2 commit(s) found containing 'src/main.c'.
  (* = archived commit)
```

---

### ignore

Manage `.vcignore` patterns interactively.

```
vc ignore                    List current patterns
vc ignore <pattern>          Add a pattern
vc ignore --delete <pattern> Remove a pattern
vc ignore --check <file>     Test if a file would be ignored
```

Both `vc add` and `vc status` skip files matching any pattern.
Uses `fnmatch` glob syntax.

**Examples:**

```bash
vc ignore "*.o"              # ignore compiled objects
vc ignore "build/"           # ignore build directory
vc ignore --check main.o     # test if main.o would be ignored
vc ignore --delete "*.o"     # remove the pattern
vc ignore                    # list all patterns
```

---

### info

Display a repository summary.

```
vc info
```

Shows: project name, active branch, remote host and path, commit
counts (active and archived), branch count, tag count, repository
size on disk, last commit details, and working tree state.

**Example output:**

```
  Repository   : /home/kelly/myproject
  Project      : myapp
  Branch       : main

  Remote host  : 192.168.0.170
  Login        : kelly
  Remote path  : /home/kelly/myproject

  Commits      : 8 active, 3 archived
  Branches     : 2
  Tags         : 1
  Repo size    : 124.3 KB

  Last commit  : 20250514_153000.zip
  Date         : 2025-05-14 15:30:00 UTC
  Author       : Jane Doe
  Message      : Fix null pointer in parser

  Working tree : clean
```

---

### init

Initialise a new repository in the current directory.

```
vc init [--email EMAIL] [--project NAME]
        [--repoHost HOST] [--repoLogin USERNAME] [--repoPrivate]
```

Prompts interactively for any values not supplied as flags.

- **User name** is read automatically from the OS login (`/etc/passwd`
  full name field). It is displayed but not prompted.
- **Project name** defaults to the current directory name and is not
  prompted — override with `--project`.
- **Email** is the only required prompt.
- **Remote host**, vcd username, and password are optional — leave blank
  to skip and configure later with `vc config`.

**Examples:**

```bash
vc init                              # email prompted; all else auto
vc init --email jane@example.com     # no prompts at all
vc init --project myapp --email jane@example.com
```

---

### list

List all active commit archives with metadata.

```
vc list              All commits, newest first
vc list --oldest     Oldest first
vc list --count <n>  Most recent n commits only
```

| Column | Description |
|--------|-------------|
| Archive | Zip filename |
| Files | Number of files staged in that commit |
| Size | Zip file size on disk |
| Date | Commit timestamp |
| Message | Commit message |

Tagged commits are highlighted in yellow. Multiple tags on the same
commit are shown comma-separated. See `vc archive --list` for archived commits.

---

### log

Display the raw internal operation log (`.vc/vc.log`).

```
vc log
```

---

### merge

Merge another branch into the current branch.

```
vc merge <branch>            3-way merge (default)
vc merge <branch> --ours     On conflict, keep current branch version
vc merge <branch> --theirs   On conflict, take incoming branch version
```

`vc merge` uses a built-in **line-level 3-way merge** — no external
tools needed, works identically on Linux, macOS, and Windows.

For each file on the source branch:
- **Not in current branch** → copied in, staged for commit
- **Same on both branches** → no action
- **Changed in different places** → merged cleanly, staged automatically
- **Changed in the same place** → conflict markers written into the file

Conflict marker format (standard, compatible with most editors):

```
<<<<<<< main
    int timeout = 30;
||||||| base
    int timeout = 10;
=======
    int timeout = 60;
>>>>>>> feature
```

After resolving conflicts: edit the file, remove the markers,
then `vc add <file>` and `vc commit --msg "Merge feature"`.

The `--ours` and `--theirs` flags skip the 3-way merge and directly
choose a winner when no base version is available.

**Example:**

```bash
vc branch feature-login    # create and switch to feature branch
# ... do work, commit ...
vc branch main             # switch back to main
vc merge feature-login     # merge feature into main
vc commit --msg "Merge feature-login"
```

---

### move

Rename a tracked file or directory (alias for `vc rename`).

```
vc move <old> <new>
```

---

### moverepo

Move a repository between `users/` and `shared/` on the vcd server.
Also updates the local `config.vc` `repo` field automatically.

```
vc moverepo <srcpath> <dstpath>
```

**Examples:**

```bash
# Publish a personal repo as shared
vc moverepo users/kelly/ushell shared/ushell

# Move a shared repo back to personal
vc moverepo shared/ushell users/kelly/ushell
```

After a successful move the local `config.vc` is updated automatically
if it points to the source path:

```
repo = shared/ushell      ← updated from users/kelly/ushell
```

**Rules enforced by the server:**

- You must own the source repo (personal repos: must be in your own
  `users/<username>/` directory; shared repos: must be listed as owner
  in the repo's `config.vc`)
- Moving to `users/` is only allowed into your own directory
- The destination must not already exist
- Path traversal (`..`) is rejected

Requires a working vcd connection — prompts for your vcd password
(or uses the cached password from `.vc/auth`).

---

### repos

List all repositories on a vcd server that are visible to you.
Does not require a local `.vc/` directory — useful for browsing before cloning.

```
vc repos <host> <username> [--port <port>]
```

Prompts for your vcd password, then connects and lists:

- **mine** — your own personal repos (`users/<you>/...`)
- **shared** — shared repos you are listed in as an allowed user
- **public** — other users' repos where `private = false`

Other users' private repos are never shown.

**Examples:**

```bash
vc repos 192.168.0.170 kelly
vc repos 192.168.0.170 kelly --port 9877
```

**Sample output:**

```
Repositories on 192.168.0.170 visible to kelly:

  users/kelly/ushell                        mine
  users/kelly/myapp                         mine
  shared/teamapp                            shared
  users/alice/opentools                     public

4 repos found.
```

---

### pull

Pull commits, branches, and tags from the remote repository.

```
vc pull
```

Uses the vcd daemon. Downloads anything not already present locally.
Commit zips are skipped if already present at the same size.
Metadata files (index, config, HEAD, tags) are always refreshed.

**Prerequisite:** `host`, `vcdUser` must be set (`vc config`).
Start vcd on the remote with `sudo vcd --start`.

---

### push

Push commits, branches, and tags to the remote repository.

```
vc push
```

Uses the vcd daemon. If vcd is not running on the remote the push
fails with a clear error. Commit zips already on the remote at the
same size are skipped.

**Prerequisite:** `host`, `vcdUser` must be set (`vc config`).
Start vcd on the remote with `sudo vcd --start`.

---

### rename

Rename a tracked file or directory and update the index.

```
vc rename <old> <new>
```

Renaming a directory updates all index entries inside it. The renamed
entry is marked staged — commit to record the rename.

---

### revert

Restore a file to its last committed state (alias for `vc checkout <file>`).

```
vc revert <file>
```

---

### show

Show full details and file listing of a specific commit archive.

```
vc show                    Show the most recent commit
vc show <archive>          Show a specific zip by name
vc show --tag <name>       Show the commit a tag points to
```

Displays the MANIFEST.txt contents and a list of all files in the
archive with their uncompressed sizes. Works with both active and
archived commit zips.

**Example output:**

```
Commit : 20250513_142300.zip
Size   : 9.8 KB

vc commit manifest
------------------
archive : 20250513_142300.zip
project : myapp
author  : Jane Doe
date    : 2025-05-13 14:23:00 UTC
files   : 8
message : Initial commit

Files in this commit (8):

  src/main.c                                3.2 KB
  src/utils.c                               1.8 KB
  Makefile                                  0.9 KB
  README.md                                 4.1 KB
```

---

### stash

Temporarily save staged and modified files so you can switch branches
or do other work without committing.

```
vc stash                       Save staged/modified files, clean working tree
vc stash --msg "description"   Stash with a descriptive message
vc stash pop                   Restore most recent stash and remove it
vc stash pop <name>            Restore a named stash and remove it
vc stash apply                 Restore without removing the stash
vc stash list                  List all stashes
vc stash drop <name>           Delete a stash entry
```

Stashes are stored in `.vc/stash/<timestamp>/` as a zip of the
saved files plus a snapshot of the index.

**Example workflow:**

```bash
# Working on feature, not ready to commit
vc add src/wip.c
vc stash --msg "WIP feature login"

# Switch branches, do other work, come back
vc branch main
# ... fix a bug, commit ...
vc branch feature-login
vc stash pop             # restore WIP files
```

---

### status

Show the current state of the working tree.

```
vc status [--remote]
```

| Section | Colour | Meaning |
|---------|--------|---------|
| Staged for commit | Green | Will be in the next `vc commit` |
| Modified since last add | Yellow | Changed after `vc add` — re-add to capture |
| Untracked | Dark grey | Unknown to vc — run `vc add` |
| Missing | Red | Tracked but deleted on disk — run `vc delete` |

Without `--remote`, status is instant and fully offline. With `--remote`,
vc connects to the vcd server and compares commit archives:

```
Remote  : kelly@192.168.0.170:users/kelly/ushell
Private : no
Remote  : 2 commits ahead — run 'vc pull'
---
```

| Remote line | Meaning |
|---|---|
| `up to date` | Local and remote have the same commits |
| `N commits ahead — run 'vc pull'` | Remote has N commits not in local |
| `N commits behind — run 'vc push'` | Local has N commits not on remote |
| Both lines shown | Repos have diverged — pull then push |

`--remote` uses the cached `.vc/auth` password or prompts if not cached.

---

### tag

Create, list, inspect, or delete tags.

```
vc tag                      List all tags
vc tag <name>               Tag the current commit
vc tag <name> --msg "text"  Tag with a message
vc tag --show <name>        Show tag details
vc tag --delete <name>      Delete a tag
```

Tags are stored in `.vc/tags/` and visible across all branches.
Multiple tags may point to the same commit.

---

### untrack

Remove files from tracking without deleting them from disk.

```
vc untrack <file> [<file2> ...]
```

Alias for `vc delete --keep`. The file immediately appears as
`untracked` in `vc status`.

---

### version

Print the vc version number.

```
vc version
```

---

## Repository Layout

```
.vc/
├── config.vc              # repository configuration
├── HEAD                   # active branch name
├── vc.log                 # append-only operation log
├── branches/              # one subdirectory per branch
│   ├── main/
│   │   ├── index          # staging index for this branch
│   │   ├── info           # branch metadata
│   │   └── HEAD_COMMIT    # filename of last commit zip
│   └── feature-x/
│       ├── index
│       ├── info
│       └── HEAD_COMMIT
├── tags/                  # one file per tag
├── stash/                 # stash entries (one dir per stash)
│   └── 20250513_142300/
│       ├── files.zip      # stashed file contents
│       ├── index          # index snapshot
│       └── message        # optional description
├── data/                  # active commit archives
│   ├── 20250513_142300.zip
│   └── 20250513_142300_2.zip   # collision suffix if same second
└── archive/               # old commits moved here by 'vc archive'
```

---

## Configuration File

Located at `.vc/config.vc`. Created by `vc init`, updated by `vc config`.

```ini
[Repo]
  user      = Jane Doe
  email     = jane@example.com
  project   = myapp
  host      = 192.168.0.170
  vcdUser   = kelly
  repo      = users/kelly/myapp
```

| Key | Description |
|-----|-------------|
| `user` | Commit author name — appears in `MANIFEST.txt` |
| `email` | Author email address |
| `project` | Project name — shown in `vc status`, `vc list`, `vc info` |
| `host` | Remote hostname or IP for `vc push` / `vc pull` / `vc clone` |
| `vcdUser` | vcd daemon username for remote operations |
| `repo` | Remote repo path relative to vcd repoRoot (e.g. `users/kelly/myapp`) |
| `port` | vcd port number (default 9876) |
| `owner` | OS login of the user who ran `vc init` — set automatically |
| `private` | `true` or `false` — if true, only the owner may push or pull |

---

## Ignore Patterns

Create `.vcignore` in the project root, or use `vc ignore <pattern>`
to manage it interactively. Uses `fnmatch` glob syntax.

```
# Compiled output
*.o
*.a
*.so

# Editor temporaries
*.swp
*~

# Build directory
build/
```

---

## Index File

Each branch has its own index at `.vc/branches/<branch>/index`.
One line per tracked file or empty directory:

```
<relative-path>|<state>|<mtime>|<size>
```

| Field | Description |
|-------|-------------|
| `relative-path` | Path from project root. Directories end with `/` |
| `state` | `staged`, `committed`, or `modified` |
| `mtime` | Unix timestamp at last `vc add` or `vc commit` |
| `size` | File size in bytes at last `vc add` or `vc commit` |

The index is written atomically via temp-file rename.

---

## Commit Archives

Each `vc commit` creates a zip in `.vc/data/`:

```
.vc/data/20250513_142300.zip
```

Contains staged files plus `MANIFEST.txt`:

```
vc commit manifest
------------------
archive : 20250513_142300.zip
project : myapp
author  : Jane Doe
date    : 2025-05-13 14:23:00 UTC
files   : 8
message : Fix null pointer in parser
```

Each archive contains only the **files staged for that commit**.
`vc checkout --all` reconstructs the full working tree by combining
all archives newest-first. Use `vc show` to inspect a commit without
extracting anything.

```bash
unzip -l .vc/data/20250513_142300.zip    # list files
unzip .vc/data/20250513_142300.zip src/main.c  # extract one file
```

---

## Testing

```bash
make test
# or
gcc -o vctest vctest.c
./vctest ~/bin/vc
```

Pass/fail is shown live as each test runs. A summary of failures is
printed at the end. Push, pull, and clone tests are skipped
automatically (they require a live vcd server).

---

## vcg — Graphical Interface

`vcg` is a GTK3 desktop GUI for `vc`. It provides a visual tree of
branches, tags, and commits read directly from `.vc/` — no subprocess
required for browsing. All `vc` commands (commit, branch, push, pull,
etc.) are run via the `vc` binary and their output is shown in a
colour-coded output pane.

![vcg main screen](vcg/vcg_main.png)

### vcg Requirements

- GTK3 and `pkg-config`
- `vc` binary on `PATH`

| Platform | Install command |
|---|---|
| Debian/Ubuntu/Pi | `sudo apt-get install libgtk-3-dev pkg-config` |
| Fedora/RHEL | `sudo dnf install gtk3-devel pkgconfig` |
| Arch | `sudo pacman -S gtk3 pkgconf` |
| macOS | `brew install gtk+3 pkg-config` |
| Windows (MSYS2) | `pacman -S mingw-w64-x86_64-gtk3 mingw-w64-x86_64-pkg-config` |

Or use the bundled helper:

```bash
make deps -C vcg
```

### Building vcg

`vcg` lives in the `vcg/` subdirectory and is built automatically
when you run `make` from the vc root directory.

```bash
# Build everything (vc, vcd, and vcg)
make

# Build vcg only
make vcg

# Install vcg to /usr/local/bin
make vcg-install

# Clean all build artefacts including vcg
make clean
```

To build vcg standalone from inside its own directory:

```bash
cd vcg
make          # build
make install  # install to /usr/local/bin
make clean    # remove build artefacts
make deps     # install GTK3 dev libraries
```

The vcg source files are:

| File | Purpose |
|---|---|
| `vcg.c` | Entry point (`main`) |
| `vcg.h` | Shared types, constants, and forward declarations |
| `vcgUi.c` | Window and widget construction |
| `vcgCallbacks.c` | Button and menu event handlers |
| `vcgConfig.c` | Persistent settings load/save |
| `vcgOutput.c` | Colour-coded output pane |
| `vcgPrefs.c` | Preferences dialog |
| `vcgTree.c` | Repository tree (branches/tags/commits) |

### vcg Features

- **Repository tree** — reads `.vc/` directly; shows branches, tags,
  and commits without running any subprocess
- **Context menu** — right-click a branch, tag, or commit for actions
- **Output pane** — colour-coded: commands in blue, success in green,
  errors in red
- **Preferences** — font, colours, window size, and working directory
  saved automatically on exit (`~/.config/vcg/vcg.conf`)
- **Browse button** — file-chooser dialog to set the working directory
- **Double-click** — loads a branch/tag/commit into the input fields
