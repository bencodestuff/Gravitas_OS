#include <gravity/filer.h>
#include <grv/syscall.h>

// gravity_filer — see filer.h. This is what files.elf used to be, minus the
// window, the path bar and the "open" policy, so the IDE can put the same
// browser in a pane and send the file somewhere else.

// Catppuccin Mocha, the desktop theme (see gravity_editor for the same note).
static const unsigned int FG     = 0xFFD0DCE8;
static const unsigned int BG     = 0xFF1E2430;
static const unsigned int SEL_FG = 0xFFFFFFFF;
static const unsigned int SEL_BG = 0xFF35507A;

int gravity_filer_is_dir(const char* name) {
    int n = 0;
    while (name[n]) n++;
    return n > 0 && name[n - 1] == '/';
}

static int name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int before(const char* a, const char* b) {
    int da = gravity_filer_is_dir(a), db = gravity_filer_is_dir(b);
    if (da != db) return da; // directories first
    return name_cmp(a, b) < 0;
}

/* Insertion sort: the listing is small and almost sorted already. */
static void sort_items(struct gravity_filer* f) {
    int start = f->has_dotdot ? 1 : 0;
    for (int i = start + 1; i < f->n; i++) {
        const char* key = f->items[i];
        int j = i - 1;
        while (j >= start && before(key, f->items[j])) {
            f->items[j + 1] = f->items[j];
            j--;
        }
        f->items[j + 1] = key;
    }
}

void gravity_filer_path(const struct gravity_filer* f, char* out, int max,
                        const char* name) {
    int n = 0;
    while (f->cwd[n] && n < max - 1) { out[n] = f->cwd[n]; n++; }
    if (n == 0 || out[n - 1] != '/') out[n++] = '/';
    for (int i = 0; name[i] && name[i] != '/' && n < max - 1; i++)
        out[n++] = name[i];
    out[n] = '\0';
}

int gravity_filer_load(struct gravity_filer* f, const char* path) {
    if (grv_chdir(path) != 0) return -1; // unreadable: stay put
    grv_getcwd(f->cwd, sizeof(f->cwd));
    grv_readdir(".", f->listing, sizeof(f->listing));

    f->n = 0;
    f->has_dotdot = !(f->cwd[0] == '/' && f->cwd[1] == '\0');
    if (f->has_dotdot) {
        f->names[0][0] = '.'; f->names[0][1] = '.'; f->names[0][2] = '/';
        f->names[0][3] = '\0';
        f->items[f->n++] = f->names[0];
    }

    char* p = f->listing;
    while (*p && f->n < GRAVITY_FILER_MAX) {
        char* nl = p;
        while (*nl && *nl != '\n') nl++;
        int len = (int)(nl - p);
        if (len > 0 && len < GRAVITY_FILER_NAME) {
            for (int i = 0; i < len; i++) f->names[f->n][i] = p[i];
            f->names[f->n][len] = '\0';
            f->items[f->n] = f->names[f->n];
            f->n++;
        }
        p = *nl ? nl + 1 : nl;
    }

    sort_items(f);
    gravity_list_set(&f->list, f->items, f->n);
    return 0;
}

int gravity_filer_up(struct gravity_filer* f) {
    int n = 0;
    while (f->cwd[n]) n++;
    if (n <= 1) return 0; // at the root
    while (n > 1 && f->cwd[n - 1] == '/') n--;
    int i = n - 1;
    while (i > 0 && f->cwd[i] != '/') i--;
    char parent[128];
    if (i == 0) { parent[0] = '/'; parent[1] = '\0'; }
    else { int k; for (k = 0; k < i; k++) parent[k] = f->cwd[k]; parent[k] = '\0'; }
    gravity_filer_load(f, parent);
    return 1;
}

const char* gravity_filer_selected(const struct gravity_filer* f) {
    if (f->list.sel < 0 || f->list.sel >= f->n) return 0;
    return f->items[f->list.sel];
}

int gravity_filer_enter(struct gravity_filer* f) {
    const char* name = gravity_filer_selected(f);
    if (name == 0) return 0;
    if (f->has_dotdot && f->list.sel == 0) { gravity_filer_up(f); return 1; }
    if (!gravity_filer_is_dir(name)) return 0;
    char path[160];
    gravity_filer_path(f, path, sizeof(path), name);
    gravity_filer_load(f, path);
    return 1;
}

int gravity_filer_init(struct gravity_filer* f, const char* path,
                       int x, int y, int w, int h, int row_h) {
    f->n = 0;
    f->has_dotdot = 0;
    f->cwd[0] = '/';
    f->cwd[1] = '\0';
    f->listing[0] = '\0';
    gravity_list_init(&f->list, x, y, w, h, row_h, FG, BG, SEL_FG, SEL_BG);
    return gravity_filer_load(f, path);
}

void gravity_filer_resize(struct gravity_filer* f, int x, int y, int w, int h) {
    gravity_list_resize(&f->list, x, y, w, h);
}
