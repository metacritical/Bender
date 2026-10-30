/* Embedded by callgraph.ben into generated native.c. Keep the report free
 * of Bender allocations and object traversal: it is an exit-time snapshot. */
#if BENDER_NATIVE_TELEMETRY
typedef struct {
    int id;
    const char *name;
    uint64_t entries, ops, exits, chains;
} BenderWorkRow;
static const char *bender_work_path;
void native_work_profile_path(const char *path) { bender_work_path = path; }

static void bender_work_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\' || c == '"') { fputc('\\', f); fputc(c, f); }
        else if (c < 32) fprintf(f, "\\u%04x", (unsigned)c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void bender_work_report(void) {
    if (!bender_work_path) return;
    FILE *f = strcmp(bender_work_path, "-") ? fopen(bender_work_path, "w") : stderr;
    if (!f) { fprintf(stderr, "work-profile: cannot open %s\n", bender_work_path); return; }
    BenderWorkRow rows[] = {
        BENDER_WORK_ROWS
        {0, NULL, 0, 0, 0, 0}
    };
    size_t n = sizeof(rows) / sizeof(rows[0]) - 1;
    uint64_t ops = 0, entries = 0, exits = 0, chains = 0;
    for (size_t i = 0; i < n; i++) {
        ops += rows[i].ops; entries += rows[i].entries;
        exits += rows[i].exits; chains += rows[i].chains;
    }
    fprintf(f, "{\n  \"schema\":1,\n  \"deterministic\":{\n");
#define WORK_FIELD(key, val, tail) fprintf(f, "    \"" key "\":%" PRIu64 tail, (uint64_t)(val))
    WORK_FIELD("interpreted_dispatches", bender_native_stat(BENDER_NATIVE_STAT_INTERP_DISPATCHES), ",\n");
    WORK_FIELD("interpreted_calls", bender_native_stat(BENDER_NATIVE_STAT_INTERP_CALLS), ",\n");
    WORK_FIELD("chained_calls", bender_native_stat(BENDER_NATIVE_STAT_CONTINUATIONS), ",\n");
    WORK_FIELD("native_ops", ops, ",\n");
    WORK_FIELD("native_entries", entries, ",\n");
    WORK_FIELD("vm_exits", bender_native_stat(BENDER_NATIVE_STAT_VM_EXITS), ",\n");
    WORK_FIELD("native_boundary_exits", exits, ",\n");
    WORK_FIELD("continuation_refusals", bender_native_stat(BENDER_NATIVE_STAT_CONTINUATION_REFUSALS), "\n  },\n");
    fprintf(f, "  \"advisory_gc\":{\n");
    WORK_FIELD("allocations", bender_native_stat(BENDER_NATIVE_STAT_ALLOCATIONS), ",\n");
    WORK_FIELD("requested_bytes", bender_native_stat(BENDER_NATIVE_STAT_ALLOC_BYTES), ",\n");
    WORK_FIELD("barrier_calls", bender_native_stat(BENDER_NATIVE_STAT_BARRIER_CALLS), ",\n");
    WORK_FIELD("major_cycles", bender_native_stat(BENDER_NATIVE_STAT_GC_MAJOR), ",\n");
    WORK_FIELD("minor_cycles", bender_native_stat(BENDER_NATIVE_STAT_GC_MINOR), ",\n");
    WORK_FIELD("fiber_minor_cycles", bender_native_stat(BENDER_NATIVE_STAT_GC_FIBER_MINOR), ",\n");
    WORK_FIELD("process_minor_cycles", bender_native_stat(BENDER_NATIVE_STAT_GC_PROCESS_MINOR), "\n  },\n");
#undef WORK_FIELD
    for (int field = 0; field < 2; field++) {
        fprintf(f, field ? "  \"top_vm_exits\":[" : "  \"top_native_ops\":[");
        unsigned char picked[sizeof(rows) / sizeof(rows[0])];
        memset(picked, 0, sizeof(picked));
        size_t printed = 0;
        for (size_t k = 0; k < n && k < 10; k++) {
            size_t best = n;
            uint64_t score = 0;
            for (size_t i = 0; i < n; i++) {
                uint64_t v = field ? rows[i].exits : rows[i].ops;
                if (!picked[i] && v && (best == n || v > score ||
                        (v == score && rows[i].id < rows[best].id))) {
                    best = i; score = v;
                }
            }
            if (best == n) break;
            picked[best] = 1;
            fprintf(f, "%s\n    {\"id\":%d,\"name\":", printed++ ? "," : "", rows[best].id);
            bender_work_json_string(f, rows[best].name);
            fprintf(f, ",\"entries\":%" PRIu64 ",\"ops\":%" PRIu64
                    ",\"vm_exits\":%" PRIu64 ",\"chained_calls\":%" PRIu64 "}",
                    rows[best].entries, rows[best].ops, rows[best].exits, rows[best].chains);
        }
        fprintf(f, "\n  ]%s\n", field ? "" : ",");
    }
    fprintf(f, "}\n");
    if (f != stderr) fclose(f);
    fprintf(stderr, "work-profile: interpreted=%" PRIu64 " native-ops=%" PRIu64
            " vm-exits=%" PRIu64 " chained=%" PRIu64 " (%s)\n",
            bender_native_stat(BENDER_NATIVE_STAT_INTERP_DISPATCHES), ops,
            bender_native_stat(BENDER_NATIVE_STAT_VM_EXITS), chains, bender_work_path);
}
#endif
