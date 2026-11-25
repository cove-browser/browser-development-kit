/* The profile snapshot and downgrade policy at start (T-PKG-05; configuration.md 3.3;
   profile_versions.h). Each test makes a data directory, writes what a previous engine left in it
   (Chromium's "Last Version" and "Snapshots/<version>/"), starts with that directory and checks
   what start did. The stub reports engine version 1.0.0.0, so "0.9.0.0" is an older milestone and
   "9999.0.0.0" a newer one. POSIX file calls; skipped where the harness cannot give a start its own
   data directory. */
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_support.h"

#define T_PATH 512

static char t_scratch[T_PATH]; /* removed after each test */
static char t_data[T_PATH];    /* <scratch>/Data, the data directory */

static void remove_tree(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                char child[T_PATH];
                snprintf(child, sizeof child, "%s/%s", path, e->d_name);
                remove_tree(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static const char* at(const char* rel) {
    static char buf[4][T_PATH];
    static int next;
    char* out = buf[next++ % 4];
    snprintf(out, T_PATH, "%s/%s", t_data, rel);
    return out;
}

static void put(const char* rel, const char* text) {
    char dir[T_PATH];
    snprintf(dir, sizeof dir, "%s", at(rel));
    for (char* p = dir + strlen(t_data) + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0755);
            *p = '/';
        }
    }
    FILE* f = fopen(at(rel), "w");
    REQUIRE(f != NULL);
    fputs(text, f);
    fclose(f);
}

/* The file's text, or "" when it does not exist. */
static const char* text_of(const char* path) {
    static char buf[256];
    buf[0] = 0;
    FILE* f = fopen(path, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
    }
    return buf;
}

