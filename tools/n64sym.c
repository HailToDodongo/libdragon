#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#define STBDS_NO_SHORT_NAMES
#define STB_DS_IMPLEMENTATION
#include "common/stb_ds.h"

#include "common/subprocess.h"
#include "common/polyfill.h"

bool flag_verbose = false;
char *n64_inst = NULL;

// Printf if verbose
void verbose(const char *fmt, ...) {
    if (flag_verbose) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
}

void usage(const char *progname)
{
    fprintf(stderr, "%s - Prepare symbol table for N64 ROMs\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: %s [flags] <program.elf> [<program.sym>]\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "Command-line flags:\n");
    fprintf(stderr, "   -v/--verbose          Verbose output\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "This program requires a libdragon toolchain installed in $N64_INST.\n");
}

char *stringtable = NULL;

int stringtable_add(char *word)
{
    if (stringtable) {
        char *found = strstr(stringtable, word);
        if (found) {
            return found - stringtable;
        }
    }

    // Append the word (without the trailing \0)
    int word_len = strlen(word);
    int idx = stbds_arraddnindex(stringtable, word_len);
    memcpy(stringtable + idx, word, word_len);
    return idx;
}


void w8(FILE *f, uint8_t v)  { fputc(v, f); }
void w16(FILE *f, uint16_t v) { w8(f, v >> 8);   w8(f, v & 0xff); }
void w32(FILE *f, uint32_t v) { w16(f, v >> 16); w16(f, v & 0xffff); }
int w32_placeholder(FILE *f) { int pos = ftell(f); w32(f, 0); return pos; }
void w32_at(FILE *f, int pos, uint32_t v)
{
    int cur = ftell(f);
    fseek(f, pos, SEEK_SET);
    w32(f, v);
    fseek(f, cur, SEEK_SET);
}
void walign(FILE *f, int align) { 
    int pos = ftell(f);
    while (pos++ % align) w8(f, 0);
}

struct symtable_s {
    uint32_t uuid;
    uint32_t addr;
    char *func;
    char *file;
    int line;

    int func_sidx;
    int file_sidx;

    int func_offset;
} *symtable = NULL;

void symbol_add(const char *elf, uint32_t addr, bool save_line)
{
    // We keep one addr2line process open for the last ELF file we processed.
    // This allows to convert multiple symbols very fast, avoiding spawning a
    // new process for each symbol.
    // NOTE: we cannot use popen() here because on some platforms (eg. glibc)
    // it only allows a single direction pipe, and we need both directions.
    // So we rely on the subprocess library for this.
    static char *addrbin = NULL;
    static struct subprocess_s subp;
    static FILE *addr2line_w = NULL, *addr2line_r = NULL;
    static const char *cur_elf = NULL;
    static char *line_buf = NULL;
    static size_t line_buf_size = 0;

    // Check if this is a new ELF file (or it's the first time we run this function)
    if (!cur_elf || strcmp(cur_elf, elf)) {
        if (cur_elf) {
            subprocess_terminate(&subp);
            cur_elf = NULL; addr2line_r = addr2line_w = NULL;
        }
        if (!addrbin)
            asprintf(&addrbin, "%s/bin/mips64-elf-addr2line", n64_inst);

        const char *cmd_addr[] = {
            addrbin, 
            "--addresses", "--inlines", "--functions", "--demangle",
            "--exe", elf,
            NULL
        };
        if (subprocess_create(cmd_addr, subprocess_option_no_window, &subp) != 0) {
            fprintf(stderr, "Error: cannot run: %s\n", addrbin);
            exit(1);
        }
        addr2line_w = subprocess_stdin(&subp);
        addr2line_r = subprocess_stdout(&subp);
        cur_elf = elf;
    }

    // Send the address to addr2line and fetch back the symbol and the function name
    // Since we activated the "--inlines" option, addr2line produces an unknown number
    // of output lines. This is a problem with pipes, as we don't know when to stop.
    // Thus, we always add a dummy second address (0x0) so that we stop when we see the
    // reply for it
    fprintf(addr2line_w, "%08x\n0\n", addr);
    fflush(addr2line_w);

    // First line is the address. It's just an echo, so ignore it.
    int n = getline(&line_buf, &line_buf_size, addr2line_r);
    assert(n >= 2 && strncmp(line_buf, "0x", 2) == 0);

    // Add one symbol for each inlined function
    bool is_inline = false;
    while (1) {
        // First line is the function name. If instead it's the dummy 0x0 address,
        // it means that we're done.
        int n = getline(&line_buf, &line_buf_size, addr2line_r);
        if (strncmp(line_buf, "0x00000000", 10) == 0) break;
        char *func = strndup(line_buf, n-1);

        // Second line is the file name and line number
        getline(&line_buf, &line_buf_size, addr2line_r);
        char *colon = strrchr(line_buf, ':');
        char *file = strndup(line_buf, colon - line_buf);
        int line = atoi(colon + 1);

        // Add the callsite to the list
        stbds_arrput(symtable, ((struct symtable_s) {
            .uuid = stbds_arrlen(symtable),
            .addr = addr | (is_inline ? 0x2 : 0),
            .func = func,
            .file = file,
            .line = save_line ? line : 0,
        }));

        is_inline = true;
    }

    // Read and skip the two remaining lines (function and file position)
    // that refers to the dummy 0x0 address
    getline(&line_buf, &line_buf_size, addr2line_r);
    getline(&line_buf, &line_buf_size, addr2line_r);
}

void elf_find_functions(const char *elf)
{
    // Run mips64-elf-nm to extract the symbol table
    char *cmd;
    asprintf(&cmd, "%s/bin/mips64-elf-nm -n %s", n64_inst, elf);

    verbose("Running: %s\n", cmd);
    FILE *nm = popen(cmd, "r");
    if (!nm) {
        fprintf(stderr, "Error: cannot run: %s\n", cmd);
        exit(1);
    }

    // Parse the file line by line and select the lines whose second word is "T"
    char *line = NULL; size_t line_size = 0;
    while (getline(&line, &line_size, nm) != -1) {
        char name[1024] = {0}; char type; uint64_t addr;
        if (sscanf(line, "%llx %c %s", &addr, &type, name) == 3) {
            if (type == 'T') {
                // Don't save the line number associated to function symbols. These
                // are the "generic" symbols which the backtracing code will fallback
                // to if it cannot find a more specific symbol, so the line number
                // has to be 0 to mean "no known line number"
                symbol_add(elf, addr, false);
            }
        }
    }
    pclose(nm);
    free(cmd); cmd = NULL;
}

void elf_find_callsites(const char *elf)
{
    // Start objdump to parse the disassembly of the ELF file
    char *cmd = NULL;
    asprintf(&cmd, "%s/bin/mips64-elf-objdump -d %s", n64_inst, elf);
    verbose("Running: %s\n", cmd);
    FILE *disasm = popen(cmd, "r");
    if (!disasm) {
        fprintf(stderr, "Error: cannot run: %s\n", cmd);
        exit(1);
    }

    // Start addr2line, to convert callsites addresses as we find them

    // Parse the disassembly
    char *line = NULL; size_t line_size = 0;
    while (getline(&line, &line_size, disasm) != -1) {
        // Find the callsites
        if (strstr(line, "\tjal\t") || strstr(line, "\tjalr\t")) {
            uint32_t addr = strtoul(line, NULL, 16);
            symbol_add(elf, addr, true);
        }
    }
    free(line);
    pclose(disasm);
}

void compact_filenames(void)
{
    while (1) {
        char *prefix = NULL; int prefix_len = 0;

        for (int i=0; i<stbds_arrlen(symtable); i++) {
            struct symtable_s *s = &symtable[i];
            if (!s->file) continue;
            if (s->file[0] != '/' && s->file[1] != ':') continue;

            if (!prefix) {
                prefix = s->file;
                prefix_len = 0;
                if (prefix[prefix_len] == '/' || prefix[prefix_len] == '\\')
                    prefix_len++;
                while (prefix[prefix_len] && prefix[prefix_len] != '/' && prefix[prefix_len] != '\\')
                    prefix_len++;
                verbose("Initial prefix: %.*s\n", prefix_len, prefix);
                if (prefix[prefix_len] == 0)
                    return;
            } else {
                if (strncmp(prefix, s->file, prefix_len) != 0) {
                    verbose("Prefix mismatch: %.*s vs %s\n", prefix_len, prefix, s->file);
                    return;
                }
            }
        }

        verbose("Removing common prefix: %.*s\n", prefix_len, prefix);

        // The prefix is common to all files, remove it
        for (int i=0; i<stbds_arrlen(symtable); i++) {
            struct symtable_s *s = &symtable[i];
            if (!s->file) continue;
            if (s->file[0] != '/' && s->file[1] != ':') continue;
            s->file += prefix_len;
        }
        break;
    }
}

void compute_function_offsets(void)
{
    uint32_t func_addr = 0;
    for (int i=0; i<stbds_arrlen(symtable); i++) {
        struct symtable_s *s = &symtable[i];
        if (s->line == 0) {
            func_addr = s->addr;
        } else {
            s->func_offset = s->addr - func_addr;
        }
    }
}

int symtable_sort_by_addr(const void *a, const void *b)
{
    const struct symtable_s *sa = a;
    const struct symtable_s *sb = b;
    // In case the address match, it means that there are multiple
    // inlines at this address. Sort by insertion order (aka stable sort)
    // so that we preserve the inline order.
    if (sa->addr != sb->addr)
        return sa->addr - sb->addr;
    return sa->uuid - sb->uuid;
}

int symtable_sort_by_func(const void *a, const void *b)
{
    const struct symtable_s *sa = a;
    const struct symtable_s *sb = b;
    int sa_len = sa->func ? strlen(sa->func) : 0;
    int sb_len = sb->func ? strlen(sb->func) : 0;
    return sb_len - sa_len;
}

void process(const char *infn, const char *outfn)
{
    verbose("Processing: %s -> %s\n", infn, outfn);

    elf_find_functions(infn);
    verbose("Found %d functions\n", stbds_arrlen(symtable));

    elf_find_callsites(infn);
    verbose("Found %d callsites\n", stbds_arrlen(symtable));

    // Compact the file names to avoid common prefixes
    // FIXME: we need to improve this to handle multiple common prefixes
    // eg: /home/foo vs /opt/n64/include
    //compact_filenames();

    // Sort the symbole table by symbol length. We want longer symbols
    // to go in first, so that shorter symbols can be found as substrings.
    // We sort by function name rather than file name, because we expect
    // substrings to match more in functions.
    qsort(symtable, stbds_arrlen(symtable), sizeof(struct symtable_s), symtable_sort_by_func);

    // Go through the symbol table and build the string table
    for (int i=0; i < stbds_arrlen(symtable); i++) {
        struct symtable_s *sym = &symtable[i];
        if (sym->func)
            sym->func_sidx = stringtable_add(sym->func);
        else
            sym->func_sidx = -1;
        if (sym->file)
            sym->file_sidx = stringtable_add(sym->file);
        else
            sym->file_sidx = -1;
    }

    // Sort the symbol table by address
    qsort(symtable, stbds_arrlen(symtable), sizeof(struct symtable_s), symtable_sort_by_addr);

    // Compute the function start offsets
    compute_function_offsets();

    // Write the symbol table to file
    verbose("Writing %s\n", outfn);
    FILE *out = fopen(outfn, "wb");
    if (!out) {
        fprintf(stderr, "Cannot create file: symtable.bin\n");
        exit(1);
    }

    fwrite("SYMT", 4, 1, out);
    w32(out, 1); // Version
    int addrtable_off = w32_placeholder(out);
    w32(out, stbds_arrlen(symtable));
    int symtable_off = w32_placeholder(out);
    w32(out, stbds_arrlen(symtable));
    int stringtable_off = w32_placeholder(out);
    w32(out, stbds_arrlen(stringtable));

    walign(out, 16);
    w32_at(out, addrtable_off, ftell(out));
    for (int i=0; i < stbds_arrlen(symtable); i++) {
        struct symtable_s *sym = &symtable[i];
        w32(out, sym->addr | (sym->line == 0 ? 1 : 0));
    }

    walign(out, 16);
    w32_at(out, symtable_off, ftell(out));
    for (int i=0; i < stbds_arrlen(symtable); i++) {
        struct symtable_s *sym = &symtable[i];
        w16(out, sym->func_sidx);
        w16(out, strlen(sym->func));
        w16(out, sym->file_sidx);
        w16(out, strlen(sym->file));
        w16(out, sym->line);
        w16(out, sym->func_offset);
    }

    walign(out, 16);
    w32_at(out, stringtable_off, ftell(out));
    fwrite(stringtable, stbds_arrlen(stringtable), 1, out);
    fclose(out);
}

// Change filename extension
char *change_ext(const char *fn, const char *ext)
{
    char *out = strdup(fn);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    strcat(out, ext);
    return out;
}

int main(int argc, char *argv[])
{
    const char *outfn = NULL;

    int i;
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            flag_verbose = true;
        } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            if (++i == argc) {
                fprintf(stderr, "missing argument for %s\n", argv[i-1]);
                return 1;
            }
            outfn = argv[i];
        } else {
            fprintf(stderr, "invalid flag: %s\n", argv[i]);
            return 1;
        }
    }

    if (i == argc) {
        fprintf(stderr, "missing input filename\n");
        return 1;
    }

    if (!n64_inst) {
        n64_inst = getenv("N64_INST");
        if (!n64_inst) {
            fprintf(stderr, "Error: N64_INST environment variable not set.\n");
            return 1;
        }
        // Remove the trailing backslash if any. On some system, running
        // popen with a path containing double backslashes will fail, so
        // we normalize it here.
        n64_inst = strdup(n64_inst);
        int n = strlen(n64_inst);
        if (n64_inst[n-1] == '/' || n64_inst[n-1] == '\\')
            n64_inst[n-1] = 0;
    }

    const char *infn = argv[i];
    if (i < argc-1)
        outfn = argv[i+1];
    else
        outfn = change_ext(infn, ".sym");

    // Check that infn exists and is readable
    FILE *in = fopen(infn, "rb");
    if (!in) {
        fprintf(stderr, "Error: cannot open file: %s\n", infn);
        return 1;
    }
    fclose(in);

    process(infn, outfn);
    return 0;
}

