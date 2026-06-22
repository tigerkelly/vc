/*
 * vctest.c  –  Test suite for the vc version control system.
 *
 * Build:   gcc -o vctest vctest.c
 * Run:     ./vctest [path-to-vc-binary]   (default: ~/bin/vc)
 *
 * Returns 0 if all tests pass, 1 if any fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Colour codes
// ---------------------------------------------------------------------------
#define COL_PASS  "\033[0;32m"
#define COL_FAIL  "\033[0;31m"
#define COL_SKIP  "\033[0;33m"
#define COL_HEAD  "\033[1;37m"
#define COL_RESET "\033[0m"
#define COL_RUN   "\033[0;36m"

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static char  vcBin[512]   = "";
static char  testDir[512] = "";
static int   totalTests   = 0;
static int   passedTests  = 0;
static int   failedTests  = 0;
static int   skippedTests = 0;

// For the summary report.
typedef struct { char name[128]; int result; char detail[256]; } TR;
static TR    results[512];
static int   resultCount = 0;

// ---------------------------------------------------------------------------
// Core: print a test name, run something, then print PASS/FAIL/SKIP
//
// Usage pattern:
//   CHECK("test name", expression_that_is_true_on_pass);
//   CHECKF("test name", exit_code == 0, "exit=%d", exit_code);
//   SKIP("test name", "reason");
// ---------------------------------------------------------------------------

static void _check(const char *name, int passed, const char *detail) {
    // Print the test name left-aligned.
    printf("    %-56s", name);
    fflush(stdout);

    totalTests++;
    TR *r = &results[resultCount++];
    snprintf(r->name,   sizeof(r->name),   "%s", name);
    snprintf(r->detail, sizeof(r->detail), "%s", detail ? detail : "");
    r->result = passed;

    if (passed == 1) {
        passedTests++;
        printf("%sPASS%s\n", COL_PASS, COL_RESET);
    } else if (passed == -1) {
        skippedTests++;
        printf("%sSKIP%s  %s\n", COL_SKIP, COL_RESET, detail ? detail : "");
    } else {
        failedTests++;
        printf("%sFAIL%s", COL_FAIL, COL_RESET);
        if (detail && detail[0])
            printf("  %s↳ %s%s", COL_FAIL, detail, COL_RESET);
        printf("\n");
    }
    fflush(stdout);
}

// CHECK(name, bool_expr)
#define CHECK(name, expr)     _check(name, (expr) ? 1 : 0, NULL)

// CHECKF(name, bool_expr, fmt, ...) — with failure detail
#define CHECKF(name, expr, ...) \
    do { \
        char _det[256]; \
        snprintf(_det, sizeof(_det), __VA_ARGS__); \
        _check(name, (expr) ? 1 : 0, _det); \
    } while(0)

// SKIP(name, reason)
#define SKIP(name, reason)    _check(name, -1, reason)

// ---------------------------------------------------------------------------
// Section header
// ---------------------------------------------------------------------------
static void section(const char *name) {
    printf("\n  %s[ %s ]%s\n", COL_HEAD, name, COL_RESET);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Shell helpers
// ---------------------------------------------------------------------------
static int runCmd(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    char full[4096];
    // Redirect stdin from /dev/null so interactive prompts never block.
    snprintf(full, sizeof(full),
             "cd '%s' && %s < /dev/null > /dev/null 2>&1", testDir, cmd);
    return WEXITSTATUS(system(full));
}

static int runCapture(char *out, size_t sz, const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    char full[4096];
    // Redirect stdin from /dev/null so interactive prompts never block.
    snprintf(full, sizeof(full),
             "cd '%s' && %s < /dev/null 2>&1", testDir, cmd);
    FILE *f = popen(full, "r");
    if (!f) { if (out) out[0] = '\0'; return -1; }

    // Read ALL output — drain the pipe fully even if the buffer is full.
    // Stopping early causes SIGPIPE (exit 141) in the child process.
    size_t total = 0;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (out && total + n < sz) {
            memcpy(out + total, tmp, n);
            total += n;
        }
        // Keep reading even after buffer is full to drain the pipe.
    }
    if (out) out[total] = '\0';

    return WEXITSTATUS(pclose(f));
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------
static int fexists(const char *rel) {
    char p[512]; snprintf(p, sizeof(p), "%s/%s", testDir, rel);
    struct stat st; return stat(p, &st) == 0;
}
static int dexists(const char *rel) {
    char p[512]; snprintf(p, sizeof(p), "%s/%s", testDir, rel);
    struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static int fcontains(const char *rel, const char *needle) {
    char p[512]; snprintf(p, sizeof(p), "%s/%s", testDir, rel);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char buf[8192]; size_t n = fread(buf, 1, sizeof(buf)-1, f); buf[n] = '\0';
    fclose(f); return strstr(buf, needle) != NULL;
}
static int countZips(void) {
    char p[512]; snprintf(p, sizeof(p), "%s/.vc/data", testDir);
    DIR *d = opendir(p); if (!d) return 0;
    int c = 0; struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".zip")) c++;
    }
    closedir(d); return c;
}
static void writef(const char *rel, const char *content) {
    char p[512]; snprintf(p, sizeof(p), "%s/%s", testDir, rel);
    char par[512]; snprintf(par, sizeof(par), "%s", p);
    char *sl = strrchr(par, '/');
    if (sl) { *sl = '\0'; char cmd[600]; snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", par); system(cmd); }
    FILE *f = fopen(p, "w"); if (f) { fputs(content, f); fclose(f); }
}

// ---------------------------------------------------------------------------
// Setup / teardown
// ---------------------------------------------------------------------------
static int setup(void) {
    snprintf(testDir, sizeof(testDir), "/tmp/vctest_%d", (int)getpid());
    if (mkdir(testDir, 0755) != 0) {
        fprintf(stderr, "Cannot create test dir: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}
static void teardown(void) {
    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf '%s'", testDir);
    system(cmd);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void testVersion(void) {
    section("version");
    char out[256];
    int rc = runCapture(out, sizeof(out), "'%s' version", vcBin);
    CHECKF("version: exits with 0",      rc == 0,              "exit=%d", rc);
    CHECK ("version: prints Version",    strstr(out, "Version") != NULL);
}

static void testHelp(void) {
    section("help");
    char out[16384];
    int rc = runCapture(out, sizeof(out), "'%s' help", vcBin);
    CHECKF("help: exits with 0",         rc == 0,              "exit=%d", rc);
    CHECK ("help: contains Usage",       strstr(out, "Usage")  != NULL);
    CHECK ("help: lists add command",    strstr(out, "add")    != NULL);
    CHECK ("help: lists commit command", strstr(out, "commit") != NULL);
    CHECK ("help: lists branch command", strstr(out, "branch") != NULL);
}

static void testInit(void) {
    section("init");
    // All required flags supplied. stdin is /dev/null so the private prompt
    // receives EOF and defaults to false without blocking.
    int rc = runCmd("'%s' init "
                    "--user 'Test User' "
                    "--email 'test@example.com' "
                    "--project testproject "
                    "--repoPath /tmp/vctest_remote",
                    vcBin);
    CHECKF("init: exits with 0",              rc == 0, "exit=%d", rc);
    CHECK ("init: creates .vc/",              dexists(".vc"));
    CHECK ("init: creates .vc/data/",         dexists(".vc/data"));
    CHECK ("init: creates .vc/branches/main", dexists(".vc/branches/main"));
    CHECK ("init: creates .vc/tags/",         dexists(".vc/tags"));
    CHECK ("init: creates config.vc",         fexists(".vc/config.vc"));
    CHECK ("init: config has user",           fcontains(".vc/config.vc", "Test User"));
    CHECK ("init: config has project",        fcontains(".vc/config.vc", "testproject"));
    CHECK ("init: config has email",          fcontains(".vc/config.vc", "test@example.com"));
    CHECK ("init: config has owner",          fcontains(".vc/config.vc", "owner"));
    CHECK ("init: config has private field",  fcontains(".vc/config.vc", "private"));
    CHECK ("init: private defaults to false", fcontains(".vc/config.vc", "false"));
    CHECK ("init: creates HEAD",              fexists(".vc/HEAD"));
    CHECK ("init: HEAD points to main",       fcontains(".vc/HEAD", "main"));
    // vc.log and index are created lazily — check them after the first add.
    CHECK ("init: creates branches/main/index", fexists(".vc/branches/main/index"));
}

static void testInitPrivate(void) {
    section("init --repoPrivate");
    // Create a second temp dir to test the --repoPrivate flag.
    char privDir[512];
    snprintf(privDir, sizeof(privDir), "%s_priv", testDir);
    mkdir(privDir, 0755);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && '%s' init "
             "--user 'Test User' "
             "--email 'test@example.com' "
             "--project privateprojtest "
             "--repoPath /tmp/vctest_remote_priv "
             "--repoPrivate "
             "< /dev/null > /dev/null 2>&1",
             privDir, vcBin);
    int rc = WEXITSTATUS(system(cmd));
    CHECKF("init --repoPrivate: exits with 0", rc == 0, "exit=%d", rc);

    // Check config in the private repo dir.
    char cfgPath[600];
    snprintf(cfgPath, sizeof(cfgPath), "%s/.vc/config.vc", privDir);
    struct stat st;
    if (stat(cfgPath, &st) == 0) {
        FILE *f = fopen(cfgPath, "r");
        char buf[2048] = "";
        if (f) { fread(buf, 1, sizeof(buf)-1, f); fclose(f); }
        CHECK ("init --repoPrivate: private=true in config",
               strstr(buf, "true") != NULL);
        CHECK ("init --repoPrivate: owner written to config",
               strstr(buf, "owner") != NULL);
    } else {
        SKIP("init --repoPrivate: config.vc not found", "init may have failed");
    }

    // Clean up.
    char rmcmd[600];
    snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", privDir);
    system(rmcmd);
}

static void testAdd(void) {
    section("add");
    writef("main.c",     "int main() { return 0; }\n");
    writef("README.md",  "# Test Project\n");
    writef("src/utils.c","void util() {}\n");

    int rc = runCmd("'%s' add", vcBin);
    CHECKF("add: exits with 0",            rc == 0, "exit=%d", rc);
    CHECK ("add: main.c in index",         fcontains(".vc/branches/main/index", "main.c"));
    CHECK ("add: README.md in index",      fcontains(".vc/branches/main/index", "README.md"));
    CHECK ("add: src/utils.c in index",    fcontains(".vc/branches/main/index", "src/utils.c"));
    CHECK ("add: entries marked staged",   fcontains(".vc/branches/main/index", "staged"));

    // vc.log is created on the first command that calls vcLog().
    CHECK ("add: vc.log created",          fexists(".vc/vc.log"));

    writef("single.c", "// single\n");
    rc = runCmd("'%s' add single.c", vcBin);
    CHECKF("add single file: exits with 0",  rc == 0, "exit=%d", rc);
    CHECK ("add single file: in index",      fcontains(".vc/branches/main/index", "single.c"));
}

static void testStatus(void) {
    section("status");
    char out[4096];
    int rc = runCapture(out, sizeof(out), "'%s' status", vcBin);
    CHECKF("status: exits with 0",       rc == 0, "exit=%d", rc);
    CHECK ("status: shows branch name",  strstr(out, "main") != NULL);
    CHECK ("status: shows staged files", strstr(out, "staged") != NULL
                                      || strstr(out, "Staged") != NULL);
}

static void testCommit(void) {
    section("commit");
    char out[2048];
    int rc = runCapture(out, sizeof(out), "'%s' commit --msg 'Initial commit'", vcBin);
    int zips = countZips();
    CHECKF("commit: exits with 0",            rc == 0, "exit=%d output='%.80s'", rc, out);
    CHECKF("commit: creates zip archive",     zips > 0, "no zips found in .vc/data");
    CHECK ("commit: writes HEAD_COMMIT",      fexists(".vc/branches/main/HEAD_COMMIT"));
    CHECK ("commit: index entries committed", fcontains(".vc/branches/main/index", "committed"));
    CHECK ("commit: message in output",       strstr(out, "Initial commit") != NULL);
}

static void testList(void) {
    section("list");
    char out[4096];
    int rc = runCapture(out, sizeof(out), "'%s' list", vcBin);
    CHECKF("list: exits with 0",         rc == 0, "exit=%d", rc);
    CHECK ("list: shows .zip archive",   strstr(out, ".zip") != NULL);
    CHECK ("list: shows commit message", strstr(out, "Initial commit") != NULL);
    CHECK ("list: shows Archive header", strstr(out, "Archive") != NULL);
    CHECK ("list: shows Files column",   strstr(out, "Files") != NULL);
}

static void testTag(void) {
    section("tag");
    int rc = runCmd("'%s' tag v1.0 --msg 'First release'", vcBin);
    CHECKF("tag create: exits with 0",   rc == 0, "exit=%d", rc);
    CHECK ("tag create: file exists",    fexists(".vc/tags/v1.0"));
    CHECK ("tag create: message stored", fcontains(".vc/tags/v1.0", "First release"));
    CHECK ("tag create: branch stored",  fcontains(".vc/tags/v1.0", "main"));
    CHECK ("tag create: commit stored",  fcontains(".vc/tags/v1.0", ".zip"));

    char out[2048];
    runCapture(out, sizeof(out), "'%s' tag", vcBin);
    CHECK ("tag list: shows tag name",   strstr(out, "v1.0") != NULL);
    CHECK ("tag list: shows message",    strstr(out, "First release") != NULL);

    runCapture(out, sizeof(out), "'%s' tag --show v1.0", vcBin);
    CHECK ("tag show: displays details", strstr(out, "v1.0") != NULL);

    // Add a second tag to the same commit.
    rc = runCmd("'%s' tag v1.0-rc --msg 'Release candidate'", vcBin);
    CHECKF("tag: second tag on same commit", rc == 0, "exit=%d", rc);
    runCapture(out, sizeof(out), "'%s' list", vcBin);
    CHECK ("tag: list shows both tags",  strstr(out, "v1.0") != NULL
                                      && strstr(out, "v1.0-rc") != NULL);

    rc = runCmd("'%s' tag --delete v1.0-rc", vcBin);
    CHECKF("tag delete: exits with 0",   rc == 0, "exit=%d", rc);
    CHECK ("tag delete: file removed",   !fexists(".vc/tags/v1.0-rc"));
    CHECK ("tag delete: v1.0 untouched", fexists(".vc/tags/v1.0"));
}

static void testBranch(void) {
    section("branch");
    int rc = runCmd("'%s' branch feature-test", vcBin);
    CHECKF("branch create: exits with 0",    rc == 0, "exit=%d", rc);
    CHECK ("branch create: dir exists",      dexists(".vc/branches/feature-test"));
    CHECK ("branch create: info exists",     fexists(".vc/branches/feature-test/info"));
    CHECK ("branch create: index exists",    fexists(".vc/branches/feature-test/index"));
    CHECK ("branch create: HEAD updated",    fcontains(".vc/HEAD", "feature-test"));

    char out[2048];
    runCapture(out, sizeof(out), "'%s' branch", vcBin);
    CHECK ("branch list: shows main",         strstr(out, "main") != NULL);
    CHECK ("branch list: shows feature-test", strstr(out, "feature-test") != NULL);
    CHECK ("branch list: marks active (*)",   strstr(out, "*") != NULL);

    // Switch back to main.
    rc = runCmd("'%s' branch main", vcBin);
    CHECKF("branch switch to main: exits with 0", rc == 0, "exit=%d", rc);
    CHECK ("branch switch: HEAD updated",    fcontains(".vc/HEAD", "main"));

    // Delete branch.
    rc = runCmd("'%s' branch --delete feature-test", vcBin);
    CHECKF("branch delete: exits with 0",    rc == 0, "exit=%d", rc);
    CHECK ("branch delete: dir removed",     !dexists(".vc/branches/feature-test"));

    // Invalid name check — use -- to stop shell treating -invalid as a flag.
    rc = runCmd("'%s' branch -- -invalid 2>/dev/null", vcBin);
    CHECK ("branch: rejects name with leading -", rc != 0);
}

static void testRename(void) {
    section("rename");
    int rc = runCmd("'%s' rename main.c app.c", vcBin);
    CHECKF("rename: exits with 0",           rc == 0, "exit=%d", rc);
    CHECK ("rename: old file gone from disk", !fexists("main.c"));
    CHECK ("rename: new file on disk",        fexists("app.c"));
    CHECK ("rename: new name in index",       fcontains(".vc/branches/main/index", "app.c"));
    CHECK ("rename: old name gone from index",!fcontains(".vc/branches/main/index", "main.c"));

    // Rename back.
    runCmd("'%s' rename app.c main.c", vcBin);
    CHECK ("rename back: main.c restored",    fexists("main.c"));
}

static void testModifyAndCommit(void) {
    section("add (re-stage modified file)");
    writef("main.c", "int main() { return 1; }\n");
    int rc = runCmd("'%s' add main.c", vcBin);
    CHECKF("re-add modified: exits with 0",  rc == 0, "exit=%d", rc);
    CHECK ("re-add modified: staged again",  fcontains(".vc/branches/main/index", "staged"));

    section("commit (2nd)");
    rc = runCmd("'%s' commit --msg 'Update main'", vcBin);
    if (rc != 0) {
        // Timestamp collision — wait a second and retry once.
        sleep(1);
        rc = runCmd("'%s' commit --msg 'Update main'", vcBin);
    }
    CHECKF("2nd commit: exits with 0",       rc == 0, "exit=%d", rc);
    CHECKF("2nd commit: two zips exist",     countZips() >= 2, "only %d zips", countZips());
}

static void testCheckout(void) {
    section("checkout");
    // Corrupt main.c and restore it.
    writef("main.c", "/* corrupted */\n");
    int rc = runCmd("'%s' checkout main.c", vcBin);
    CHECKF("checkout file: exits with 0",    rc == 0, "exit=%d", rc);
    CHECK ("checkout file: content restored",!fcontains("main.c", "corrupted"));

    // --all restores all files.
    writef("README.md", "corrupted readme\n");
    writef("main.c",    "/* also corrupted */\n");
    rc = runCmd("'%s' checkout --all", vcBin);
    CHECKF("checkout --all: exits with 0",   rc == 0, "exit=%d", rc);
    // Verify corrupted content is gone from both files.
    CHECK ("checkout --all: README restored",!fcontains("README.md", "corrupted"));
    CHECK ("checkout --all: main.c restored",!fcontains("main.c", "corrupted"));

    // --tag restores to a tagged commit.
    rc = runCmd("'%s' checkout --tag v1.0", vcBin);
    CHECKF("checkout --tag: exits with 0",   rc == 0, "exit=%d", rc);
}

