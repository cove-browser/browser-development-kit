/* The profile snapshot and downgrade policy on Chromium (T-PKG-05; configuration.md 3.3;
   profile_versions.h). The policy applies to the data directory before Chromium opens it, and the
   harness starts Chromium once, before the first test. So each scenario has a launch of its own
   (scripts/test-engine.sh runs every engine_launch_* scenario alone): bdk_engine_launch_prepare,
   which the harness calls before that first start, writes what another engine milestone left in
   the data directory (Chromium's "Last Version" and "Snapshots/<version>/"), makes any start that
   must fail before Chromium starts, and chooses the policy of the start that opens Chromium. The
   scenario then checks what that start did, and that a profile opens there. In a run of the whole
   suite a scenario finds nothing prepared and says so. Built only into the engine conformance app
   (BDK_HARNESS_ENGINE_TESTS). POSIX file calls. */
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_support.h"

/* From harness_engine.mm: the directory Chromium opens. */
const char* bdk_harness_engine_data_dir(void);

#define T_PATH 512

static char t_data[T_PATH];     /* the launch's data directory */
static const char* g_prepared;  /* the scenario this launch was prepared for, or NULL */
static char g_refused_code[48]; /* the error code of a start made before Chromium started */

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

static const char* in_dir(const char* dir, const char* rel) {
    static char buf[4][T_PATH];
    static int next;
    char* out = buf[next++ % 4];
    snprintf(out, T_PATH, "%s/%s", dir, rel);
    return out;
}