static int exists(const char* path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

/* Makes the scratch directory and data directory; 0 (and the test does nothing) when the harness
   cannot give a start its own data directory. */
static int setup(void) {
    if (!bdk_harness_own_data_directory()) {
        printf("    skipped: the runtime keeps one data directory per process\n");
        return 0;
    }
    static int serial;
    snprintf(t_scratch, sizeof t_scratch, "/tmp/bdk-profile-versions-%ld-%d", (long)getpid(),
             ++serial);
    remove_tree(t_scratch);
    REQUIRE(mkdir(t_scratch, 0700) == 0);
    snprintf(t_data, sizeof t_data, "%s/Data", t_scratch);
    REQUIRE(mkdir(t_data, 0755) == 0);
    return 1;
}

static void teardown(void) {
    remove_tree(t_scratch);
}

/* Starts with the data directory and `runtime` members (NULL for none); returns the status, and the
   error domain and code in `domain` and `code` on failure. */
static bdk_status_t start_in_data(const char* runtime, char* domain, char* code) {
    char extra[256];
    if (runtime)
        snprintf(extra, sizeof extra, "\"runtime\":{%s}", runtime);
    t_config_extra = runtime ? extra : NULL;
    bdk_start_config_t c;
    t_fill_start_config(&c);
    c.user_data_path_override = S(t_data);
    bdk_error_t err = 0;
    bdk_status_t s = bdk_runtime_start(&c, &t_runtime, &err);
    t_config_extra = NULL;
    if (s != BDK_OK) {
        t_runtime = 0;
        REQUIRE(err != 0);
        t_error_text(err, domain, 48, code, 48);
    }
    return s;
}

static void start_ok(const char* runtime) {
    char domain[48] = "", code[48] = "";
    bdk_status_t s = start_in_data(runtime, domain, code);
    if (s != BDK_OK)
        printf("    start failed: %s.%s\n", domain, code);
    REQUIRE(s == BDK_OK);
}

static void test_a_first_start_records_the_engine_version(void) {
    if (!setup())
        return;
    start_ok(NULL);
    CHECK(strcmp(text_of(at("Last Version")), "1.0.0.0") == 0);
    CHECK(!exists(at("Snapshots")));
    t_stop();
    start_ok(NULL); /* the same version again: nothing to do */
    CHECK(!exists(at("Snapshots")));
    teardown();
}

static void test_an_upgrade_to_a_newer_milestone_snapshots_the_profile(void) {
    if (!setup())
        return;
    put("Last Version", "0.9.0.0");
    put("Default/Preferences", "old");
    put("Default/Cache/data_0", "cache");
    put("Local State", "state");
    put("SingletonLock", "lock");
    start_ok(NULL);
    CHECK(strcmp(text_of(at("Snapshots/0.9.0.0/Default/Preferences")), "old") == 0);
    CHECK(strcmp(text_of(at("Snapshots/0.9.0.0/Local State")), "state") == 0);
    CHECK(strcmp(text_of(at("Snapshots/0.9.0.0/Last Version")), "0.9.0.0") == 0);
    CHECK(!exists(at("Snapshots/0.9.0.0/Default/Cache"))); /* caches are rebuilt, not kept */
    CHECK(!exists(at("Snapshots/0.9.0.0/SingletonLock")));
    CHECK(strcmp(text_of(at("Default/Preferences")), "old") == 0); /* the profile itself stays */
    CHECK(strcmp(text_of(at("Last Version")), "1.0.0.0") == 0);
    teardown();
}

static void test_snapshot_on_upgrade_false_takes_none(void) {
    if (!setup())
        return;
    put("Last Version", "0.9.0.0");
    put("Default/Preferences", "old");
    start_ok("\"snapshot_on_upgrade\":false");
    CHECK(!exists(at("Snapshots")));
    CHECK(strcmp(text_of(at("Last Version")), "1.0.0.0") == 0);
    teardown();
}

static void test_snapshots_beyond_three_are_removed_oldest_first(void) {
    if (!setup())
        return;
    put("Snapshots/0.1.0.0/Last Version", "0.1.0.0");
    put("Snapshots/0.2.0.0/Last Version", "0.2.0.0");
    put("Snapshots/0.3.0.0/Last Version", "0.3.0.0");
    put("Last Version", "0.9.0.0");
    start_ok(NULL);
    CHECK(exists(at("Snapshots/0.9.0.0")));
    CHECK(exists(at("Snapshots/0.3.0.0")));
    CHECK(exists(at("Snapshots/0.2.0.0")));
    CHECK(!exists(at("Snapshots/0.1.0.0")));
    teardown();
}

static void test_a_downgrade_with_no_snapshot_is_refused_and_touches_nothing(void) {
    if (!setup())
        return;
    put("Last Version", "9999.0.0.0");
    put("Default/Preferences", "newer");
    char domain[48] = "", code[48] = "";
    CHECK(start_in_data(NULL, domain, code) == BDK_ERR_FAILED);
    CHECK(strcmp(domain, "runtime") == 0);
    CHECK(strcmp(code, "downgradeRefused") == 0);
    CHECK(strcmp(text_of(at("Last Version")), "9999.0.0.0") == 0);
    CHECK(strcmp(text_of(at("Default/Preferences")), "newer") == 0);
    CHECK(start_in_data("\"downgrade\":\"refuse\"", domain, code) == BDK_ERR_FAILED);
    CHECK(strcmp(code, "downgradeRefused") == 0);
    t_start(); /* a refused start leaves nothing behind */
    teardown();
}

static void test_fresh_profile_moves_the_newer_data_aside_untouched(void) {
    if (!setup())
        return;
    put("Last Version", "9999.0.0.0");
    put("Default/Preferences", "newer");
    start_ok("\"downgrade\":\"fresh-profile\"");
    char aside[T_PATH];
    snprintf(aside, sizeof aside, "%s/Data.newer-9999.0.0.0/Default/Preferences", t_scratch);
    CHECK(strcmp(text_of(aside), "newer") == 0);
    snprintf(aside, sizeof aside, "%s/Data.newer-9999.0.0.0/Last Version", t_scratch);
    CHECK(strcmp(text_of(aside), "9999.0.0.0") == 0);
    CHECK(!exists(at("Default")));
    CHECK(strcmp(text_of(at("Last Version")), "1.0.0.0") == 0);
    teardown();
}

static void test_a_downgrade_restores_the_newest_usable_snapshot(void) {
    if (!setup())
        return;
    put("Last Version", "9999.0.0.0");
    put("Default/Preferences", "newer");
    put("Default/Bookmarks", "newer bookmarks");
    put("Snapshots/0.5.0.0/Last Version", "0.5.0.0");
    put("Snapshots/0.5.0.0/Default/Preferences", "older");
    put("Snapshots/0.9.0.0/Last Version", "0.9.0.0");
    put("Snapshots/0.9.0.0/Default/Preferences", "snapshot");
    put("Snapshots/2.0.0.0/Last Version", "2.0.0.0"); /* newer than the engine: not usable */
    put("Snapshots/2.0.0.0/Default/Preferences", "too new");
    put("Snapshots/0.95.0.0/Default/Preferences", "no Last Version: not a snapshot");
    start_ok(NULL); /* the policy only decides when there is no snapshot */
    CHECK(strcmp(text_of(at("Default/Preferences")), "snapshot") == 0);
    CHECK(!exists(at("Default/Bookmarks")));
    CHECK(!exists(at("Snapshots/0.9.0.0")));
    CHECK(exists(at("Snapshots/0.5.0.0")));
    CHECK(exists(at("Snapshots/2.0.0.0")));
    CHECK(!exists(at(".bdk-discard")));
    CHECK(strcmp(text_of(at("Last Version")), "1.0.0.0") == 0);
    teardown();
}

static void test_a_downgrade_within_a_milestone_is_allowed(void) {
    if (!setup())
        return;
    put("Last Version", "1.5.0.0");
    put("Default/Preferences", "same milestone");
    start_ok(NULL);
    CHECK(strcmp(text_of(at("Default/Preferences")), "same milestone") == 0);
    CHECK(strcmp(text_of(at("Last Version")), "1.0.0.0") == 0);
    teardown();
}

static void test_an_unreadable_last_version_counts_as_a_first_start(void) {
    if (!setup())
        return;
    put("Last Version", "not a version");
    start_ok(NULL);
    CHECK(strcmp(text_of(at("Last Version")), "1.0.0.0") == 0);
    CHECK(!exists(at("Snapshots")));
    teardown();
}

static void test_an_invalid_policy_fails_start(void) {
    if (!setup())
        return;
    const char* bad[] = {"\"downgrade\":\"wipe\"", "\"downgrade\":1",
                         "\"snapshot_on_upgrade\":\"yes\""};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        char domain[48] = "", code[48] = "";
        CHECK(start_in_data(bad[i], domain, code) == BDK_ERR_FAILED);
        CHECK(strcmp(domain, "config") == 0);
        CHECK(strcmp(code, "resolvedInvalid") == 0);
    }
    CHECK(
        !exists(at("Last Version"))); /* the configuration is checked before the data is touched */
    teardown();
}

const t_test_t t_profile_version_tests[] = {
    {"profile_versions.first_start_records_the_version",
     test_a_first_start_records_the_engine_version},
    {"profile_versions.upgrade_snapshots",
     test_an_upgrade_to_a_newer_milestone_snapshots_the_profile},
    {"profile_versions.snapshot_on_upgrade_false", test_snapshot_on_upgrade_false_takes_none},
    {"profile_versions.retention", test_snapshots_beyond_three_are_removed_oldest_first},
    {"profile_versions.downgrade_refused",
     test_a_downgrade_with_no_snapshot_is_refused_and_touches_nothing},
    {"profile_versions.downgrade_fresh_profile",
     test_fresh_profile_moves_the_newer_data_aside_untouched},
    {"profile_versions.downgrade_restores_snapshot",
     test_a_downgrade_restores_the_newest_usable_snapshot},
    {"profile_versions.minor_downgrade_allowed", test_a_downgrade_within_a_milestone_is_allowed},
    {"profile_versions.unreadable_last_version",
     test_an_unreadable_last_version_counts_as_a_first_start},
    {"profile_versions.invalid_policy", test_an_invalid_policy_fails_start},
    {NULL, NULL},
};