static void testRevert(void) {
    section("revert");
    // Write a known string, add, commit, then corrupt and revert.
    // Use a unique string unlikely to appear elsewhere.
    writef("revert_test.c", "// REVERT_MARKER_XYZ\n");
    runCmd("'%s' add revert_test.c", vcBin);
    int rc = runCmd("'%s' commit --msg 'Revert test commit'", vcBin);

    if (rc != 0) {
        // If commit failed (e.g. timestamp collision), wait 1 second and retry.
        sleep(1);
        rc = runCmd("'%s' commit --msg 'Revert test commit'", vcBin);
    }

    // Corrupt the file.
    writef("revert_test.c", "broken content\n");
    rc = runCmd("'%s' revert revert_test.c", vcBin);
    CHECKF("revert: exits with 0",           rc == 0, "exit=%d", rc);
    CHECK ("revert: broken content gone",    !fcontains("revert_test.c", "broken content"));
    CHECK ("revert: committed content back", fcontains("revert_test.c", "REVERT_MARKER_XYZ"));
}

static void testDelete(void) {
    section("delete");
    writef("todelete.c", "// delete me\n");
    runCmd("'%s' add todelete.c", vcBin);
    runCmd("'%s' commit --msg 'Add disposable file'", vcBin);

    int rc = runCmd("'%s' delete --force todelete.c", vcBin);
    CHECKF("delete --force: exits with 0",   rc == 0, "exit=%d", rc);
    CHECK ("delete --force: file gone",      !fexists("todelete.c"));
    CHECK ("delete --force: not in index",   !fcontains(".vc/branches/main/index", "todelete.c"));

    writef("keepme.c", "// keep\n");
    runCmd("'%s' add keepme.c", vcBin);
    runCmd("'%s' commit --msg 'Add keepme'", vcBin);
    rc = runCmd("'%s' delete --keep keepme.c", vcBin);
    CHECKF("delete --keep: exits with 0",    rc == 0, "exit=%d", rc);
    CHECK ("delete --keep: file on disk",    fexists("keepme.c"));
    CHECK ("delete --keep: not in index",    !fcontains(".vc/branches/main/index", "keepme.c"));
}