static const char* at(const char* rel) {
    return in_dir(t_data, rel);
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
    /* Runs before the suite (bdk_engine_launch_prepare), so no REQUIRE: a file not written fails
       the scenario's checks. */
    FILE* f = fopen(at(rel), "w");
    if (!f)
        return;
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

/* The milestone of a version string, or -1. */
static long milestone_of(const char* version) {
    char* end = NULL;
    long m = strtol(version, &end, 10);
    return end && end != version && *end == '.' ? m : -1;
}

/* Whether this launch was prepared for the scenario `name`. */
static int prepared(const char* name) {
    if (!g_prepared || strcmp(g_prepared, name) != 0) {
        printf("    skipped: needs a launch of its own (--run %s)\n", name);
        return 0;
    }
    return 1;
}

/* A start on the data directory with `runtime` members, before Chromium starts; returns its status
   and leaves its error code in g_refused_code. */
static bdk_status_t start_before_chromium(const char* runtime) {
    char extra[256];
    snprintf(extra, sizeof extra, "\"runtime\":{%s}", runtime);
    t_config_extra = extra;
    bdk_start_config_t c;
    t_fill_start_config(&c);
    t_config_extra = NULL;
    bdk_runtime_t rt = 0;
    bdk_error_t err = 0;
    bdk_status_t s = bdk_runtime_start(&c, &rt, &err);
    if (s != BDK_OK && err) {
        char domain[48];
        t_error_text(err, domain, sizeof domain, g_refused_code, sizeof g_refused_code);
    }
    return s;
}

/* Chromium runs on the directory: a profile opens there, and the engine's version is recorded. */
static void check_chromium_uses_the_directory(void) {
    t_start_frozen();
    char id[64];
    t_create_profile("Versioned", id, sizeof id);
    bdk_profile_t p = t_open_profile(id);
    CHECK(p != 0);
    CHECK(exists(at(id)));
    CHECK(milestone_of(text_of(at("Last Version"))) > 157);
}

/* ---- An upgrade: snapshotted ---- */

#define UPGRADE "engine_launch_an_upgrade_snapshots_the_profile_and_records_the_version"

/* The runtime members of the start that opens Chromium (the harness's first start). */
static char g_launch_policy[128];

static void prepare_upgrade(void) {
    put("Last Version", "157.0.7000.0");
    put("Earlier/marker", "157");
    put("Earlier/Cache/data_0", "cached");
    snprintf(g_launch_policy, sizeof g_launch_policy, "\"snapshot_on_upgrade\":true");
}

static void test_launch_an_upgrade_snapshots_the_profile_and_records_the_version(void) {
    if (!prepared(UPGRADE))
        return;
    CHECK(strcmp(text_of(at("Snapshots/157.0.7000.0/Last Version")), "157.0.7000.0") == 0);
    CHECK(strcmp(text_of(at("Snapshots/157.0.7000.0/Earlier/marker")), "157") == 0);
    CHECK(!exists(at("Snapshots/157.0.7000.0/Earlier/Cache")));
    CHECK(strcmp(text_of(at("Earlier/marker")), "157") == 0);
    check_chromium_uses_the_directory();
}

/* ---- A downgrade with a snapshot: restored ---- */

#define RESTORE "engine_launch_a_downgrade_restores_the_newest_snapshot"

static void prepare_restore(void) {
    put("Last Version", "9999.0.0.0");
    put("newer.txt", "from 9999");
    put("Snapshots/100.0.0.0/Last Version", "100.0.0.0");
    put("Snapshots/100.0.0.0/restored.txt", "from 100");
    put("Snapshots/9000.0.0.0/Last Version", "9000.0.0.0");
    snprintf(g_launch_policy, sizeof g_launch_policy, "\"downgrade\":\"refuse\"");
}

static void test_launch_a_downgrade_restores_the_newest_snapshot(void) {
    if (!prepared(RESTORE))
        return;
    CHECK(strcmp(text_of(at("restored.txt")), "from 100") == 0);
    CHECK(!exists(at("newer.txt")));
    check_chromium_uses_the_directory();
}

/* ---- A downgrade with no snapshot: refused, or a fresh profile ---- */

#define FRESH "engine_launch_a_downgrade_with_no_snapshot_is_refused_then_starts_fresh"

static int g_refused_left_it; /* the refused start left the directory as it was */

static void prepare_fresh(void) {
    put("Last Version", "9999.0.0.0");
    put("newer.txt", "from 9999");
    g_refused_left_it = start_before_chromium("\"downgrade\":\"refuse\"") != BDK_OK &&
                        strcmp(text_of(at("Last Version")), "9999.0.0.0") == 0 &&
                        strcmp(text_of(at("newer.txt")), "from 9999") == 0;
    snprintf(g_launch_policy, sizeof g_launch_policy, "\"downgrade\":\"fresh-profile\"");
}

static void test_launch_a_downgrade_with_no_snapshot_is_refused_then_starts_fresh(void) {
    if (!prepared(FRESH))
        return;
    CHECK(strcmp(g_refused_code, "downgradeRefused") == 0);
    CHECK(g_refused_left_it);
    char aside[T_PATH];
    snprintf(aside, sizeof aside, "%s.newer-9999.0.0.0", t_data);
    CHECK(strcmp(text_of(in_dir(aside, "newer.txt")), "from 9999") == 0);
    CHECK(strcmp(text_of(in_dir(aside, "Last Version")), "9999.0.0.0") == 0);
    CHECK(!exists(at("newer.txt")));
    check_chromium_uses_the_directory();
    remove_tree(aside);
}

void bdk_engine_launch_prepare(const char* name) {
    static const struct {
        const char* name;
        void (*prepare)(void);
    } kLaunches[] = {
        {UPGRADE, prepare_upgrade}, {RESTORE, prepare_restore}, {FRESH, prepare_fresh}};
    for (size_t i = 0; i < sizeof kLaunches / sizeof kLaunches[0]; ++i) {
        if (strcmp(name, kLaunches[i].name) != 0)
            continue;
        snprintf(t_data, sizeof t_data, "%s", bdk_harness_engine_data_dir());
        g_prepared = kLaunches[i].name;
        kLaunches[i].prepare();
        /* The policy of the harness's first start, which opens Chromium on the directory. */
        static char extra[160];
        snprintf(extra, sizeof extra, "\"runtime\":{%s}", g_launch_policy);
        t_config_extra = extra;
        return;
    }
}

const t_test_t t_engine_profile_version_tests[] = {
    {UPGRADE, test_launch_an_upgrade_snapshots_the_profile_and_records_the_version},
    {RESTORE, test_launch_a_downgrade_restores_the_newest_snapshot},
    {FRESH, test_launch_a_downgrade_with_no_snapshot_is_refused_then_starts_fresh},
    {NULL, NULL},
};
