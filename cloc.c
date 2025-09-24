#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <libgen.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/limits.h>

#define USAGE "USAGE: clocc [directory]"
#define ERROR "\033[31mERROR:\033[0m "

void exit_err(const char *err) {
    fprintf(stderr, ERROR "%s.\n", err);
    exit(1);
}

typedef struct Lang {
    const char *name;
    const char **ex;
    const char *line_cmt;
    const char *block_cmt_a;
    const char *block_cmt_b;
    
    uint64_t code;
    uint64_t empty;
    uint64_t comment;
    uint64_t total;
    uint64_t size;
} Lang;

static const char *c_ex[] = {"c", NULL};
static const char *cpp_ex[] = {"cpp", NULL};
static const char *h_ex[] = {"h", "hpp", NULL};
static const char *rs_ex[] = {"rs", NULL};
static const char *py_ex[] = {"py", NULL};

static Lang langs[] = {
    {
        .name = "C",
        .ex = c_ex,
        .line_cmt = "//",
        .block_cmt_a = "/*",
        .block_cmt_b = "*/",
    },
    {
        .name = "C++",
        .ex = cpp_ex,
        .line_cmt = "//",
        .block_cmt_a = "/*",
        .block_cmt_b = "*/",
    },
    {
        .name = "Header",
        .ex = h_ex,
        .line_cmt = "//",
        .block_cmt_a = "/*",
        .block_cmt_b = "*/",
    },
    {
        .name = "Rust",
        .ex = rs_ex,
        .line_cmt = "//",
        .block_cmt_a = "/*",
        .block_cmt_b = "*/",
    },
    {
        .name = "Python",
        .ex = py_ex,
        .line_cmt = "#",
    },
    {0}
};

Lang *get_lang(const char *filename) {
    const char *ex = NULL;
    { // find extension in path
        const char *iter = filename + 1;
        while (*iter) {
            if (*iter == '.')
                ex = iter+1;
            ++iter;
        }
        
        if (ex == NULL || *ex == 0) return NULL;
    }
    
    // search for extension in langs
    for (Lang *lang = langs; lang->name != NULL; ++lang) {
        for (const char **lang_ex = lang->ex; *lang_ex != NULL; ++lang_ex) {
            if (strcmp(ex, *lang_ex) == 0)
                return lang;
        }
    }
    
    return NULL;
}

#define MAP_OVERFLOW 16
static void count_file(int dirfd, const char *filename) {
    // find language
    Lang *lang;
    {
        lang = get_lang(filename);
        if (lang == NULL) return;
    }
    
    // map file
    int fd;
    char *file;
    uint64_t fsize;
    {
        fd = openat(dirfd, filename, O_RDONLY);
        if (fd < 0) exit_err("could not open file");
        
        struct stat stats;
        if (fstat(fd, &stats) != 0) exit_err("could not stat file");
        fsize = (uint64_t)stats.st_size;
        // we allocate a bit past the end - this allows us to skip some bounds checks. 
        file = mmap(NULL, fsize + MAP_OVERFLOW, PROT_READ, MAP_PRIVATE, fd, 0);
        if (file == MAP_FAILED) exit_err("could not map file");
        posix_madvise(file, fsize, POSIX_MADV_SEQUENTIAL | POSIX_MADV_WILLNEED);
    }
    
    // lil precomputation
    uint64_t line_cmt_len    = lang->line_cmt    ? strlen(lang->line_cmt   ) : 0; 
    uint64_t block_cmt_a_len = lang->block_cmt_a ? strlen(lang->block_cmt_a) : 0; 
    uint64_t block_cmt_b_len = lang->block_cmt_b ? strlen(lang->block_cmt_b) : 0; 
    
    // count file
    {
        lang->size += fsize;
        
        char *f = file;
        char *end = f + fsize;
        bool empty = true;
        bool code = false;
        bool comment = false;
        while (f < end) {
            char c = *f;
            
            if (c == '\n') {
                if (empty)   lang->empty++;
                if (comment && !code) lang->comment++;
                if (code)    lang->code++;
                lang->total++;
                ++f;
                empty = true;
                code = false;
                comment = false;
            } else if (line_cmt_len != 0 && strncmp(f, lang->line_cmt, line_cmt_len) == 0) {
                comment = true;
                empty = false;
                while (f < end && *f != '\n')
                    ++f;
            } else if (block_cmt_a_len != 0 && strncmp(f, lang->block_cmt_a, block_cmt_a_len) == 0) {
                comment = true;
                empty = false;
                while (f < end && *f != '\n')
                    ++f;
                
                if (empty)   lang->empty++;
                if (comment && !code) lang->comment++;
                if (code)    lang->code++;
                lang->total++;
                ++f;
                
                while (f < end) {
                    c = *f;
                    
                    if (c == '\n') {
                        lang->comment++;
                        lang->total++;
                    } else if (strncmp(f, lang->block_cmt_b, block_cmt_b_len) == 0) {
                        empty = false;
                        code = false;
                        comment = true;
                        ++f;
                        break;
                    }                    
                    ++f;
                }
            } else if (c != ' ' && c != '\t') {
                empty = false;
                code = true;
                ++f;
            } else {
                ++f;
            }
        }
        
        if (fsize != 0 && *(end-1) != '\n') {
            if (empty)   lang->empty++;
            if (comment && !code) lang->comment++;
            if (code)    lang->code++;
            lang->total++;
        }
    }
    
    // unmap file
    munmap(file, fsize + MAP_OVERFLOW);
    close(fd);
}