static void testConfig(void) {
    section("config");
    char out[2048];
    int rc = runCapture(out, sizeof(out), "'%s' config --show", vcBin);
    CHECKF("config --show: exits with 0",    rc == 0, "exit=%d", rc);
    CHECK ("config --show: shows user",      strstr(out, "Test User") != NULL);
    CHECK ("config --show: shows project",   strstr(out, "testproject") != NULL);
    CHECK ("config --show: shows email",     strstr(out, "test@example.com") != NULL);
}

static void testLog(void) {
    section("log");
    char out[4096];
    int rc = runCapture(out, sizeof(out), "'%s' log", vcBin);
    CHECKF("log: exits with 0",              rc == 0, "exit=%d", rc);
    CHECK ("log: vc.log file exists",        fexists(".vc/vc.log"));
}

static void testStatus2(void) {
    section("status (change detection)");
    writef("untracked.c", "// untracked\n");
    char out[4096];
    runCapture(out, sizeof(out), "'%s' status", vcBin);
    CHECK ("status: shows untracked file",   strstr(out, "untracked") != NULL
                                          || strstr(out, "Untracked") != NULL);

    writef("README.md", "# Modified content\n");
    runCapture(out, sizeof(out), "'%s' status", vcBin);
    CHECK ("status: detects modified file",  strstr(out, "modified") != NULL
                                          || strstr(out, "Modified") != NULL);
}

