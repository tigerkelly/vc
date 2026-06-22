# vc / vcd Tutorial

A step-by-step guide to setting up and using the `vc` version control
system and its companion `vcd` daemon.

---

## Table of Contents

1. [What is vc?](#1-what-is-vc)
2. [Installing on the Server](#2-installing-on-the-server)
3. [Setting up vcd](#3-setting-up-vcd)
4. [Adding Users](#4-adding-users)
5. [Installing vc on a Client](#5-installing-vc-on-a-client)
6. [Starting your first project](#6-starting-your-first-project)
7. [Day-to-day workflow](#7-day-to-day-workflow)
8. [Working with branches](#8-working-with-branches)
9. [Collaborating — shared repos](#9-collaborating--shared-repos)
10. [Cloning a repo on another machine](#10-cloning-a-repo-on-another-machine)
11. [Checking remote status](#11-checking-remote-status)
12. [Browsing available repos](#12-browsing-available-repos)
13. [Merging branches](#13-merging-branches)
14. [Using vcg (graphical interface)](#14-using-vcg-graphical-interface)

---

## 1. What is vc?

`vc` is a lightweight version control system where each commit is a
**timestamped zip snapshot** of your staged files. It is designed for
personal and small-team use on a LAN.

The system has three components:

| Component | What it does |
|---|---|
| `vc` | Command-line client — commit, push, pull, branch, merge |
| `vcd` | Server daemon — manages remote repositories, handles authentication |
| `vcg` | Optional GTK3 desktop GUI for `vc` |

`vcd` runs on a Linux server (e.g. a Raspberry Pi) and listens on
TCP port 9876. The `vc` client connects to it for push, pull, and
clone operations.

---

## 2. Installing on the Server

On the machine that will host repositories (Linux only for `vcd`):

```bash
# Clone or copy the vc source
cd ~/work
git clone <source>   # or copy the directory

# Install build dependencies
cd vc
make install-deps    # runs: sudo apt-get install libzip-dev libreadline-dev

# Build everything
make

# Copy vcd to a system location
sudo cp vcd /home/pi/bin/vcd    # or wherever you keep binaries
```

---

## 3. Setting up vcd

`vcd` needs a directory to store repositories and its configuration.
Run `--init` once as root to create everything:

```bash
sudo /home/pi/bin/vcd --init --reporoot /ssd/repo
```

This creates:
- `/ssd/repo/` — root directory for all repositories
- `/ssd/repo/vcd.conf` — server configuration
- `/ssd/repo/users/` — personal repository directories
- `/ssd/repo/shared/` — shared repository directories

The generated `vcd.conf` looks like:

```ini
port     = 9876
repoRoot = /ssd/repo
userDb   = /ssd/repo/users.db
logFile  = /ssd/repo/vcd.log
runUser  = nobody
```

> **Note:** `vcd.conf` and `users.db` live alongside `repoRoot` so
> that `vcd` can read them after dropping privileges to `nobody`.

### Start vcd

```bash
sudo /home/pi/bin/vcd --start &
```

You should see:

```
vcd v1.0 starting  (built Jun  7 2026 14:33:05)
  Port      : 9876
  Repo root : /ssd/repo
  User DB   : /ssd/repo/users.db
  Log       : /ssd/repo/vcd.log
  Running as: nobody (uid=65534 gid=65534)
Listening...
```

### Run vcd as a system service (optional)

Create `/etc/systemd/system/vcd.service`:

```ini
[Unit]
Description=vc repository daemon
After=network.target

[Service]
Type=simple
ExecStart=/home/pi/bin/vcd --start
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Then enable it:

```bash
sudo systemctl enable vcd
sudo systemctl start vcd
```

---

## 4. Adding Users

Each person who wants to push or pull needs a vcd account (separate
from their OS login).

```bash
sudo /home/pi/bin/vcd --adduser kelly
```

You will be prompted:

```
Config:
  repoRoot : /ssd/repo
  userDb   : /ssd/repo/users.db

Repo directory under /ssd/repo/
  Press Enter for 'users/kelly':

New password  : ****
Confirm       : ****
User 'kelly' added.
Repo dir : /ssd/repo/users/kelly
```

### Create a repository for the user

```bash
sudo /home/pi/bin/vcd --initrepo users/kelly/myproject
```

Or skip this — `vc push` will offer to create the repo automatically
on the first push.

### Useful admin commands

```bash
sudo vcd --listusers          # list all users and their repo directories
sudo vcd --passwd kelly       # change a user's password
sudo vcd --version            # print version and build date
```

---

## 5. Installing vc on a Client

On the machine you will develop on (Linux, macOS, or Windows/MSYS2):

```bash
cd vc
make vc       # build just the client (no vcd needed)
cp vc ~/bin/  # put it on your PATH
```

On **macOS** (requires Homebrew):
```bash
make install-deps   # brew install libzip readline
make vc
```

On **Windows** (MSYS2 MinGW64 terminal):
```bash
make install-deps   # pacman -S mingw-w64-x86_64-libzip etc.
make vc
cp vc.exe ~/bin/
```

---

## 6. Starting your first project

### Create the project directory

```bash
mkdir ~/work/myproject
cd ~/work/myproject
```

### Initialise a vc repository

```bash
vc init
```

`vc init` prompts for the details it needs:

```
Initialising vc repository...

Enter repository details (leave blank to cancel):

  User name    : Kelly Wiles          ← read from OS automatically
  Project name : myproject            ← defaults to directory name
  Email        : kelly@example.com    ← enter your email
  Remote host  : (optional) 192.168.0.170
  vcd username : kelly
  vcd password : ****
  Verifying vcd credentials... OK

Repository initialised in /home/kelly/work/myproject/.vc/
Remote repo  : users/kelly/myproject
```

> **Tip:** If you want to work locally only (no remote), just press
> Enter at the `Remote host` prompt.

### Check status

```bash
vc status
```

```
Project : myproject  |  Branch : main
---

  Untracked files (use 'vc add' to track):
    untracked  README.md
    untracked  src/main.c

  0 staged, 0 modified, 2 untracked
```

### Stage and commit files

```bash
# Stage all files in the project
vc add

# Or stage specific files
vc add src/main.c README.md

# Check what's staged
vc status

# Commit
vc commit --msg "Initial commit"
```

```
Commit: 20260607_143200.zip
  Committed 3 file(s).
```

### Push to the server

```bash
vc push
```

```
Pushing to kelly@192.168.0.170:users/kelly/myproject
  Pushing .vc/data/20260607_143200.zip ... done
  Pushing .vc/branches/main/index     ... done
Push complete: 2 file(s) sent.
```

---

## 7. Day-to-day workflow

The typical day-to-day cycle:

```bash
# Pull latest changes from server (if working on multiple machines)
vc pull

# Edit files...

# Stage changes
vc add

# Review what's staged
vc status

# Commit
vc commit --msg "Fixed the login timeout bug"

# Push to server
vc push
```

### Viewing history

```bash
vc list                  # list all commits on current branch
vc log                   # brief log with messages
vc show 20260607_143200  # show files in a specific commit
```

### Viewing differences

```bash
vc diff src/main.c       # diff working file against last commit
vc diff --meld src/      # open Meld visual diff tool
```

### Undoing changes

```bash
vc revert src/main.c     # restore file to last committed version
vc stash                 # stash all staged changes for later
vc stash pop             # restore stashed changes
```

---

## 8. Working with branches

Branches let you work on a feature without affecting the main branch.

### Create a branch

```bash
vc branch new feature-login
vc branch switch feature-login
```

You are now on `feature-login`. Make changes, commit as normal.

### List branches

```bash
vc branch list
```

```
  main           (3 commits)
* feature-login  (1 commit)
```

### Merge back to main

```bash
vc branch switch main
vc merge feature-login
```

If the two branches changed **different parts** of a file, `vc merge`
handles it automatically:

```
  merged    src/auth.c
  added     src/login.c

Merge complete:
  Added  : 1 file(s)
  Merged : 1 file(s)
  Unchanged : 5 file(s)

Run 'vc commit --msg "Merge feature-login"' to record the merge.
```

If both branches changed the **same lines**, conflict markers are
written into the file:

```
<<<<<<< main
    int timeout = 30;
||||||| base
    int timeout = 10;
=======
    int timeout = 60;
>>>>>>> feature-login
```

Open the file, decide which version to keep (or combine them),
remove the markers, then:

```bash
vc add src/auth.c
vc commit --msg "Merge feature-login (resolved conflict in auth.c)"
```

### Delete a branch

```bash
vc branch delete feature-login
```

---

## 9. Collaborating — shared repos

A shared repo is accessible to multiple users.

### Create a shared repo on the server

```bash
sudo vcd --initrepo shared/teamapp
```

Grant access to specific users by setting `allowedUsers` in the repo's
config on the server:

```bash
cd /ssd/repo/shared/teamapp
vc config --set allowedUsers alice,bob,kelly
```

### Use the shared repo

Each team member points their local repo at the shared path:

```bash
vc config --set repo shared/teamapp
vc push
vc pull
```

### Make a personal repo public (readable by all)

```bash
vc config --set private false
```

### Move a repo from personal to shared

```bash
vc moverepo users/kelly/myproject shared/myproject
```

This moves the directory on the server and updates your local
`config.vc` automatically.

---

## 10. Cloning a repo on another machine

To get a copy of an existing remote repo on a new machine:

```bash
vc clone --user kelly 192.168.0.170 users/kelly/myproject
cd myproject
vc status
```

`vc clone` downloads the full `.vc/` directory, checks out the latest
commit, and saves your password to `.vc/auth` so you won't be
prompted again.

### Browse available repos first

If you don't know the exact repo path, list what's available:

```bash
vc repos 192.168.0.170 kelly
```

```
Repositories on 192.168.0.170 visible to kelly:

  users/kelly/myproject                     mine
  users/kelly/ushell                        mine
  shared/teamapp                            shared
  users/alice/opentools                     public

4 repos found.
```

---

## 11. Checking remote status

`vc status` is fast and offline by default. Add `--remote` to also
check whether the remote repo is ahead or behind:

```bash
vc status --remote
```

```
Remote  : kelly@192.168.0.170:users/kelly/myproject
Private : no
Remote  : 2 commits ahead — run 'vc pull'
---

  0 staged, 1 modified, 0 untracked
```

| Message | Action |
|---|---|
| `up to date` | Nothing to do |
| `N commits ahead — run 'vc pull'` | Someone else pushed — pull first |
| `N commits behind — run 'vc push'` | You have unpushed commits |

---

## 12. Browsing available repos

```bash
vc repos <host> <username>
```

Lists all repos you can see:
- **mine** — your own personal repos
- **shared** — shared repos you are listed in
- **public** — other users' repos where `private = false`

```bash
vc repos 192.168.0.170 kelly
vcd password for kelly@192.168.0.170: ****

Repositories on 192.168.0.170 visible to kelly:

  users/kelly/myproject                     mine
  shared/teamapp                            shared

2 repos found.
```

---

## 13. Merging branches

`vc merge` uses a built-in **3-way line-level merge** — no external
tools required. It works on Linux, macOS, and Windows.

```bash
vc branch switch main
vc merge feature-x
```

The merge compares:
- **base** — the last committed version of the file in `main`
- **ours** — the current `main` version
- **theirs** — the `feature-x` version

Files changed in different places are merged cleanly. Files changed
in the same place get conflict markers for manual resolution.

See [section 8](#8-working-with-branches) for the full merge workflow.

---

## 14. Using vcg (graphical interface)

`vcg` is an optional GTK3 desktop GUI. Build it alongside `vc`:

```bash
make              # builds vc, vcd, and vcg
make vcg-install  # install vcg to /usr/local/bin
```

Launch it from your project directory:

```bash
cd ~/work/myproject
vcg
```

The interface has:
- **Left sidebar** — Quick Actions (Status, Pull, Push, Auth) and
  repository tree (branches, tags, commits)
- **Tabbed panels** — Source, Files, Manage, Info
- **Output pane** — colour-coded output from every `vc` command

All `vc` commands are accessible from the GUI — no need to use the
terminal for day-to-day work.

---

## Quick Reference

| Task | Command |
|---|---|
| Initialise a repo | `vc init` |
| Stage all files | `vc add` |
| Stage a file | `vc add <file>` |
| Commit | `vc commit --msg "message"` |
| Push to server | `vc push` |
| Pull from server | `vc pull` |
| Show status | `vc status` |
| Show status + remote | `vc status --remote` |
| List commits | `vc list` |
| Diff a file | `vc diff <file>` |
| Revert a file | `vc revert <file>` |
| New branch | `vc branch new <name>` |
| Switch branch | `vc branch switch <name>` |
| Merge a branch | `vc merge <name>` |
| Clone a repo | `vc clone --user <u> <host> <path>` |
| Browse repos | `vc repos <host> <user>` |
| Move repo | `vc moverepo <src> <dst>` |
| Show version | `vc version` |
| Get help | `vc help` |

---

*See [README.md](README.md) for full command reference and
[Compare.md](Compare.md) for a comparison with git.*