static int filter_dir(const struct dirent *entry) {
    return entry->d_type == DT_DIR
        && entry->d_name[0] != '.'
        && strcmp(entry->d_name, "target") != 0
        && strcmp(entry->d_name, "build") != 0;
}
static int filter_file(const struct dirent *entry) {
    return entry->d_type == DT_REG
        && entry->d_name[0] != '.';
}

static void count_dir_inner(int parent_dirfd, const char *dir) {
    struct dirent **entries;
    int dirfd = openat(parent_dirfd, dir, O_RDONLY); 
    
    {// iter files
        int filenum = scandirat(parent_dirfd, dir, &entries, filter_file, NULL);
        if (filenum < 0)
            exit_err("invalid filepath"); // TODO better error
            
        for (int file = 0; file < filenum; ++file) {
            struct dirent* entry = entries[file];
            count_file(dirfd, entry->d_name);
        }
        
        free(entries);
    }
    
    {// iter dirs
        int dirnum = scandirat(parent_dirfd, dir, &entries, filter_dir, NULL);
        if (dirnum < 0)
            exit_err("invalid dirpath"); // TODO better error
            
        for (int diri = 0; diri < dirnum; ++diri) {
            struct dirent* entry = entries[diri];
            count_dir_inner(dirfd, entry->d_name);
        }
        
        free(entries);
    }
    
    close(dirfd);
}

static void count_dir(const char *dir) {
    struct dirent **entries;
    int dirfd = open(dir, O_RDONLY); 
    
    {// iter files
        int filenum = scandir(dir, &entries, filter_file, NULL);
        if (filenum < 0)
            exit_err("invalid filepath"); // TODO better error
            
        for (int file = 0; file < filenum; ++file) {
            struct dirent* entry = entries[file];
            count_file(dirfd, entry->d_name);
        }
        
        free(entries);
    }
    
    {// iter dirs
        int dirnum = scandir(dir, &entries, filter_dir, NULL);
        if (dirnum < 0)
            exit_err("invalid dirpath"); // TODO better error
            
        for (int diri = 0; diri < dirnum; ++diri) {
            struct dirent* entry = entries[diri];
            count_dir_inner(dirfd, entry->d_name);
        }
        
        free(entries);
    }
    
    close(dirfd);
}

int main(int argc, char *argv[]) {
    char dir[PATH_MAX];
    
    if (argc < 2) {
        if (getcwd(dir, sizeof(dir)) == NULL)
            exit_err("could not get working directory");
    } else {
        strcpy(dir, argv[1]);
    }
    
    // if passed just a file, only count that
    struct stat st;
    if (stat(dir, &st) == 0 && S_ISREG(st.st_mode)) {
        char *parent_dir = strdup(dir);
        parent_dir = dirname(parent_dir);
        int dirfd = open(parent_dir, O_RDONLY);

        char *base = basename(dir);
        count_file(dirfd, base);
    } else {
        count_dir(dir);
    }
    
    printf("Lang\t| Total\t| Code\t| Cmt\t| Blank\n");
    printf("-------- ------- ------- ------- -------\n");
    for (Lang *lang = langs; lang->name != NULL; ++lang) {
        if (lang->size) {
            printf(
                "%s\t| %lu\t| %lu\t| %lu\t| %lu\n",
                lang->name,
                lang->total,
                lang->code,
                lang->comment,
                lang->empty
            );
        }
    }
}