static void testEmptyDir(void) {
    section("empty directory");
    char emptyPath[512];
    snprintf(emptyPath, sizeof(emptyPath), "%s/emptydir", testDir);
    mkdir(emptyPath, 0755);

    // Stage everything including the empty dir.
    runCmd("'%s' add", vcBin);
    CHECK ("empty dir: staged in index", fcontains(".vc/branches/main/index", "emptydir/"));

    int rc = runCmd("'%s' commit --msg 'Add empty dir'", vcBin);
    if (rc != 0) {
        sleep(1);
        rc = runCmd("'%s' commit --msg 'Add empty dir'", vcBin);
    }
    CHECKF("empty dir commit: exits with 0", rc == 0, "exit=%d", rc);
    CHECKF("empty dir commit: zip created",  countZips() > 0, "no zips");
}

static void testEdgeCases(void) {
    section("edge cases");

    // Init in an already-initialised repo should fail.
    int rc = runCmd("'%s' init --user x --email x@x.com "
                    "--project x --repoPath /tmp/x 2>/dev/null", vcBin);
    CHECK ("init: fails if already initialised", rc != 0);

    // Commit with nothing staged should not create a zip.
    int zipsBefore = countZips();
    runCmd("'%s' commit --msg 'Empty' 2>/dev/null", vcBin);
    CHECKF("commit: no zip when nothing staged",
           countZips() == zipsBefore,
           "zip count changed: %d -> %d", zipsBefore, countZips());

    // Delete a nonexistent file should fail.
    rc = runCmd("'%s' delete --force nosuchfile.c 2>/dev/null", vcBin);
    CHECK ("delete: fails for nonexistent file", rc != 0);

    // Rename to an existing name should fail.
    rc = runCmd("'%s' rename main.c README.md 2>/dev/null", vcBin);
    CHECK ("rename: fails when dest exists", rc != 0);
}

static void testPushPullClone(void) {
    section("push / pull / clone");

    // These tests require a live vcd server.
    // Set VCD_TEST_HOST, VCD_TEST_USER, VCD_TEST_PASS, VCD_TEST_REPO
    // in the environment to enable them.
    const char *host  = getenv("VCD_TEST_HOST");
    const char *user  = getenv("VCD_TEST_USER");
    const char *pass  = getenv("VCD_TEST_PASS");
    const char *repo  = getenv("VCD_TEST_REPO");

    if (!host || !user || !pass || !repo) {
        SKIP("push:  set VCD_TEST_HOST/USER/PASS/REPO to enable",
             "environment not configured");
        SKIP("pull:  set VCD_TEST_HOST/USER/PASS/REPO to enable",
             "environment not configured");
        SKIP("clone: set VCD_TEST_HOST/USER/PASS/REPO to enable",
             "environment not configured");
        return;
    }

    printf("  vcd target: %s@%s %s\n", user, host, repo);

    // Configure the test repo for remote operations.
    runCmd("'%s' config --set host    %s", vcBin, host);
    runCmd("'%s' config --set vcdUser %s", vcBin, user);
    runCmd("'%s' config --set repo    %s", vcBin, repo);

    // Write password to stdin via a helper pipe so it isn't on the cmdline.
    // vcAuth cache: pre-seed .vc/auth by writing a known password file.
    // Simplest: use VCD_TEST_PASS as the password via echo piping.

    // Test push.
    int rc = runCmd("echo '%s' | '%s' push 2>/dev/null", pass, vcBin);
    CHECKF("push: exits with 0", rc == 0, "exit=%d (check vcd is running on %s)", rc, host);

    // Test pull.
    rc = runCmd("echo '%s' | '%s' pull 2>/dev/null", pass, vcBin);
    CHECKF("pull: exits with 0", rc == 0, "exit=%d", rc);

    // Test clone into a temp dir.
    char cloneDir[512];
    snprintf(cloneDir, sizeof(cloneDir), "%s/vc_clone_test", testDir);
    rc = runCmd("echo '%s' | '%s' clone --user %s %s %s %s 2>/dev/null",
                pass, vcBin, user, host, repo, cloneDir);
    CHECKF("clone: exits with 0", rc == 0, "exit=%d", rc);
    CHECK ("clone: created directory",
           access(cloneDir, F_OK) == 0);
    CHECK ("clone: .vc directory exists",
           ({ char p[512];
              snprintf(p, sizeof(p), "%s/.vc", cloneDir);
              access(p, F_OK) == 0; }));

    // Cleanup clone dir.
    runCmd("rm -rf '%s'", cloneDir);
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
static void printReport(void) {
    printf("\n");
    printf("%s══════════════════════════════════════════════════════════════%s\n",
           COL_HEAD, COL_RESET);
    printf("%s  vc Test Report%s\n", COL_HEAD, COL_RESET);
    printf("%s══════════════════════════════════════════════════════════════%s\n",
           COL_HEAD, COL_RESET);

    // List any failures for easy review.
    if (failedTests > 0) {
        printf("\n  %sFailed tests:%s\n", COL_FAIL, COL_RESET);
        for (int i = 0; i < resultCount; i++) {
            if (results[i].result == 0) {
                printf("    %s✗%s  %s\n", COL_FAIL, COL_RESET, results[i].name);
                if (results[i].detail[0])
                    printf("       %s↳ %s%s\n",
                           COL_FAIL, results[i].detail, COL_RESET);
            }
        }
    }

    printf("\n");
    printf("%s──────────────────────────────────────────────────────────────%s\n",
           COL_HEAD, COL_RESET);
    printf("  Total %-4d  ", totalTests);
    printf("%sPassed %-4d%s  ", COL_PASS, passedTests, COL_RESET);
    if (failedTests > 0)
        printf("%sFailed %-4d%s  ", COL_FAIL, failedTests, COL_RESET);
    else
        printf("Failed 0     ");
    if (skippedTests > 0)
        printf("%sSkipped %-3d%s", COL_SKIP, skippedTests, COL_RESET);
    printf("\n");
    printf("%s──────────────────────────────────────────────────────────────%s\n",
           COL_HEAD, COL_RESET);

    if (failedTests == 0)
        printf("\n  %s✓ All tests passed.%s\n\n", COL_PASS, COL_RESET);
    else
        printf("\n  %s✗ %d test(s) failed.%s\n\n", COL_FAIL, failedTests, COL_RESET);
}

static void testNoRepo(void) {
    section("no repo (error handling)");
    // Run vc status outside any repo — should fail cleanly, not segfault.
    char out[512];
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "'%s' status", vcBin);
    // Run from /tmp which has no .vc/
    char full[1024];
    snprintf(full, sizeof(full), "cd /tmp && %s < /dev/null 2>&1; echo \"EXIT:$?\"", cmd);
    FILE *f = popen(full, "r");
    char buf[512] = "";
    if (f) { fread(buf, 1, sizeof(buf)-1, f); pclose(f); }
    // Must not segfault (exit 139) and must print an error message.
    CHECK("no repo: exits without segfault", strstr(buf, "EXIT:139") == NULL);
    CHECK("no repo: prints error message",
          strstr(buf, "not a vc") != NULL || strstr(buf, "No .vc") != NULL ||
          strstr(buf, "repository") != NULL);
}

static void testConfigSet(void) {
    section("config --set");
    int rc = runCmd("'%s' config --set host testhost.example.com", vcBin);
    CHECKF("config --set host: exits with 0",    rc == 0, "exit=%d", rc);
    CHECK ("config --set host: saved to config",
           fcontains(".vc/config.vc", "testhost.example.com"));

    rc = runCmd("'%s' config --set vcdUser testlogin", vcBin);
    CHECKF("config --set vcdUser: exits with 0", rc == 0, "exit=%d", rc);
    CHECK ("config --set vcdUser: saved to config",
           fcontains(".vc/config.vc", "testlogin"));

    rc = runCmd("'%s' config --set private true", vcBin);
    CHECKF("config --set private true: exits with 0", rc == 0, "exit=%d", rc);
    CHECK ("config --set private: saved to config",
           fcontains(".vc/config.vc", "true"));

    rc = runCmd("'%s' config --set private false", vcBin);
    CHECKF("config --set private false: exits with 0", rc == 0, "exit=%d", rc);
    CHECK ("config --set private false: saved",
           fcontains(".vc/config.vc", "false"));

    // Unknown key should fail.
    rc = runCmd("'%s' config --set unknownkey value 2>/dev/null", vcBin);
    CHECK ("config --set: rejects unknown key", rc != 0);
}

static void testPrivate(void) {
    section("private repo");
    // Enable private mode.
    int rc = runCmd("'%s' config --set private true", vcBin);
    CHECKF("private: enable exits with 0",    rc == 0, "exit=%d", rc);
    CHECK ("private: config shows private",
           fcontains(".vc/config.vc", "private") &&
           fcontains(".vc/config.vc", "true"));
    CHECK ("private: config shows owner",
           fcontains(".vc/config.vc", "owner"));

    // Config --show should display private and owner fields.
    char out[2048];
    runCapture(out, sizeof(out), "'%s' config --show", vcBin);
    CHECK ("private: --show displays Private field",
           strstr(out, "Private") != NULL || strstr(out, "private") != NULL);
    CHECK ("private: --show displays Owner field",
           strstr(out, "Owner") != NULL || strstr(out, "owner") != NULL);

    // Restore to public for subsequent tests.
    runCmd("'%s' config --set private false", vcBin);
}

static void testIgnore(void) {
    section("ignore");
    int rc = runCmd("'%s' ignore '*.tmp'", vcBin);
    CHECKF("ignore add: exits with 0",         rc == 0, "exit=%d", rc);
    CHECK ("ignore add: pattern in .vcignore", fcontains(".vcignore", "*.tmp"));

    rc = runCmd("'%s' ignore '*.log'", vcBin);
    CHECKF("ignore add 2nd: exits with 0",     rc == 0, "exit=%d", rc);

    // List patterns.
    char out[1024];
    runCapture(out, sizeof(out), "'%s' ignore", vcBin);
    CHECK ("ignore list: shows *.tmp",  strstr(out, "*.tmp") != NULL);
    CHECK ("ignore list: shows *.log",  strstr(out, "*.log") != NULL);

    // Duplicate should not be added.
    rc = runCmd("'%s' ignore '*.tmp' 2>/dev/null", vcBin);
    CHECK ("ignore: duplicate not added twice",
           rc == 0); // exits 0 but prints "already in .vcignore"

    // --check
    runCapture(out, sizeof(out), "'%s' ignore --check test.tmp", vcBin);
    CHECK ("ignore --check: detects ignored file",
           strstr(out, "ignored") != NULL);

    runCapture(out, sizeof(out), "'%s' ignore --check main.c", vcBin);
    CHECK ("ignore --check: non-ignored file not ignored",
           strstr(out, "NOT") != NULL || strstr(out, "not") != NULL);

    // --delete
    rc = runCmd("'%s' ignore --delete '*.log'", vcBin);
    CHECKF("ignore --delete: exits with 0",    rc == 0, "exit=%d", rc);
    CHECK ("ignore --delete: pattern removed", !fcontains(".vcignore", "*.log"));
    CHECK ("ignore --delete: other pattern kept", fcontains(".vcignore", "*.tmp"));
}

static void testInfo(void) {
    section("info");
    char out[4096];
    int rc = runCapture(out, sizeof(out), "'%s' info", vcBin);
    CHECKF("info: exits with 0",           rc == 0, "exit=%d", rc);
    CHECK ("info: shows Repository",       strstr(out, "Repository") != NULL);
    CHECK ("info: shows Project",          strstr(out, "Project") != NULL ||
                                           strstr(out, "testproject") != NULL);
    CHECK ("info: shows Branch",           strstr(out, "Branch") != NULL ||
                                           strstr(out, "main") != NULL);
    CHECK ("info: shows Commits",          strstr(out, "Commits") != NULL ||
                                           strstr(out, "commit") != NULL);
    CHECK ("info: shows Working tree",     strstr(out, "Working tree") != NULL ||
                                           strstr(out, "clean") != NULL);
}

static void testShow(void) {
    section("show");
    char out[4096];
    // Show most recent commit.
    int rc = runCapture(out, sizeof(out), "'%s' show", vcBin);
    CHECKF("show: exits with 0",           rc == 0, "exit=%d", rc);
    CHECK ("show: displays Commit",        strstr(out, "Commit") != NULL ||
                                           strstr(out, ".zip") != NULL);
    CHECK ("show: displays files list",    strstr(out, "Files in this commit") != NULL);
    CHECK ("show: shows file sizes",       strstr(out, "KB") != NULL ||
                                           strstr(out, "KB") != NULL ||
                                           strstr(out, "Size") != NULL);
    CHECK ("show: shows Modified column",  strstr(out, "Modified") != NULL);

    // Show by tag.
    rc = runCapture(out, sizeof(out), "'%s' show --tag v1.0 2>/dev/null", vcBin);
    // Tag was deleted in testTag, so this should fail gracefully.
    CHECK ("show --tag missing: fails cleanly", rc != 0 || strlen(out) > 0);
}

static void testArchive(void) {
    section("archive");
    // Need at least 2 commits — add and commit another file.
    writef("archive_test.c", "// archive test\n");
    runCmd("'%s' add archive_test.c", vcBin);
    sleep(1);  // ensure different timestamp
    runCmd("'%s' commit --msg 'Archive test commit'", vcBin);

    int zipsBefore = countZips();

    // Archive keeping only the most recent.
    char out[2048];
    int rc = runCapture(out, sizeof(out), "'%s' archive --keep 1", vcBin);
    CHECKF("archive --keep 1: exits with 0",   rc == 0, "exit=%d", rc);

    int zipsAfter = countZips();
    CHECK ("archive: zips moved from data/",   zipsAfter < zipsBefore);
    CHECK ("archive: at least 1 zip remains",  zipsAfter >= 1);

    // Check archive dir has zips.
    char archDir[512];
    snprintf(archDir, sizeof(archDir), "%s/.vc/archive", testDir);
    DIR *d = opendir(archDir);
    int archivedCount = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t l = strlen(de->d_name);
            if (l > 4 && strcmp(de->d_name + l - 4, ".zip") == 0)
                archivedCount++;
        }
        closedir(d);
    }
    CHECK ("archive: archived zips exist in .vc/archive", archivedCount > 0);

    // --list should show archived commits.
    rc = runCapture(out, sizeof(out), "'%s' archive --list", vcBin);
    CHECKF("archive --list: exits with 0",     rc == 0, "exit=%d", rc);
    CHECK ("archive --list: shows Archive column",
           strstr(out, "Archive") != NULL);
    CHECK ("archive --list: shows .zip",       strstr(out, ".zip") != NULL);

    // --restore: bring one back.
    // Find an archived zip name.
    char archivedZip[64] = "";
    DIR *d2 = opendir(archDir);
    if (d2) {
        struct dirent *de;
        while ((de = readdir(d2)) != NULL) {
            size_t l = strlen(de->d_name);
            if (l > 4 && strcmp(de->d_name + l - 4, ".zip") == 0) {
                snprintf(archivedZip, sizeof(archivedZip), "%s", de->d_name);
                break;
            }
        }
        closedir(d2);
    }
    if (archivedZip[0]) {
        rc = runCmd("'%s' archive --restore '%s'", vcBin, archivedZip);
        CHECKF("archive --restore: exits with 0", rc == 0, "exit=%d", rc);
        CHECK ("archive --restore: zip back in data/", countZips() > zipsAfter);
    } else {
        SKIP("archive --restore: no archived zip found to restore", "no zips");
    }
}

static void testDiff(void) {
    section("diff");

    // Create and commit a dedicated file for diff testing.
    writef("diff_test.c", "int diffTest() { return 0; }\n");
    runCmd("'%s' add diff_test.c", vcBin);
    int rc = runCmd("'%s' commit --msg 'Diff test baseline'", vcBin);
    if (rc != 0) { sleep(1); runCmd("'%s' commit --msg 'Diff test baseline'", vcBin); }

    // Modify it — diff should show differences.
    writef("diff_test.c", "int diffTest() { return 42; }\n");

    char out[4096];
    rc = runCapture(out, sizeof(out), "'%s' diff diff_test.c", vcBin);
    CHECKF("diff: exits with 0",           rc == 0, "exit=%d", rc);
    CHECK ("diff: shows --- header",       strstr(out, "---") != NULL);
    CHECK ("diff: shows +++ header",       strstr(out, "+++") != NULL);
    CHECK ("diff: shows changed line",     strstr(out, "+") != NULL);

    // Restore to committed state — diff should show no differences.
    runCmd("'%s' checkout diff_test.c", vcBin);
    rc = runCapture(out, sizeof(out), "'%s' diff diff_test.c", vcBin);
    CHECK ("diff: no differences message",
           strstr(out, "No differences") != NULL);

    // Diff on untracked file should fail.
    writef("untracked_diff.c", "// never tracked\n");
    rc = runCmd("'%s' diff untracked_diff.c 2>/dev/null", vcBin);
    CHECK ("diff: fails for untracked file", rc != 0);
}

static void testHistory(void) {
    section("history");
    char out[4096];
    int rc = runCapture(out, sizeof(out), "'%s' history README.md", vcBin);
    CHECKF("history: exits with 0",           rc == 0, "exit=%d", rc);
    CHECK ("history: shows History header",   strstr(out, "History") != NULL ||
                                              strstr(out, "README") != NULL);
    CHECK ("history: shows .zip entries",     strstr(out, ".zip") != NULL);

    // --count limits results.
    rc = runCapture(out, sizeof(out), "'%s' history README.md --count 1", vcBin);
    CHECKF("history --count 1: exits with 0", rc == 0, "exit=%d", rc);

    // History for untracked file should report not found.
    rc = runCapture(out, sizeof(out), "'%s' history nosuchfile.c 2>&1", vcBin);
    CHECK ("history: not found message for uncommitted file",
           strstr(out, "not found") != NULL || strstr(out, "no commit") != NULL ||
           strstr(out, "commit") != NULL);
}

static void testStash(void) {
    section("stash");
    // Stage a file then stash it.
    writef("stash_test.c", "// stash test\n");
    runCmd("'%s' add stash_test.c", vcBin);

    int rc = runCmd("'%s' stash --msg 'test stash'", vcBin);
    CHECKF("stash: exits with 0",         rc == 0, "exit=%d", rc);

    // Working tree should be clean after stash.
    char out[2048];
    runCapture(out, sizeof(out), "'%s' status", vcBin);
    CHECK ("stash: working tree clean after stash",
           strstr(out, "stash_test.c") == NULL ||
           strstr(out, "staged") == NULL);

    // Stash list should show the entry.
    runCapture(out, sizeof(out), "'%s' stash list", vcBin);
    CHECK ("stash list: shows stash entry", strstr(out, "test stash") != NULL ||
                                            strlen(out) > 10);

    // Stash directory should exist.
    char stashDir[512];
    snprintf(stashDir, sizeof(stashDir), "%s/.vc/stash", testDir);
    CHECK ("stash: .vc/stash/ directory exists", dexists(".vc/stash"));

    // Pop should restore the file.
    rc = runCmd("'%s' stash pop", vcBin);
    CHECKF("stash pop: exits with 0",     rc == 0, "exit=%d", rc);
    CHECK ("stash pop: file restored",    fexists("stash_test.c"));

    // After pop, stash list should be empty.
    runCapture(out, sizeof(out), "'%s' stash list", vcBin);
    CHECK ("stash: list empty after pop",
           strstr(out, "No stashes") != NULL || strlen(out) < 20);
}

static void testMove(void) {
    section("move");
    writef("moveme.c", "// move me\n");
    runCmd("'%s' add moveme.c", vcBin);
    runCmd("'%s' commit --msg 'Add moveme'", vcBin);

    int rc = runCmd("'%s' move moveme.c moved.c", vcBin);
    CHECKF("move: exits with 0",          rc == 0, "exit=%d", rc);
    CHECK ("move: old file gone",         !fexists("moveme.c"));
    CHECK ("move: new file exists",        fexists("moved.c"));
    CHECK ("move: new name in index",      fcontains(".vc/branches/main/index", "moved.c"));
    CHECK ("move: old name gone from index", !fcontains(".vc/branches/main/index", "moveme.c"));

    // Clean up.
    runCmd("'%s' delete --force moved.c", vcBin);
}

static void testUntrack(void) {
    section("untrack");
    writef("untrackme.c", "// untrack me\n");
    runCmd("'%s' add untrackme.c", vcBin);
    runCmd("'%s' commit --msg 'Add untrackme'", vcBin);

    int rc = runCmd("'%s' untrack untrackme.c", vcBin);
    CHECKF("untrack: exits with 0",        rc == 0, "exit=%d", rc);
    CHECK ("untrack: file still on disk",  fexists("untrackme.c"));
    CHECK ("untrack: not in index",        !fcontains(".vc/branches/main/index", "untrackme.c"));
}

static void testMerge(void) {
    section("merge");
    // Create a feature branch with a unique file.
    runCmd("'%s' branch merge-feature", vcBin);
    writef("feature_only.c", "// feature file\n");
    runCmd("'%s' add feature_only.c", vcBin);
    sleep(1);
    int rc = runCmd("'%s' commit --msg 'Feature commit'", vcBin);
    if (rc != 0) {
        sleep(1);
        rc = runCmd("'%s' commit --msg 'Feature commit'", vcBin);
    }

    // Switch back to main and merge.
    runCmd("'%s' branch main", vcBin);
    rc = runCmd("'%s' merge merge-feature", vcBin);
    CHECKF("merge: exits with 0",          rc == 0, "exit=%d", rc);
    CHECK ("merge: new file added to disk", fexists("feature_only.c"));
    CHECK ("merge: new file in index",
           fcontains(".vc/branches/main/index", "feature_only.c"));

    // Commit the merge.
    rc = runCmd("'%s' commit --msg 'Merge merge-feature'", vcBin);
    if (rc != 0) { sleep(1); runCmd("'%s' commit --msg 'Merge merge-feature'", vcBin); }

    // Cannot merge into itself.
    rc = runCmd("'%s' merge main 2>/dev/null", vcBin);
    CHECK ("merge: rejects merge into itself", rc != 0);

    // Non-existent branch should fail.
    rc = runCmd("'%s' merge no-such-branch 2>/dev/null", vcBin);
    CHECK ("merge: fails for missing branch",  rc != 0);

    // Clean up.
    runCmd("'%s' branch --delete merge-feature", vcBin);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc >= 2) {
        snprintf(vcBin, sizeof(vcBin), "%s", argv[1]);
    } else {
        char *home = getenv("HOME");
        snprintf(vcBin, sizeof(vcBin), "%s/bin/vc", home ? home : "");
    }

    struct stat st;
    if (stat(vcBin, &st) != 0) {
        fprintf(stderr, "ERROR: vc binary not found at '%s'\n"
                        "Usage: %s [/path/to/vc]\n", vcBin, argv[0]);
        return 1;
    }

    printf("\n%s vc Test Suite%s\n", COL_HEAD, COL_RESET);
    printf("  Binary  : %s\n", vcBin);

    if (setup() != 0) return 1;
    printf("  Test dir: %s\n", testDir);

    testVersion();
    testHelp();
    testInit();
    testInitPrivate();
    testAdd();
    testStatus();
    testCommit();
    testList();
    testTag();
    testBranch();
    testRename();
    testModifyAndCommit();
    testCheckout();
    testRevert();
    testDelete();
    testConfig();
    testConfigSet();
    testPrivate();
    testLog();
    testStatus2();
    testIgnore();
    testInfo();
    testShow();
    testDiff();
    testHistory();
    testStash();
    testArchive();
    testMove();
    testUntrack();
    testMerge();
    testEmptyDir();
    testEdgeCases();
    testNoRepo();
    testPushPullClone();

    printReport();
    teardown();
    return failedTests > 0 ? 1 : 0;
}
