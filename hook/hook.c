// OpenGL shader hook for Hytale: Windows opengl32.dll proxy / Linux SDL3 interposer.
// The Windows proxy forwards everything to the system opengl32.dll and
// intercepts shader compilation so the "shaderpack" folder next to this DLL can replace
// the game's embedded GLSL. Programs are relinked live whenever a file in
// shaderpack/programs or shaderpack/include changes; uniform locations are virtualised
// so the game's cached locations stay valid across relinks.
#ifdef _WIN32
#include <windows.h>
#else
#include "platform_linux.h"
#endif
#include <wctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <wctype.h>
#include <stddef.h>

#ifndef AU_HAS_DLSS
#define AU_HAS_DLSS 0
#endif
static int g_dlssState; // 0 not tried, 1 ready, -1 unavailable

#ifdef _WIN32
#define AU_GL_EXPORT __declspec(dllexport)
#else
// Linux uses SDL's proc lookup; don't interpose GL calls internal to the driver.
#define AU_GL_EXPORT static
#endif

typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;

#define GL_SHADER_TYPE        0x8B4F
#define GL_COMPILE_STATUS     0x8B81
#define GL_LINK_STATUS        0x8B82
#define GL_INFO_LOG_LENGTH    0x8B84
#define GL_ATTACHED_SHADERS   0x8B85
#define GL_CURRENT_PROGRAM    0x8B8D
#define GL_VERTEX_SHADER      0x8B31
#define GL_FRAGMENT_SHADER    0x8B30
#define GL_GEOMETRY_SHADER    0x8DD9
#define GL_INVALID_INDEX      0xFFFFFFFFu

int stubs_resolve(HMODULE real);

#ifdef _WIN32
static HMODULE g_real;
#endif
static wchar_t g_packDir[MAX_PATH];
static CRITICAL_SECTION g_lock;
static FILE *g_log;
static int g_dump;
static volatile LONG g_reloadRequested;
static int g_enabled = 1;

// ----------------------------------------------------------------------------------------
// Logging / files

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

static void pack_path(wchar_t *out, const char *rel)
{
#ifdef _WIN32
    _snwprintf(out, MAX_PATH, L"%s\\%hs", g_packDir, rel);
#else
    wchar_t wide[MAX_PATH];
    au_to_wide(wide, MAX_PATH, rel);
    for (wchar_t *p = wide; *p; p++) if (*p == L'\\') *p = L'/';
    swprintf(out, MAX_PATH, L"%ls/%ls", g_packDir, wide);
#endif
    out[MAX_PATH - 1] = 0;
}

static char *read_file(const char *rel, size_t *outLen)
{
    wchar_t path[MAX_PATH];
    pack_path(path, rel);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    // Strip a UTF-8 BOM.
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 2);
        got -= 3;
    }
    if (outLen) *outLen = got;
    return buf;
}

static void write_file(const char *rel, const char *data, size_t n)
{
    wchar_t path[MAX_PATH];
    pack_path(path, rel);
    FILE *f = _wfopen(path, L"wb");
    if (f) { fwrite(data, 1, n, f); fclose(f); }
}

static uint64_t fnv1a(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

// Growable string
typedef struct { char *p; size_t n, cap; } Str;
static void str_add(Str *s, const char *d, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        s->cap = (s->n + n + 1) * 2;
        s->p = (char *)realloc(s->p, s->cap);
    }
    memcpy(s->p + s->n, d, n);
    s->n += n;
    s->p[s->n] = 0;
}
static void str_cstr(Str *s, const char *d) { str_add(s, d, strlen(d)); }

// ----------------------------------------------------------------------------------------
// Real GL entry points

typedef PROC (WINAPI *PFN_wglGetProcAddress)(LPCSTR);
static PFN_wglGetProcAddress real_wglGetProcAddress;

#define GLFN(ret, name, args) typedef ret (APIENTRY *PFN_##name) args; static PFN_##name r_##name;
GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
GLFN(void, glGetShaderiv, (GLuint, GLenum, GLint *))
GLFN(GLuint, glCreateShader, (GLenum))
GLFN(void, glCompileShader, (GLuint))
GLFN(void, glDeleteShader, (GLuint))
GLFN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
GLFN(void, glAttachShader, (GLuint, GLuint))
GLFN(void, glDetachShader, (GLuint, GLuint))
GLFN(void, glLinkProgram, (GLuint))
GLFN(void, glGetProgramiv, (GLuint, GLenum, GLint *))
GLFN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
GLFN(void, glGetAttachedShaders, (GLuint, GLsizei, GLsizei *, GLuint *))
GLFN(void, glDeleteProgram, (GLuint))
GLFN(void, glUseProgram, (GLuint))
GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar *))
GLFN(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *))
GLFN(void, glUniformBlockBinding, (GLuint, GLuint, GLuint))
GLFN(void, glGetActiveUniformBlockName, (GLuint, GLuint, GLsizei, GLsizei *, GLchar *))
GLFN(void, glGetIntegerv, (GLenum, GLint *))

GLFN(void, glUniform1f, (GLint, GLfloat))
GLFN(void, glUniform2f, (GLint, GLfloat, GLfloat))
GLFN(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLFN(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLFN(void, glUniform1i, (GLint, GLint))
GLFN(void, glUniform2i, (GLint, GLint, GLint))
GLFN(void, glUniform3i, (GLint, GLint, GLint, GLint))
GLFN(void, glUniform4i, (GLint, GLint, GLint, GLint, GLint))
GLFN(void, glUniform3ui, (GLint, GLuint, GLuint, GLuint))
GLFN(void, glUniform1fv, (GLint, GLsizei, const GLfloat *))
GLFN(void, glUniform2fv, (GLint, GLsizei, const GLfloat *))
GLFN(void, glUniform3fv, (GLint, GLsizei, const GLfloat *))
GLFN(void, glUniform4fv, (GLint, GLsizei, const GLfloat *))
GLFN(void, glUniform1iv, (GLint, GLsizei, const GLint *))
GLFN(void, glUniform1uiv, (GLint, GLsizei, const GLuint *))
GLFN(void, glUniformMatrix2fv, (GLint, GLsizei, GLboolean, const GLfloat *))
GLFN(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *))
GLFN(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))

static int g_helpersResolved;
static void resolve_helpers(void)
{
    if (g_helpersResolved) return;
    g_helpersResolved = 1;
#define RES(name) if (!r_##name) r_##name = (PFN_##name)real_wglGetProcAddress(#name);
    RES(glShaderSource) RES(glGetShaderiv) RES(glCreateShader) RES(glCompileShader) RES(glDeleteShader)
    RES(glGetShaderInfoLog) RES(glAttachShader) RES(glDetachShader) RES(glLinkProgram) RES(glGetProgramiv)
    RES(glGetProgramInfoLog) RES(glGetAttachedShaders) RES(glDeleteProgram) RES(glUseProgram)
    RES(glGetUniformLocation) RES(glGetUniformBlockIndex) RES(glUniformBlockBinding) RES(glGetActiveUniformBlockName)
    RES(glUniform1f) RES(glUniform2f) RES(glUniform3f) RES(glUniform4f) RES(glUniform1i) RES(glUniform2i)
    RES(glUniform3i) RES(glUniform4i) RES(glUniform3ui) RES(glUniform1fv) RES(glUniform2fv) RES(glUniform3fv)
    RES(glUniform4fv) RES(glUniform1iv) RES(glUniform1uiv) RES(glUniformMatrix2fv) RES(glUniformMatrix3fv)
    RES(glUniformMatrix4fv)
#undef RES
    if (!r_glGetIntegerv) r_glGetIntegerv = (PFN_glGetIntegerv)GetProcAddress(g_real, "glGetIntegerv");
}

// ----------------------------------------------------------------------------------------
// Shader identification index (shaderpack/index.txt)

typedef struct { char name[64]; int nsig; uint64_t sig[3]; } IndexEntry;
static IndexEntry *g_index;
static int g_indexCount;

static uint64_t line_hash(const char *s, size_t n)
{
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return n ? fnv1a(s, n) : 0;
}

static void load_index(void)
{
    free(g_index);
    g_index = NULL;
    g_indexCount = 0;
    size_t len;
    char *txt = read_file("index.txt", &len);
    if (!txt) { logf_("index.txt missing"); return; }
    int cap = 128;
    g_index = (IndexEntry *)calloc(cap, sizeof(IndexEntry));
    char *line = txt;
    while (*line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        char *fields[4] = {0};
        int nf = 0;
        char *p = line;
        while (p && nf < 4) {
            fields[nf++] = p;
            char *tab = strchr(p, '\t');
            if (tab) { *tab = 0; p = tab + 1; } else p = NULL;
        }
        if (nf >= 2) {
            if (g_indexCount == cap) { cap *= 2; g_index = (IndexEntry *)realloc(g_index, cap * sizeof(IndexEntry)); }
            IndexEntry *e = &g_index[g_indexCount++];
            memset(e, 0, sizeof(*e));
            strncpy(e->name, fields[0], sizeof(e->name) - 1);
            for (int i = 1; i < nf; i++) e->sig[e->nsig++] = line_hash(fields[i], strlen(fields[i]));
        }
        if (!eol) break;
        line = eol + 1;
    }
    free(txt);
    logf_("index: %d entries", g_indexCount);
}

// Returns index entry number or -1.
static int identify(const char *src)
{
    if (!g_indexCount) return -1;
    // Small open-addressing set of the source's line hashes.
    size_t cap = 4096;
    for (const char *c = src; *c; c++) if (*c == '\n') cap++;
    size_t sz = 1;
    while (sz < cap * 2) sz <<= 1;
    uint64_t *set = (uint64_t *)calloc(sz, sizeof(uint64_t));
    const char *p = src;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        uint64_t h = line_hash(p, n);
        if (h) {
            size_t i = (size_t)h & (sz - 1);
            while (set[i] && set[i] != h) i = (i + 1) & (sz - 1);
            set[i] = h;
        }
        if (!e) break;
        p = e + 1;
    }
    int found = -1;
    for (int k = 0; k < g_indexCount && found < 0; k++) {
        IndexEntry *en = &g_index[k];
        int ok = en->nsig > 0;
        for (int s = 0; s < en->nsig && ok; s++) {
            uint64_t h = en->sig[s];
            size_t i = (size_t)h & (sz - 1);
            while (set[i] && set[i] != h) i = (i + 1) & (sz - 1);
            if (set[i] != h) ok = 0;
        }
        if (ok) found = k;
    }
    free(set);
    return found;
}

// ----------------------------------------------------------------------------------------
// Override source construction

static int is_comment_line(const char *line, const char *inc)
{
    for (const char *c = line; c < inc; c++) {
        if (c[0] == '/' && c[1] == '/') return 1;
    }
    return 0;
}

static void expand(Str *out, const char *text, int depth)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p + 1) : strlen(p);
        const char *inc = NULL;
        for (const char *c = p; c < p + n; c++) {
            if (*c == '#' && !strncmp(c, "#include", 8)) { inc = c; break; }
        }
        if (inc && depth < 16 && !is_comment_line(p, inc)) {
            const char *q1 = strchr(inc, '"');
            const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2 && q2 < p + n) {
                char name[128], rel[160];
                size_t ln = (size_t)(q2 - q1 - 1);
                if (ln >= sizeof(name)) ln = sizeof(name) - 1;
                memcpy(name, q1 + 1, ln);
                name[ln] = 0;
                snprintf(rel, sizeof(rel), "include\\%s", name);
                char *body = read_file(rel, NULL);
                if (!body) { snprintf(rel, sizeof(rel), "original\\%s", name); body = read_file(rel, NULL); }
                if (body) {
                    str_cstr(out, "// BEGIN ");
                    str_cstr(out, name);
                    str_cstr(out, "\n");
                    expand(out, body, depth + 1);
#if !AU_HAS_DLSS
                    if (!strcmp(name, "AU_Settings.glsl"))
                        str_cstr(out, "\n#undef AU_DLSS\n#define AU_DLSS 0\n#undef AU_DLSS_RR\n#define AU_DLSS_RR 0\n");
#elif !defined(_WIN32)
                    if (!strcmp(name, "AU_Settings.glsl")) {
                        str_cstr(out, "\n#undef AU_DLSS_FRAMEGEN\n#define AU_DLSS_FRAMEGEN 0\n");
                        if (g_dlssState < 0)
                            str_cstr(out, "\n#undef AU_DLSS\n#define AU_DLSS 0\n");
                    }
#endif
#ifdef __APPLE__
                    // macOS OpenGL 4.1 has no image load/store: no voxel grid, no image-based GI history.
                    if (!strcmp(name, "AU_Settings.glsl"))
                        str_cstr(out, "#undef AU_LOCAL_LIGHTS\n#define AU_LOCAL_LIGHTS 0\n#undef AU_PATH_TRACING\n#define AU_PATH_TRACING 0\n"
                                      "#undef AU_GI_DENOISER\n#define AU_GI_DENOISER 0\n#if AU_SSS_VOXEL == 1\n#undef AU_SSS_VOXEL\n#define AU_SSS_VOXEL 0\n#endif\n");
#endif
                    str_cstr(out, "\n// END ");
                    str_cstr(out, name);
                    str_cstr(out, "\n");
                    free(body);
                } else {
                    logf_("include not found: %s", name);
                    str_cstr(out, "#error include not found\n");
                }
                p += n;
                continue;
            }
        }
        // Drop #version lines from the override; the game's prefix carries it.
        const char *t = p;
        while (t < p + n && (*t == ' ' || *t == '\t')) t++;
        if (!strncmp(t, "#version", 8)) str_cstr(out, "\n");
        else str_add(out, p, n);
        p += n;
    }
}

// Builds the replacement source for runtime source `orig` identified as entry `idx`.
// Returns NULL when there is no override file.
static char *build_override(const char *orig, int idx)
{
    if (idx < 0 || !g_enabled) return NULL;
    char rel[128];
    snprintf(rel, sizeof(rel), "programs\\%s.glsl", g_index[idx].name);
    char *main = read_file(rel, NULL);
    if (!main) return NULL;

    Str out = {0};
    const char *marker = strstr(orig, "END: CPU - DEFINES");
    if (marker) {
        const char *eol = strchr(marker, '\n');
        size_t n = eol ? (size_t)(eol - orig + 1) : strlen(orig);
        str_add(&out, orig, n);
    } else {
        const char *eol = strchr(orig, '\n');
        size_t n = eol ? (size_t)(eol - orig + 1) : strlen(orig);
        str_add(&out, orig, n); // just the #version line
    }
    str_cstr(&out, "#define HYTALE_SHADERPACK 1\n");
#ifdef __APPLE__
    // Apple's compiler rejects #extension after any code, and the pack's additions come before the
    // game's own directives. All of them are ": enable", so hoisting them out of #if blocks is safe.
    Str body = {0};
    expand(&body, main, 0);
    for (char *line = body.p; line && *line; ) {
        char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (!strncmp(line + strspn(line, " \t"), "#extension", 10)) {
            str_add(&out, line, length);
            str_cstr(&out, "\n");
            memset(line, ' ', length);
        }
        line = end ? end + 1 : NULL;
    }
    if (body.n) str_add(&out, body.p, body.n);
    free(body.p);
#else
    expand(&out, main, 0);
#endif
    free(main);
    return out.p;
}

static const char *type_tag(GLint type)
{
    switch (type) {
    case GL_VERTEX_SHADER: return "vs";
    case GL_FRAGMENT_SHADER: return "fs";
    case GL_GEOMETRY_SHADER: return "gs";
    default: return "xx";
    }
}

// Compiles `src` into the shader object `sh`. Returns 0 on failure (and logs the error).
static int compile_into(GLuint sh, GLenum type, const char *src, const char *name)
{
    const GLchar *s = src;
    r_glShaderSource(sh, 1, &s, NULL);
    r_glCompileShader(sh);
    GLint ok = 0;
    r_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint ll = 0;
        r_glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &ll);
        char *log = (char *)malloc(ll + 1);
        log[0] = 0;
        r_glGetShaderInfoLog(sh, ll, NULL, log);
        logf_("COMPILE ERROR %s (%s):\n%s", name, type_tag(type), log);
        char rel[160];
        snprintf(rel, sizeof(rel), "errors\\%s_%s.glsl", name, type_tag(type));
        write_file(rel, src, strlen(src));
        snprintf(rel, sizeof(rel), "errors\\%s_%s.log", name, type_tag(type));
        write_file(rel, log, strlen(log));
        free(log);
    }
    return ok;
}

// Compiles `src` into a fresh shader object. Returns 0 on failure (and logs the error).
static GLuint compile_new(GLenum type, const char *src, const char *name)
{
    GLuint sh = r_glCreateShader(type);
    if (!compile_into(sh, type, src, name)) {
        r_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// ----------------------------------------------------------------------------------------
// Shader / program bookkeeping

// orig: the game's source; curHash: hash of the source currently compiled for this stage;
// id: in a program's copy, the game's shader object attached for this stage.
typedef struct { GLenum type; char *orig; int idx; int overridden; uint64_t curHash; GLuint id; } ShaderRec;

typedef struct {
    char *name;
    GLint real;
    int fn;              // cached call kind (0 = none)
    GLsizei count;
    GLboolean transpose;
    size_t bytes;
    void *data;
} UniformRec;

typedef struct { char *name; GLuint binding; } BlockRec;

typedef struct {
    int used;
    int linked;
    int nsh;
    ShaderRec sh[4];
    int nuni, capuni;
    UniformRec *uni;
    int nblk;
    BlockRec blk[8];
    char pname[64];      // name of the fragment stage (or the first identified stage)
    int blkChecked;      // unbound uniform blocks already given the game's binding
    int extrasChecked;   // custom texture / forwarded uniform locations resolved
    int hasExtras;
    GLint ctexLoc[8];
    GLint fwdLoc[16];
    GLint locFrame, locTime, locWetness;
    GLint locPreviousVP, locPreviousCamera, locSceneJitter, locPreviousJitter, locGeometryJitter, locPreviousGeometryJitter, locHistoryValid;
    GLint locCurrentProjection;
    GLint locGICompact, locGISourceSize, locGIPreviousInverse, locGIPreviousDepth;
    GLint locVoxGeo, locVoxCol;
    GLint locNativeEmission;
    GLint locMaterialLOD;
    GLint locJitter, locUIFragmentScale;
    int usesGI;
    int giDenoise, giTemporal, giLegacy;
    int giCompact, giLights;
    GLint locPTPass, locLocalLightTiles;
    int usesDlss;
    GLint locDlssActive, locDlssRR;
    GLint locMaterials;      // uHookMaterials (terrain): per-tile block materials
    GLint locSSSMasks, locSSSReady;
    int shareGen, shareSrc;  // cached: is this program the source of a texture share
} ProgRec;

// Uniform block bindings the game uses, by block name. Programs we rewrite may gain
// blocks (e.g. the scene UBO) that the game never binds for them.
typedef struct { char name[64]; GLuint binding; } KnownBlock;
static KnownBlock g_knownBlocks[16];
static int g_nKnownBlocks;


static ShaderRec *g_shaders;   // indexed by shader id
static size_t g_shaderCap;
static ProgRec *g_progs;       // indexed by program id
static size_t g_progCap;
static GLuint g_curProg;

static ShaderRec *shader_rec(GLuint id, int create)
{
    if (id >= g_shaderCap) {
        if (!create) return NULL;
        size_t n = g_shaderCap ? g_shaderCap : 256;
        while (n <= id) n *= 2;
        g_shaders = (ShaderRec *)realloc(g_shaders, n * sizeof(ShaderRec));
        memset(g_shaders + g_shaderCap, 0, (n - g_shaderCap) * sizeof(ShaderRec));
        g_shaderCap = n;
    }
    return &g_shaders[id];
}

// The game's shader object `id` now holds a source with this hash (ours or the game's).
static void shader_compiled(GLuint id, int overridden, uint64_t hash)
{
    ShaderRec *rec = shader_rec(id, 0);
    if (!rec || !rec->orig) return;
    rec->overridden = overridden;
    rec->curHash = hash;
}

static ProgRec *prog_rec(GLuint id, int create)
{
    if (id >= g_progCap) {
        if (!create) return NULL;
        size_t n = g_progCap ? g_progCap : 256;
        while (n <= id) n *= 2;
        g_progs = (ProgRec *)realloc(g_progs, n * sizeof(ProgRec));
        memset(g_progs + g_progCap, 0, (n - g_progCap) * sizeof(ProgRec));
        g_progCap = n;
    }
    ProgRec *p = &g_progs[id];
    if (!p->used && !create) return NULL;
    p->used = 1;
    return p;
}

static void prog_free(ProgRec *p)
{
    for (int i = 0; i < p->nsh; i++) free(p->sh[i].orig);
    for (int i = 0; i < p->nuni; i++) { free(p->uni[i].name); free(p->uni[i].data); }
    free(p->uni);
    for (int i = 0; i < p->nblk; i++) free(p->blk[i].name);
    memset(p, 0, sizeof(*p));
}

// Virtual uniform locations: (index + 1) << 10 | element offset.
#define VLOC_SHIFT 10
#define VLOC_MASK ((1 << VLOC_SHIFT) - 1)

static UniformRec *vloc_rec(ProgRec *p, GLint vloc, GLint *real)
{
    if (vloc < (1 << VLOC_SHIFT)) { *real = -1; return NULL; }
    int idx = (vloc >> VLOC_SHIFT) - 1;
    int off = vloc & VLOC_MASK;
    if (!p || idx >= p->nuni) { *real = -1; return NULL; }
    UniformRec *u = &p->uni[idx];
    *real = u->real < 0 ? -1 : u->real + off;
    return off == 0 ? u : NULL;
}

static void refresh_uniforms(GLuint prog, ProgRec *p)
{
    for (int i = 0; i < p->nuni; i++) p->uni[i].real = r_glGetUniformLocation(prog, p->uni[i].name);
}

// ----------------------------------------------------------------------------------------
// Hooks: shader compilation

static void APIENTRY hook_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    resolve_helpers();

    size_t total = 0;
    for (GLsizei i = 0; i < count; i++)
        total += (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(strings[i]);
    char *src = (char *)malloc(total + 1);
    size_t pos = 0;
    for (GLsizei i = 0; i < count; i++) {
        size_t n = (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(src + pos, strings[i], n);
        pos += n;
    }
    src[total] = 0;

    GLint type = 0;
    r_glGetShaderiv(shader, GL_SHADER_TYPE, &type);

    EnterCriticalSection(&g_lock);
    int idx = identify(src);
    ShaderRec *rec = shader_rec(shader, 1);
    free(rec->orig);
    rec->orig = src;
    rec->type = (GLenum)type;
    rec->idx = idx;
    rec->overridden = 0;

    if (g_dump) {
        uint64_t h = fnv1a(src, total);
        char rel[160];
        snprintf(rel, sizeof(rel), "dump\\%s_%s_%016llx.glsl", idx >= 0 ? g_index[idx].name : "unknown", type_tag(type), (unsigned long long)h);
        wchar_t path[MAX_PATH];
        pack_path(path, rel);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) write_file(rel, src, total);
    }

    char *ovr = build_override(src, idx);
    GLuint test = 0;
    if (ovr) {
        test = compile_new((GLenum)type, ovr, g_index[idx].name);
        if (test) r_glDeleteShader(test);
    }
    LeaveCriticalSection(&g_lock);

    if (ovr && test) {
        const GLchar *s = ovr;
        r_glShaderSource(shader, 1, &s, NULL);
        rec->overridden = 1;
        rec->curHash = fnv1a(ovr, strlen(ovr));
        logf_("override applied: %s (%s) shader=%u", g_index[idx].name, type_tag(type), shader);
    } else {
        rec->curHash = fnv1a(src, total);
        r_glShaderSource(shader, count, strings, lengths);
    }
    free(ovr);
}

#define GL_ACTIVE_ATTRIBUTES 0x8B89
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void (APIENTRY *PFN_glGetActiveAttrib)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
typedef GLint (APIENTRY *PFN_glGetAttribLocation)(GLuint, const GLchar *);

// After the game links a program that contains overridden stages: make sure the link worked
// and that every vertex attribute the original shaders expose is still active (the game
// treats a missing attribute as fatal). Otherwise relink with the game's sources.
// Returns 1 if the program was left as is.
static int validate_override_link(GLuint prog, ProgRec *p, GLint ok)
{
    int any = 0;
    for (int i = 0; i < p->nsh; i++) if (p->sh[i].overridden) any = 1;
    if (!any) return 1;

    static PFN_glCreateProgram createProg;
    static PFN_glGetActiveAttrib activeAttrib;
    static PFN_glGetAttribLocation attribLoc;
    if (!createProg) {
        createProg = (PFN_glCreateProgram)real_wglGetProcAddress("glCreateProgram");
        activeAttrib = (PFN_glGetActiveAttrib)real_wglGetProcAddress("glGetActiveAttrib");
        attribLoc = (PFN_glGetAttribLocation)real_wglGetProcAddress("glGetAttribLocation");
    }
    int bad = !ok;
    char missing[64] = "";
    if (!bad && createProg && activeAttrib && attribLoc) {
        GLuint tmp = createProg();
        GLuint sh[4] = {0};
        for (int i = 0; i < p->nsh; i++) {
            sh[i] = compile_new(p->sh[i].type, p->sh[i].orig, "original");
            if (sh[i]) r_glAttachShader(tmp, sh[i]);
        }
        r_glLinkProgram(tmp);
        GLint tmpOk = 0, n = 0;
        r_glGetProgramiv(tmp, GL_LINK_STATUS, &tmpOk);
        if (tmpOk) r_glGetProgramiv(tmp, GL_ACTIVE_ATTRIBUTES, &n);
        for (GLint a = 0; a < n && !bad; a++) {
            char name[64] = {0};
            GLint size; GLenum type;
            activeAttrib(tmp, (GLuint)a, sizeof(name) - 1, NULL, &size, &type, name);
            if (strncmp(name, "gl_", 3) && attribLoc(prog, name) < 0) { bad = 1; snprintf(missing, sizeof(missing), "%s", name); }
        }
        for (int i = 0; i < p->nsh; i++) if (sh[i]) { r_glDetachShader(tmp, sh[i]); r_glDeleteShader(sh[i]); }
        r_glDeleteProgram(tmp);
    }
    if (!bad) return 1;

    logf_("OVERRIDE REJECTED for %s (program %u): %s%s - using the game's shaders", p->pname, prog,
          ok ? "attribute optimised away: " : "link failed", missing);
    // The game's sources, compiled back into its own shader objects (see relink_program).
    for (int i = 0; i < p->nsh; i++) {
        ShaderRec *s = &p->sh[i];
        uint64_t hash = fnv1a(s->orig, strlen(s->orig));
        if (compile_into(s->id, s->type, s->orig, p->pname)) shader_compiled(s->id, 0, hash);
        s->overridden = 0;
        s->curHash = hash;
    }
    r_glLinkProgram(prog);
    return 0;
}

static void APIENTRY hook_glLinkProgram(GLuint prog)
{
    resolve_helpers();
    EnterCriticalSection(&g_lock);
    ProgRec *p = prog_rec(prog, 1);
    for (int i = 0; i < p->nsh; i++) free(p->sh[i].orig);
    p->nsh = 0;
    GLuint ids[4];
    GLsizei n = 0;
    r_glGetAttachedShaders(prog, 4, &n, ids);
    for (GLsizei i = 0; i < n && i < 4; i++) {
        ShaderRec *s = shader_rec(ids[i], 0);
        if (!s || !s->orig) continue;
        ShaderRec *d = &p->sh[p->nsh++];
        d->type = s->type;
        d->orig = _strdup(s->orig);
        d->idx = s->idx;
        d->overridden = s->overridden;
        d->curHash = s->curHash;
        d->id = ids[i];
    }
    p->pname[0] = 0;
    for (int i = 0; i < p->nsh; i++) {
        if (p->sh[i].idx < 0) continue;
        if (!p->pname[0] || p->sh[i].type == GL_FRAGMENT_SHADER)
            snprintf(p->pname, sizeof(p->pname), "%s", g_index[p->sh[i].idx].name);
    }
    p->blkChecked = 0;
    p->extrasChecked = 0;
    LeaveCriticalSection(&g_lock);

    r_glLinkProgram(prog);

    GLint ok = 0;
    r_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    EnterCriticalSection(&g_lock);
    p = prog_rec(prog, 1);
    if (!validate_override_link(prog, p, ok)) r_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    p->linked = ok;
    refresh_uniforms(prog, p);
    if (!ok) {
        char buf[2048] = {0};
        r_glGetProgramInfoLog(prog, sizeof(buf) - 1, NULL, buf);
        logf_("LINK ERROR program %u (%s): %s", prog, p->pname, buf);
    }
    LeaveCriticalSection(&g_lock);
}

static void APIENTRY hook_glDeleteProgram(GLuint prog)
{
    EnterCriticalSection(&g_lock);
    ProgRec *p = prog_rec(prog, 0);
    if (p) prog_free(p);
    LeaveCriticalSection(&g_lock);
    r_glDeleteProgram(prog);
}

// The game deletes shader objects while they are still attached (before linking), so
// shader records are kept until the id is reused by a new glShaderSource.
static void APIENTRY hook_glDeleteShader(GLuint shader)
{
    r_glDeleteShader(shader);
}

static GLint APIENTRY hook_glGetUniformLocation(GLuint prog, const GLchar *name)
{
    EnterCriticalSection(&g_lock);
    ProgRec *p = prog_rec(prog, 1);
    int idx = -1;
    for (int i = 0; i < p->nuni; i++) if (!strcmp(p->uni[i].name, name)) { idx = i; break; }
    if (idx < 0) {
        if (p->nuni == p->capuni) {
            p->capuni = p->capuni ? p->capuni * 2 : 16;
            p->uni = (UniformRec *)realloc(p->uni, p->capuni * sizeof(UniformRec));
        }
        idx = p->nuni++;
        UniformRec *u = &p->uni[idx];
        memset(u, 0, sizeof(*u));
        u->name = _strdup(name);
        u->real = r_glGetUniformLocation(prog, name);
    }
    GLint v = (GLint)((idx + 1) << VLOC_SHIFT);
    LeaveCriticalSection(&g_lock);
    return v;
}

static void APIENTRY hook_glUniformBlockBinding(GLuint prog, GLuint index, GLuint binding)
{
    resolve_helpers();
    char name[128] = {0};
    r_glGetActiveUniformBlockName(prog, index, sizeof(name) - 1, NULL, name);
    EnterCriticalSection(&g_lock);
    ProgRec *p = prog_rec(prog, 1);
    int found = 0;
    for (int i = 0; i < p->nblk; i++) if (!strcmp(p->blk[i].name, name)) { p->blk[i].binding = binding; found = 1; }
    if (!found && p->nblk < 8 && name[0]) { p->blk[p->nblk].name = _strdup(name); p->blk[p->nblk].binding = binding; p->nblk++; }
    found = 0;
    for (int i = 0; i < g_nKnownBlocks; i++) if (!strcmp(g_knownBlocks[i].name, name)) found = 1;
    if (!found && g_nKnownBlocks < 16 && name[0]) {
        snprintf(g_knownBlocks[g_nKnownBlocks].name, 64, "%.63s", name);
        g_knownBlocks[g_nKnownBlocks].binding = binding;
        g_nKnownBlocks++;
        logf_("uniform block %s -> binding %u", name, binding);
    }
    LeaveCriticalSection(&g_lock);
    r_glUniformBlockBinding(prog, index, binding);
}

static void APIENTRY hook_glUseProgram(GLuint prog);

// ----------------------------------------------------------------------------------------
// Hooks: uniform setters (translate virtual locations, cache values for relinks)

enum { U_1F = 1, U_2F, U_3F, U_4F, U_1I, U_2I, U_3I, U_4I, U_3UI, U_1FV, U_2FV, U_3FV, U_4FV, U_1IV, U_1UIV, U_M2, U_M3, U_M4 };

static void cache_uniform(UniformRec *u, int fn, GLsizei count, GLboolean tr, const void *data, size_t bytes)
{
    if (!u) return;
    if (u->bytes != bytes) { free(u->data); u->data = malloc(bytes); u->bytes = bytes; }
    memcpy(u->data, data, bytes);
    u->fn = fn;
    u->count = count;
    u->transpose = tr;
}

#define UPRE GLint real; EnterCriticalSection(&g_lock); UniformRec *u = vloc_rec(prog_rec(g_curProg, 0), loc, &real);
#define UPOST LeaveCriticalSection(&g_lock);

static void APIENTRY hook_glUniform1f(GLint loc, GLfloat a) { UPRE GLfloat v[1] = {a}; cache_uniform(u, U_1F, 1, 0, v, sizeof v); UPOST r_glUniform1f(real, a); }
static void APIENTRY hook_glUniform2f(GLint loc, GLfloat a, GLfloat b) { UPRE GLfloat v[2] = {a, b}; cache_uniform(u, U_2F, 1, 0, v, sizeof v); UPOST r_glUniform2f(real, a, b); }
static void APIENTRY hook_glUniform3f(GLint loc, GLfloat a, GLfloat b, GLfloat c) { UPRE GLfloat v[3] = {a, b, c}; cache_uniform(u, U_3F, 1, 0, v, sizeof v); UPOST r_glUniform3f(real, a, b, c); }
static void APIENTRY hook_glUniform4f(GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { UPRE GLfloat v[4] = {a, b, c, d}; cache_uniform(u, U_4F, 1, 0, v, sizeof v); UPOST r_glUniform4f(real, a, b, c, d); }
static void APIENTRY hook_glUniform1i(GLint loc, GLint a) { UPRE GLint v[1] = {a}; cache_uniform(u, U_1I, 1, 0, v, sizeof v); UPOST r_glUniform1i(real, a); }
static void APIENTRY hook_glUniform2i(GLint loc, GLint a, GLint b) { UPRE GLint v[2] = {a, b}; cache_uniform(u, U_2I, 1, 0, v, sizeof v); UPOST r_glUniform2i(real, a, b); }
static void APIENTRY hook_glUniform3i(GLint loc, GLint a, GLint b, GLint c) { UPRE GLint v[3] = {a, b, c}; cache_uniform(u, U_3I, 1, 0, v, sizeof v); UPOST r_glUniform3i(real, a, b, c); }
static void APIENTRY hook_glUniform4i(GLint loc, GLint a, GLint b, GLint c, GLint d) { UPRE GLint v[4] = {a, b, c, d}; cache_uniform(u, U_4I, 1, 0, v, sizeof v); UPOST r_glUniform4i(real, a, b, c, d); }
static void APIENTRY hook_glUniform3ui(GLint loc, GLuint a, GLuint b, GLuint c) { UPRE GLuint v[3] = {a, b, c}; cache_uniform(u, U_3UI, 1, 0, v, sizeof v); UPOST r_glUniform3ui(real, a, b, c); }
static void APIENTRY hook_glUniform1fv(GLint loc, GLsizei n, const GLfloat *v) { UPRE cache_uniform(u, U_1FV, n, 0, v, n * sizeof(GLfloat)); UPOST r_glUniform1fv(real, n, v); }
static void APIENTRY hook_glUniform2fv(GLint loc, GLsizei n, const GLfloat *v) { UPRE cache_uniform(u, U_2FV, n, 0, v, n * 2 * sizeof(GLfloat)); UPOST r_glUniform2fv(real, n, v); }
static void APIENTRY hook_glUniform3fv(GLint loc, GLsizei n, const GLfloat *v) { UPRE cache_uniform(u, U_3FV, n, 0, v, n * 3 * sizeof(GLfloat)); UPOST r_glUniform3fv(real, n, v); }
static void APIENTRY hook_glUniform4fv(GLint loc, GLsizei n, const GLfloat *v) { UPRE cache_uniform(u, U_4FV, n, 0, v, n * 4 * sizeof(GLfloat)); UPOST r_glUniform4fv(real, n, v); }
static void APIENTRY hook_glUniform1iv(GLint loc, GLsizei n, const GLint *v) { UPRE cache_uniform(u, U_1IV, n, 0, v, n * sizeof(GLint)); UPOST r_glUniform1iv(real, n, v); }
static void APIENTRY hook_glUniform1uiv(GLint loc, GLsizei n, const GLuint *v) { UPRE cache_uniform(u, U_1UIV, n, 0, v, n * sizeof(GLuint)); UPOST r_glUniform1uiv(real, n, v); }
static void APIENTRY hook_glUniformMatrix2fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v) { UPRE cache_uniform(u, U_M2, n, t, v, n * 4 * sizeof(GLfloat)); UPOST r_glUniformMatrix2fv(real, n, t, v); }
static void APIENTRY hook_glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v) { UPRE cache_uniform(u, U_M3, n, t, v, n * 9 * sizeof(GLfloat)); UPOST r_glUniformMatrix3fv(real, n, t, v); }
static void APIENTRY hook_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v) { UPRE cache_uniform(u, U_M4, n, t, v, n * 16 * sizeof(GLfloat)); UPOST r_glUniformMatrix4fv(real, n, t, v); }

static void replay_uniform(UniformRec *u)
{
    if (!u->fn || u->real < 0) return;
    GLint l = u->real;
    const GLfloat *f = (const GLfloat *)u->data;
    const GLint *i = (const GLint *)u->data;
    const GLuint *ui = (const GLuint *)u->data;
    switch (u->fn) {
    case U_1F: r_glUniform1f(l, f[0]); break;
    case U_2F: r_glUniform2f(l, f[0], f[1]); break;
    case U_3F: r_glUniform3f(l, f[0], f[1], f[2]); break;
    case U_4F: r_glUniform4f(l, f[0], f[1], f[2], f[3]); break;
    case U_1I: r_glUniform1i(l, i[0]); break;
    case U_2I: r_glUniform2i(l, i[0], i[1]); break;
    case U_3I: r_glUniform3i(l, i[0], i[1], i[2]); break;
    case U_4I: r_glUniform4i(l, i[0], i[1], i[2], i[3]); break;
    case U_3UI: r_glUniform3ui(l, ui[0], ui[1], ui[2]); break;
    case U_1FV: r_glUniform1fv(l, u->count, f); break;
    case U_2FV: r_glUniform2fv(l, u->count, f); break;
    case U_3FV: r_glUniform3fv(l, u->count, f); break;
    case U_4FV: r_glUniform4fv(l, u->count, f); break;
    case U_1IV: r_glUniform1iv(l, u->count, i); break;
    case U_1UIV: r_glUniform1uiv(l, u->count, ui); break;
    case U_M2: r_glUniformMatrix2fv(l, u->count, u->transpose, f); break;
    case U_M3: r_glUniformMatrix3fv(l, u->count, u->transpose, f); break;
    case U_M4: r_glUniformMatrix4fv(l, u->count, u->transpose, f); break;
    }
}

// ----------------------------------------------------------------------------------------
// Live reload

// Returns 1 = unchanged, 2 = relinked, 0 = compile failure, -1 = link failure.
// The new sources are compiled into the game's own shader objects, which stay attached. The game
// links those objects into new programs later (when a world loads) without sending their source
// again. Swapping in fresh objects freed the game's (it flags them for deletion right after
// attaching), the driver handed their names to other passes' shaders, and the world loaded black
// after a settings change (issue #12).
static int relink_program(GLuint prog, ProgRec *p, int useOverrides)
{
    char *src[4] = {0};
    int isOvr[4] = {0};
    uint64_t hash[4] = {0};
    int changed = 0;
    for (int i = 0; i < p->nsh; i++) {
        ShaderRec *s = &p->sh[i];
        char *ovr = useOverrides ? build_override(s->orig, s->idx) : NULL;
        isOvr[i] = ovr != NULL;
        src[i] = ovr ? ovr : _strdup(s->orig);
        hash[i] = fnv1a(src[i], strlen(src[i]));
        if (hash[i] != s->curHash) changed = 1;
    }
    if (!changed) {
        for (int i = 0; i < p->nsh; i++) free(src[i]);
        return 1;
    }

    for (int i = 0; i < p->nsh; i++) {
        ShaderRec *s = &p->sh[i];
        // Objects shared between programs (one vertex shader for several passes) compile once.
        ShaderRec *object = shader_rec(s->id, 0);
        if (object && object->orig && object->curHash == hash[i]) continue;
        const char *name = s->idx >= 0 ? g_index[s->idx].name : "unknown";
        int compiled = compile_into(s->id, s->type, src[i], name);
        if (!compiled && isOvr[i]) {
            // Override broke: fall back to the game's source for this stage.
            free(src[i]);
            src[i] = _strdup(s->orig);
            isOvr[i] = 0;
            hash[i] = fnv1a(src[i], strlen(src[i]));
            compiled = compile_into(s->id, s->type, src[i], name);
        }
        if (!compiled) {
            for (int k = 0; k < p->nsh; k++) free(src[k]);
            return 0;
        }
        shader_compiled(s->id, isOvr[i], hash[i]);
    }

    r_glLinkProgram(prog);
    GLint ok = 0;
    r_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    for (int i = 0; i < p->nsh; i++) {
        p->sh[i].overridden = isOvr[i];
        p->sh[i].curHash = hash[i];
        free(src[i]);
    }
    if (!ok) {
        char buf[2048] = {0};
        r_glGetProgramInfoLog(prog, sizeof(buf) - 1, NULL, buf);
        const char *name = p->nsh && p->sh[p->nsh - 1].idx >= 0 ? g_index[p->sh[p->nsh - 1].idx].name : "unknown";
        logf_("RELINK ERROR program %u (%s): %s", prog, name, buf);
        return -1;
    }

    refresh_uniforms(prog, p);
    p->blkChecked = 0;
    p->extrasChecked = 0;
    for (int i = 0; i < p->nblk; i++) {
        GLuint bi = r_glGetUniformBlockIndex(prog, p->blk[i].name);
        if (bi != GL_INVALID_INDEX) r_glUniformBlockBinding(prog, bi, p->blk[i].binding);
    }
    r_glUseProgram(prog);
    for (int i = 0; i < p->nuni; i++) replay_uniform(&p->uni[i]);
    return 2;
}

// ----------------------------------------------------------------------------------------
// Texture sharing between passes (shaderpack/share.txt):
//   <SourceProgram> <sourceSampler> <DestProgram> <destSampler> <unit>
// The texture bound to the source program's sampler is re-bound for the destination
// program on a spare texture unit, e.g. the sun shadow map for volumetric light.

#define GL_TEXTURE0 0x84C0
#ifdef __APPLE__
// Hytale's macOS build binds its own textures to units 16-18. A pack sampler of another type on
// the same unit is an error there and the draw is dropped (water vanished), so the hook's own
// textures (materials, emission, SSS masks) start at 32, which macOS's 80 combined units allow.
#define MAX_UNITS 48
#define HOOK_UNIT_BASE 32
#else
#define MAX_UNITS 32
#define HOOK_UNIT_BASE 16
#endif
typedef struct { GLenum target; GLuint tex; GLuint sampler; } UnitState;
static UnitState g_units[MAX_UNITS];
static GLuint g_activeUnit;
static GLuint g_maxGameUnit;

typedef void (APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glActiveTexture)(GLenum);
typedef void (APIENTRY *PFN_glBindSampler)(GLuint, GLuint);
static PFN_glBindTexture r_glBindTexture;
static PFN_glActiveTexture r_glActiveTexture;
static PFN_glBindSampler r_glBindSampler;

typedef struct {
    char src[64], srcSampler[64], dst[64], dstSampler[64];
    GLuint unit;
    int raw;             // bind with our own sampler: no depth comparison, nearest filtering
    int valid;
    GLenum target;
    GLuint tex, sampler;
} Share;
static Share g_shares[16];
static int g_nShares;
static int g_shareGen = 1;

#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
typedef void (APIENTRY *PFN_glGenSamplers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glSamplerParameteri)(GLuint, GLenum, GLint);
static GLuint g_rawSampler;

static GLuint get_raw_sampler(void)
{
    if (g_rawSampler) return g_rawSampler;
    PFN_glGenSamplers gen = (PFN_glGenSamplers)real_wglGetProcAddress("glGenSamplers");
    PFN_glSamplerParameteri par = (PFN_glSamplerParameteri)real_wglGetProcAddress("glSamplerParameteri");
    if (!gen || !par) return 0;
    gen(1, &g_rawSampler);
    par(g_rawSampler, GL_TEXTURE_COMPARE_MODE, 0);
    par(g_rawSampler, 0x2801 /*MIN_FILTER*/, GL_NEAREST);
    par(g_rawSampler, 0x2800 /*MAG_FILTER*/, GL_NEAREST);
    par(g_rawSampler, 0x2802 /*WRAP_S*/, GL_CLAMP_TO_EDGE);
    par(g_rawSampler, 0x2803 /*WRAP_T*/, GL_CLAMP_TO_EDGE);
    return g_rawSampler;
}

static void load_shares(void)
{
    g_nShares = 0;
    char *txt = read_file("share.txt", NULL);
    if (!txt) return;
    char *save = NULL;
    for (char *line = strtok_s(txt, "\n", &save); line; line = strtok_s(NULL, "\n", &save)) {
        if (line[0] == '#' || g_nShares >= 16) continue;
        Share s;
        memset(&s, 0, sizeof(s));
        unsigned unit = 0;
        char flag[16] = "";
        int n = sscanf(line, "%63s %63s %63s %63s %u %15s", s.src, s.srcSampler, s.dst, s.dstSampler, &unit, flag);
        if (n >= 5 && unit < MAX_UNITS) {
            s.unit = unit;
            s.raw = !strcmp(flag, "raw");
            g_shares[g_nShares++] = s;
        }
    }
    free(txt);
    g_shareGen++;
    logf_("shares: %d", g_nShares);
}

static void APIENTRY hook_glActiveTexture(GLenum t)
{
    g_activeUnit = t - GL_TEXTURE0;
    if (g_activeUnit < MAX_UNITS && g_activeUnit > g_maxGameUnit) {
        g_maxGameUnit = g_activeUnit;
        logf_("game uses texture unit %u", g_maxGameUnit);
    }
    r_glActiveTexture(t);
}

AU_GL_EXPORT void APIENTRY glBindTexture(GLenum target, GLuint tex)
{
    if (g_activeUnit < MAX_UNITS) { g_units[g_activeUnit].target = target; g_units[g_activeUnit].tex = tex; }
    r_glBindTexture(target, tex);
}

static void APIENTRY hook_glBindSampler(GLuint unit, GLuint sampler)
{
    if (unit < MAX_UNITS) g_units[unit].sampler = sampler;
    r_glBindSampler(unit, sampler);
}

static void capture_shares(ProgRec *prev)
{
    for (int i = 0; i < g_nShares; i++) {
        Share *s = &g_shares[i];
        if (strcmp(prev->pname, s->src)) continue;
        for (int k = 0; k < prev->nuni; k++) {
            UniformRec *u = &prev->uni[k];
            if (strcmp(u->name, s->srcSampler) || u->fn != U_1I) continue;
            GLint unit = *(GLint *)u->data;
            if (unit >= 0 && unit < MAX_UNITS && g_units[unit].tex) {
                if (!s->valid || s->tex != g_units[unit].tex)
                    logf_("share %s.%s captured: tex %u target 0x%x (unit %d)", s->src, s->srcSampler, g_units[unit].tex, g_units[unit].target, unit);
                s->target = g_units[unit].target;
                s->tex = g_units[unit].tex;
                s->sampler = g_units[unit].sampler;
                s->valid = 1;
            }
        }
    }
}

static int is_share_src(ProgRec *p)
{
    if (p->shareGen != g_shareGen) {
        p->shareGen = g_shareGen;
        p->shareSrc = 0;
        for (int i = 0; i < g_nShares; i++) if (!strcmp(p->pname, g_shares[i].src)) p->shareSrc = 1;
    }
    return p->shareSrc;
}

static void temporal_prepare(ProgRec *program);

// Shares are captured at draw time: by the next glUseProgram the game has often re-bound the
// unit for the following pass (the shadow map's unit holds the scene depth by then).
static void share_draw_capture(void)
{
    if (!g_nShares || !g_curProg) return;
    ProgRec *p = prog_rec(g_curProg, 0);
    if (!p || !p->linked || !is_share_src(p)) return;
    EnterCriticalSection(&g_lock);
    capture_shares(p);
    LeaveCriticalSection(&g_lock);
}

typedef void (APIENTRY *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
typedef void (APIENTRY *PFN_glDrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glDrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);
static PFN_glDrawArrays r_glDrawArrays;
static PFN_glDrawElements r_glDrawElements;
static PFN_glDrawArraysInstanced r_glDrawArraysInstanced;
static PFN_glDrawElementsInstanced r_glDrawElementsInstanced;

static GLuint g_giProg;   // program that wants the GI history bound at its draw (see gi_bind)
static void gi_bind(int temporal, int raw, int divisor, int compact, int lights);
static void profile_use(const char *name);
static void gi_release(void);
static void gi_draw(GLenum mode, GLint first, GLsizei count);
static GLuint g_dlssProg; // the program writing the DLSS inputs (PostEffectFS, see dlss_draw)
static int g_dlssRR;
static int g_dlssPending; // inputs written this frame, DLSS runs at the present (dlss_present)
static void dlss_draw(GLenum mode, GLint first, GLsizei count);
static int dlss_present(void);
static int dlss_present_rect(int x, int y, int w, int h, const char *how, int direct);
static float g_jitterNdc[2], g_jitterPx[2], g_jitterNextPx[2];   // see jitter_next_frame
static float g_sceneJitter[2];
// Hook-side settings (AU_Settings.glsl, read at every reload): DLSS jitter on/off and the signs
// of the jitter / motion handed to DLSS (axis conventions).
static double g_setDlssJitter = 1, g_setJitterSX = 1, g_setJitterSY = 1, g_setMotionSX = 1, g_setMotionSY = 1;
static double g_setDlssDebug = 0;   // 1 = show DLSS's colour input, 2 = reset every frame, 3 depth, 4 motion
static double g_setDlssMode = 1;    // AU_DLSS: 1 = Super Resolution, 2 = Ray Reconstruction
static double g_setDlssPreset = 0;  // AU_DLSS_PRESET: 0 NVIDIA's default model, 1 sharpest, 2 less ghosting
static double g_setDlssRRPreset = 0;
static double g_setDlssSharpness = 0.2;
static int g_setRenderScale, g_setDlssEnabled;
static int g_setFrameGen;
static int g_setVoxelColors;
static int g_temporalFrame = -1, g_temporalValid;
static float g_temporalScene[152], g_temporalPreviousScene[152];
static void framegen_before_ui(ProgRec *program);
static void framegen_after_scene(ProgRec *program);
#if AU_HAS_DLSS
static int g_dlssPresentedFrame = -100;   // last frame DLSS's output reached the window
#endif
static int g_postPresent;                 // the frame is on the window: UI follows, no jitter

// Terrain program in use (materials): the atlas is bound by draw time, not at glUseProgram.
static ProgRec *g_matProgRec;
static void materials_draw_check(void);
static void materials_use(ProgRec *p);
typedef struct { GLint viewport[4], scissor[4]; int active, fragmentLocation; } RenderUIState;
static RenderUIState render_ui_begin(void);
static void render_ui_end(RenderUIState state);

AU_GL_EXPORT void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    ProgRec *program = g_curProg ? prog_rec(g_curProg, 0) : NULL;
    if (program) temporal_prepare(program);
    share_draw_capture();
    if (g_matProgRec) materials_draw_check();
    if (g_giProg && g_giProg == g_curProg) { gi_draw(mode, first, count); return; }
    if (g_dlssProg && g_dlssProg == g_curProg) { dlss_draw(mode, first, count); return; }
    if (g_dlssPending && dlss_present()) return;
    framegen_before_ui(program);
    RenderUIState ui = render_ui_begin();
    r_glDrawArrays(mode, first, count);
    render_ui_end(ui);
    framegen_after_scene(program);
}

AU_GL_EXPORT void APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    ProgRec *program = g_curProg ? prog_rec(g_curProg, 0) : NULL;
    if (program) temporal_prepare(program);
    share_draw_capture();
    if (g_matProgRec) materials_draw_check();
    if (g_dlssPending && dlss_present()) return;
    framegen_before_ui(program);
    RenderUIState ui = render_ui_begin();
    r_glDrawElements(mode, count, type, indices);
    render_ui_end(ui);
    framegen_after_scene(program);
}

static void APIENTRY hook_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei inst)
{
    ProgRec *program = g_curProg ? prog_rec(g_curProg, 0) : NULL;
    if (program) temporal_prepare(program);
    share_draw_capture();
    if (g_matProgRec) materials_draw_check();
    framegen_before_ui(program);
    RenderUIState ui = render_ui_begin();
    r_glDrawArraysInstanced(mode, first, count, inst);
    render_ui_end(ui);
}

static void APIENTRY hook_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst)
{
    ProgRec *program = g_curProg ? prog_rec(g_curProg, 0) : NULL;
    if (program) temporal_prepare(program);
    share_draw_capture();
    if (g_matProgRec) materials_draw_check();
    framegen_before_ui(program);
    RenderUIState ui = render_ui_begin();
    r_glDrawElementsInstanced(mode, count, type, indices, inst);
    render_ui_end(ui);
}

static void apply_shares(GLuint prog, ProgRec *p)
{
    for (int i = 0; i < g_nShares; i++) {
        Share *s = &g_shares[i];
        if (!s->valid || strcmp(p->pname, s->dst)) continue;
        GLint loc = r_glGetUniformLocation(prog, s->dstSampler);
        if (loc < 0) continue;
        GLuint sampler = s->raw ? get_raw_sampler() : s->sampler;
        r_glActiveTexture(GL_TEXTURE0 + s->unit);
        r_glBindTexture(s->target, s->tex);
        if (r_glBindSampler) r_glBindSampler(s->unit, sampler);
        r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
        g_units[s->unit].target = s->target;
        g_units[s->unit].tex = s->tex;
        g_units[s->unit].sampler = sampler;
        r_glUniform1i(loc, (GLint)s->unit);
    }
}

#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36

// Gives uniform blocks the game never bound for this program the binding it uses elsewhere.
static void apply_known_blocks(GLuint prog, ProgRec *p)
{
    p->blkChecked = 1;
    GLint n = 0;
    r_glGetProgramiv(prog, GL_ACTIVE_UNIFORM_BLOCKS, &n);
    for (GLint b = 0; b < n; b++) {
        char name[128] = {0};
        r_glGetActiveUniformBlockName(prog, (GLuint)b, sizeof(name) - 1, NULL, name);
        int bound = 0;
        for (int i = 0; i < p->nblk; i++) if (!strcmp(p->blk[i].name, name)) bound = 1;
        if (bound) continue;
        for (int i = 0; i < g_nKnownBlocks; i++) {
            if (strcmp(g_knownBlocks[i].name, name)) continue;
            r_glUniformBlockBinding(prog, (GLuint)b, g_knownBlocks[i].binding);
            logf_("auto-bound %s for %s (program %u)", name, p->pname, prog);
        }
    }
}

static int pack_flag(const char *rel)
{
    wchar_t path[MAX_PATH];
    pack_path(path, rel);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static volatile LONG g_toggleOff;

// ----------------------------------------------------------------------------------------
// Custom textures (shaderpack/textures.txt):  <file.raw> <samplerName> <unit>
// .raw = "HYTX" + u32 width + u32 height + RGBA8 pixels (tools/png_to_raw.py).
// Any program with an active sampler of that name gets the texture on that unit.

#define GL_TEXTURE_2D         0x0DE1
#define GL_RGBA               0x1908
#define GL_RGBA8              0x8058
#define GL_UNSIGNED_BYTE      0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S     0x2802
#define GL_TEXTURE_WRAP_T     0x2803
#define GL_LINEAR             0x2601
#define GL_REPEAT             0x2901
#define GL_UNPACK_ALIGNMENT   0x0CF5
#define GL_TEXTURE_BINDING_2D 0x8069

typedef void (APIENTRY *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_glPixelStorei)(GLenum, GLint);
typedef void (APIENTRY *PFN_glGenerateMipmap)(GLenum);

typedef struct { char file[64]; char sampler[64]; GLuint unit; GLuint tex; int loaded; } CustomTex;
static CustomTex g_ctex[8];
static int g_nCtex;

static void load_textures_cfg(void)
{
    static PFN_glDeleteTextures del;
    if (!del) del = (PFN_glDeleteTextures)GetProcAddress(g_real, "glDeleteTextures");
    for (int i = 0; i < g_nCtex; i++) if (g_ctex[i].tex) del(1, &g_ctex[i].tex);
    g_nCtex = 0;
    char *txt = read_file("textures.txt", NULL);
    if (!txt) return;
    char *save = NULL;
    for (char *line = strtok_s(txt, "\n", &save); line; line = strtok_s(NULL, "\n", &save)) {
        if (line[0] == '#' || g_nCtex >= 8) continue;
        CustomTex t;
        memset(&t, 0, sizeof(t));
        unsigned unit = 0;
        if (sscanf(line, "%63s %63s %u", t.file, t.sampler, &unit) == 3 && unit < MAX_UNITS) {
            t.unit = unit;
            g_ctex[g_nCtex++] = t;
        }
    }
    free(txt);
}

static void ensure_textures(void)
{
    static PFN_glGenTextures gen;
    static PFN_glTexImage2D img;
    static PFN_glTexParameteri par;
    static PFN_glPixelStorei pst;
    if (!gen) {
        gen = (PFN_glGenTextures)GetProcAddress(g_real, "glGenTextures");
        img = (PFN_glTexImage2D)GetProcAddress(g_real, "glTexImage2D");
        par = (PFN_glTexParameteri)GetProcAddress(g_real, "glTexParameteri");
        pst = (PFN_glPixelStorei)GetProcAddress(g_real, "glPixelStorei");
    }
    for (int i = 0; i < g_nCtex; i++) {
        CustomTex *t = &g_ctex[i];
        if (t->loaded) continue;
        t->loaded = 1;
        char rel[96];
        snprintf(rel, sizeof(rel), "textures\\%s", t->file);
        size_t len = 0;
        unsigned char *raw = (unsigned char *)read_file(rel, &len);
        if (!raw || len < 12 || memcmp(raw, "HYTX", 4)) { logf_("texture %s missing/invalid", t->file); free(raw); continue; }
        uint32_t w, h;
        memcpy(&w, raw + 4, 4);
        memcpy(&h, raw + 8, 4);
        if (len < 12 + (size_t)w * h * 4) { logf_("texture %s truncated", t->file); free(raw); continue; }
        GLint prevTex = 0;
        r_glActiveTexture(GL_TEXTURE0 + t->unit);
        r_glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        gen(1, &t->tex);
        r_glBindTexture(GL_TEXTURE_2D, t->tex);
        pst(GL_UNPACK_ALIGNMENT, 1);
        img(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, raw + 12);
        pst(GL_UNPACK_ALIGNMENT, 4);
        // Mipmaps + anisotropic filtering: Hytale's TAA doesn't jitter, so unfiltered
        // high-frequency lookups (water normals) alias into grain at a distance.
        static PFN_glGenerateMipmap genMips;
        if (!genMips) genMips = (PFN_glGenerateMipmap)real_wglGetProcAddress("glGenerateMipmap");
        if (genMips) {
            genMips(GL_TEXTURE_2D);
            par(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2703 /*LINEAR_MIPMAP_LINEAR*/);
            par(GL_TEXTURE_2D, 0x84FE /*TEXTURE_MAX_ANISOTROPY*/, 16);
        } else {
            par(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
        par(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        par(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        par(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        r_glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
        free(raw);
        logf_("texture %s loaded (%ux%u) -> %s unit %u", t->file, w, h, t->sampler, t->unit);
    }
}

// ----------------------------------------------------------------------------------------
// Voxel grid for the path tracer (shaderpack/include/AU_Voxel.glsl): two 3D images that
// live for the whole session, auVoxGeo (R32UI) on image unit 6 and auVoxCol (RGBA8) on
// unit 7. The game only uses image units 0-1 (its OIT k-buffer). Programs that declare
// those images get the units assigned, and a memory barrier separates the passes that
// write the grid (MapChunk) from the ones that read it.

#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_BINDING_3D 0x806A
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_R32UI 0x8236
#define GL_RED_INTEGER 0x8D94
#define GL_UNSIGNED_INT 0x1405
#define GL_READ_WRITE 0x88BA
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x20
#define VOX_X 256
#define VOX_Y 128
#define VOX_Z 256
#define VOX_UNIT_GEO 6
#define VOX_UNIT_COL 7
#define VOX_UNIT_LIGHTS 3
#define VOX_LIGHTS_W 2049
#define VOX_LIGHTS_H 37

typedef void (APIENTRY *PFN_glTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFN_glBindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
typedef void (APIENTRY *PFN_glMemoryBarrier)(GLuint);
typedef void (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef GLenum (APIENTRY *PFN_glGetError)(void);
static PFN_glBindImageTexture r_glBindImageTexture;
static PFN_glMemoryBarrier r_glMemoryBarrier;
static GLuint g_voxTex[2];
static GLuint g_voxLights;
static int g_voxState;            // 0 not created yet, 1 ready, -1 unavailable
static char g_voxLastUser[64];    // pname of the last program that used the grid

static void vox_create(void)
{
    g_voxState = -1;
    PFN_glTexImage3D img3d = (PFN_glTexImage3D)real_wglGetProcAddress("glTexImage3D");
    PFN_glBindBuffer bindBuf = (PFN_glBindBuffer)real_wglGetProcAddress("glBindBuffer");
    PFN_glGenTextures gen = (PFN_glGenTextures)GetProcAddress(g_real, "glGenTextures");
    PFN_glTexParameteri par = (PFN_glTexParameteri)GetProcAddress(g_real, "glTexParameteri");
    PFN_glGetError getErr = (PFN_glGetError)GetProcAddress(g_real, "glGetError");
    r_glBindImageTexture = (PFN_glBindImageTexture)real_wglGetProcAddress("glBindImageTexture");
    r_glMemoryBarrier = (PFN_glMemoryBarrier)real_wglGetProcAddress("glMemoryBarrier");
    if (!img3d || !bindBuf || !r_glBindImageTexture || !r_glMemoryBarrier) { logf_("voxels: image load/store not available"); return; }
    void *zero = calloc((size_t)VOX_X * VOX_Y * VOX_Z, 4);
    if (!zero) return;
    while (getErr() != 0) {}
    GLint prevTex = 0, prevPbo = 0;
    r_glGetIntegerv(GL_TEXTURE_BINDING_3D, &prevTex);
    r_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevPbo);
    if (prevPbo) bindBuf(GL_PIXEL_UNPACK_BUFFER, 0);
    gen(g_setVoxelColors ? 2 : 1, g_voxTex);
    for (int i = 0; i < (g_setVoxelColors ? 2 : 1); i++) {
        r_glBindTexture(GL_TEXTURE_3D, g_voxTex[i]);
        par(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        par(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        par(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
        if (i == 0) img3d(GL_TEXTURE_3D, 0, GL_R32UI, VOX_X, VOX_Y, VOX_Z, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, zero);
        else img3d(GL_TEXTURE_3D, 0, GL_RGBA8, VOX_X, VOX_Y, VOX_Z, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    }
    r_glBindTexture(GL_TEXTURE_3D, (GLuint)prevTex);
    PFN_glTexImage2D img2d = (PFN_glTexImage2D)GetProcAddress(g_real, "glTexImage2D");
    GLint prev2d = 0;
    r_glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2d);
    gen(1, &g_voxLights);
    r_glBindTexture(GL_TEXTURE_2D, g_voxLights);
    par(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    par(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    par(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    img2d(GL_TEXTURE_2D, 0, GL_R32UI, VOX_LIGHTS_W, VOX_LIGHTS_H, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, zero);
    r_glBindTexture(GL_TEXTURE_2D, (GLuint)prev2d);
    if (prevPbo) bindBuf(GL_PIXEL_UNPACK_BUFFER, (GLuint)prevPbo);
    free(zero);
    GLenum err = getErr();
    if (err) { logf_("voxels: creating the grid failed (GL error 0x%x)", err); return; }
    g_voxState = 1;
    logf_("voxels: %dx%dx%d grid on image units %d/%d", VOX_X, VOX_Y, VOX_Z, VOX_UNIT_GEO, VOX_UNIT_COL);
}

// Empties the grid; shader reloads do this, since the shaders that fill it may have changed.
static void vox_clear(void)
{
    typedef void (APIENTRY *PFN_glClearTexImage)(GLuint, GLint, GLenum, GLenum, const void *);
    static PFN_glClearTexImage clr;
    if (g_voxState != 1) return;
    if (!clr) clr = (PFN_glClearTexImage)real_wglGetProcAddress("glClearTexImage");
    if (!clr) return;
    clr(g_voxTex[0], 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    if (g_voxTex[1]) clr(g_voxTex[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (g_voxLights) clr(g_voxLights, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
}

// Temporal accumulation for the path tracer (DeferredFS): two ping-pong sets of
// screen-sized textures, radiance (RGBA16F: bounce rgb, sky visibility), the world position
// each pixel was accumulated for (RGBA32F: xyz, history length) and block light (RGBA16F,
// torches). Each frame the shader reads last frame's set as textures (units 21/22/20) and
// writes this frame's through images (4/5/2).
// Sized from the viewport at the draw, since the program switch comes before it.
#define GL_RGBA16F 0x881A
#define GL_RGBA32F 0x8814
#define GL_FLOAT 0x1406
#define GL_VIEWPORT 0x0BA2
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x8
#define GI_IMG_RAD 4
#define GI_IMG_POS 5
#define GI_IMG_LIGHT 2
#define GI_TEX_RAD 21
#define GI_TEX_POS 22
#define GI_TEX_LIGHT 20
#define GI_TEX_RAW_RAD 17
#define GI_TEX_RAW_POS 19
#define GI_TEX_RAW_LIGHT 31
static int g_frame;            // counted at PostEffectFS (defined with the uniform forwarding)
static GLuint g_giTex[2][3];   // [frame parity][0 radiance, 1 position, 2 block light]
static GLuint g_giRawTex[3];
static int g_giW, g_giH;

static int g_giTemporal;
static int g_giRaw, g_setGIHalf;
static int g_giCompact, g_giLights;

static void gi_release(void)
{
    if (!g_giTex[0][0] && !g_giRawTex[0]) return;
    PFN_glDeleteTextures del = (PFN_glDeleteTextures)GetProcAddress(g_real, "glDeleteTextures");
    static const GLuint units[6] = {GI_TEX_RAD, GI_TEX_POS, GI_TEX_LIGHT,
                                   GI_TEX_RAW_RAD, GI_TEX_RAW_POS, GI_TEX_RAW_LIGHT};
    for (int index = 0; index < 6; index++) {
        GLuint unit = units[index];
        r_glActiveTexture(GL_TEXTURE0 + unit);
        r_glBindTexture(GL_TEXTURE_2D, 0);
        if (r_glBindSampler) r_glBindSampler(unit, 0);
        g_units[unit].target = GL_TEXTURE_2D;
        g_units[unit].tex = 0;
        g_units[unit].sampler = 0;
    }
    r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
    if (r_glBindImageTexture) {
        r_glBindImageTexture(GI_IMG_RAD, 0, 0, 0, 0, GL_READ_WRITE, GL_RGBA16F);
        r_glBindImageTexture(GI_IMG_POS, 0, 0, 0, 0, GL_READ_WRITE, GL_RGBA32F);
        r_glBindImageTexture(GI_IMG_LIGHT, 0, 0, 0, 0, GL_READ_WRITE, GL_RGBA16F);
    }
    if (g_giTex[0][0]) del(6, &g_giTex[0][0]);
    if (g_giRawTex[0]) del(3, g_giRawTex);
    memset(g_giTex, 0, sizeof(g_giTex));
    memset(g_giRawTex, 0, sizeof(g_giRawTex));
    g_giW = g_giH = g_giTemporal = g_giRaw = g_giCompact = g_giLights = 0;
    logf_("GI denoiser buffers released");
}

static void gi_bind(int temporal, int raw, int divisor, int compact, int lights)
{
    typedef void (APIENTRY *PFN_glClearTexImage)(GLuint, GLint, GLenum, GLenum, const void *);
    static PFN_glClearTexImage clr;
    if (!r_glBindImageTexture) r_glBindImageTexture = (PFN_glBindImageTexture)real_wglGetProcAddress("glBindImageTexture");
    if (!r_glMemoryBarrier) r_glMemoryBarrier = (PFN_glMemoryBarrier)real_wglGetProcAddress("glMemoryBarrier");
    if (!r_glBindImageTexture || !r_glMemoryBarrier) return;
    GLint viewport[4] = {0};
    r_glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    int width = (viewport[2] + divisor - 1) / divisor, height = (viewport[3] + divisor - 1) / divisor;
    if (width != g_giW || height != g_giH || temporal != g_giTemporal || raw != g_giRaw || compact != g_giCompact || lights != g_giLights) {
        gi_release();
        PFN_glGenTextures gen = (PFN_glGenTextures)GetProcAddress(g_real, "glGenTextures");
        PFN_glTexImage2D img = (PFN_glTexImage2D)GetProcAddress(g_real, "glTexImage2D");
        PFN_glTexParameteri par = (PFN_glTexParameteri)GetProcAddress(g_real, "glTexParameteri");
        if (!clr) clr = (PFN_glClearTexImage)real_wglGetProcAddress("glClearTexImage");
        GLint previousTexture = 0;
        r_glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        for (int index = 0; index < (temporal ? 9 : 3); index++) {
            if (index < 3 && !raw) continue;
            int channel = index % 3;
            if (channel == 2 && !lights) continue;
            GLuint *handle = index < 3 ? &g_giRawTex[channel] : &g_giTex[(index - 3) / 3][channel];
            gen(1, handle);
            GLuint texture = *handle;
            r_glBindTexture(GL_TEXTURE_2D, texture);
            par(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            par(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            par(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            GLenum format = channel == 1 && compact ? 0x8230 : channel == 1 ? GL_RGBA32F : GL_RGBA16F;
            GLenum components = channel == 1 && compact ? 0x8227 : GL_RGBA;
            img(GL_TEXTURE_2D, 0, format, width, height, 0, components, GL_FLOAT, NULL);
            if (clr) clr(texture, 0, components, GL_FLOAT, NULL);
        }
        r_glBindTexture(GL_TEXTURE_2D, (GLuint)previousTexture);
        g_giW = width;
        g_giH = height;
        g_giTemporal = temporal;
        g_giRaw = raw;
        g_giCompact = compact;
        g_giLights = lights;
        int channels = lights ? 3 : 2;
        int bytes = 8 + (compact ? 8 : 16) + (lights ? 8 : 0);
        logf_("GI denoiser %dx%d: raw=%d history=%d compact=%d memory=%.1f MiB", g_giW, g_giH, raw ? channels : 0, temporal ? channels * 2 : 0, compact,
              (double)g_giW * g_giH * bytes * (raw + temporal * 2) / 1048576.0);
    }
    if (!temporal) return;
    int current = g_frame & 1;
    r_glBindImageTexture(GI_IMG_RAD, g_giTex[current][0], 0, 0, 0, GL_READ_WRITE, GL_RGBA16F);
    r_glBindImageTexture(GI_IMG_POS, g_giTex[current][1], 0, 0, 0, GL_READ_WRITE, compact ? 0x8230 : GL_RGBA32F);
    r_glBindImageTexture(GI_IMG_LIGHT, g_giTex[current][2], 0, 0, 0, GL_READ_WRITE, GL_RGBA16F);
    static const GLuint historyUnits[3] = {GI_TEX_RAD, GI_TEX_POS, GI_TEX_LIGHT};
    for (int channel = 0; channel < 3; channel++) {
        GLuint unit = historyUnits[channel];
        r_glActiveTexture(GL_TEXTURE0 + unit);
        r_glBindTexture(GL_TEXTURE_2D, g_giTex[current ^ 1][channel]);
        if (r_glBindSampler) r_glBindSampler(unit, 0);
        g_units[unit].target = GL_TEXTURE_2D;
        g_units[unit].tex = g_giTex[current ^ 1][channel];
        g_units[unit].sampler = 0;
    }
    r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
}

static void gi_draw(GLenum mode, GLint first, GLsizei count)
{
    ProgRec *program = prog_rec(g_curProg, 0);
    if (!program || program->locPTPass < 0 || (program->locVoxGeo >= 0 && g_voxState != 1)) {
        r_glDrawArrays(mode, first, count);
        return;
    }
    int useHistory = program->giTemporal;
    int rayReconstruction = 0;
#if AU_HAS_DLSS
    if (g_setDlssEnabled && g_setDlssMode == 2 && g_dlssState == 1 && g_dlssRR
        && g_frame - g_dlssPresentedFrame <= 2) { useHistory = 0; rayReconstruction = 1; }
#endif
    if (useHistory) gi_bind(1, 0, 1, 0, program->giLights);
    else gi_release();
    if (useHistory && !g_giTex[0][0]) useHistory = 0;
    if (program->locVoxCol >= 0) {
        profile_use("GI voxel feed");
        r_glUniform1i(program->locPTPass, 1);
        r_glDrawArrays(mode, first, count);
        r_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }
    if (program->locLocalLightTiles >= 0) {
        static void (APIENTRY *setViewport)(GLint, GLint, GLsizei, GLsizei);
        static GLboolean (APIENTRY *isEnabled)(GLenum);
        static void (APIENTRY *enable)(GLenum), (APIENTRY *disable)(GLenum);
        if (!setViewport) {
            setViewport = (void *)GetProcAddress(g_real, "glViewport");
            isEnabled = (void *)GetProcAddress(g_real, "glIsEnabled");
            enable = (void *)GetProcAddress(g_real, "glEnable");
            disable = (void *)GetProcAddress(g_real, "glDisable");
        }
        GLint viewport[4];
        r_glGetIntegerv(GL_VIEWPORT, viewport);
        GLboolean scissor = isEnabled(0x0c11);
        if (scissor) disable(0x0c11);
        profile_use("GI local light culling");
        r_glUniform1i(program->locLocalLightTiles, 2);
        r_glUniform1i(program->locPTPass, 2);
        setViewport(0, 0, 32, 2048);
        r_glDrawArrays(mode, first, count);
        r_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (scissor) enable(0x0c11);
    }
    profile_use(useHistory ? "GI Original + lighting" : "GI lighting");
    r_glUniform1i(program->locPTPass, rayReconstruction ? 5 : useHistory ? 3 : 4);
    r_glDrawArrays(mode, first, count);
    if (useHistory) r_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    r_glUniform1i(program->locPTPass, 0);
    if (program->locLocalLightTiles >= 0) r_glUniform1i(program->locLocalLightTiles, 0);
}
// ----------------------------------------------------------------------------------------
// Block materials. Hytale's terrain has no material data, so the hook reads its block atlas
// (the texture array MapChunkFS samples as uTexture) once, fingerprints every 32x32 tile and
// looks the fingerprints up in shaderpack/materials.txt (the game's own block textures,
// fingerprinted and classified by name at install: tools/prepare_shaders.ps1). The result is a
// small texture array, one RG8 texel per tile (class, level), bound for the terrain shader as
// uHookMaterials.

#define MAT_UNIT (HOOK_UNIT_BASE + 2)
#define GL_TEXTURE_2D_ARRAY_ 0x8C1A
#define GL_TEXTURE_BINDING_2D_ARRAY_ 0x8C1D
#define GL_TEXTURE_WIDTH_ 0x1000
#define GL_TEXTURE_HEIGHT_ 0x1001
#define GL_TEXTURE_DEPTH_ 0x8071
#define GL_TEXTURE_INTERNAL_FORMAT_ 0x1003
#define GL_PIXEL_PACK_BUFFER_ 0x88EB
#define GL_PIXEL_UNPACK_BUFFER_ 0x88EC
#define GL_PIXEL_PACK_BUFFER_BINDING_ 0x88ED
#define GL_PIXEL_UNPACK_BUFFER_BINDING_ 0x88EF
#define GL_PACK_ALIGNMENT_ 0x0D05
#define GL_UNPACK_ALIGNMENT_ 0x0CF5
#define GL_UNPACK_ROW_LENGTH_ 0x0CF2
#define GL_UNPACK_IMAGE_HEIGHT_ 0x806E
#define GL_RG8_ 0x822B
#define GL_RG_ 0x8227

typedef void (APIENTRY *PFN_matGetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
typedef void (APIENTRY *PFN_matGetTexImage)(GLenum, GLint, GLenum, GLenum, void *);
typedef void (APIENTRY *PFN_matPixelStorei)(GLenum, GLint);
typedef void (APIENTRY *PFN_matTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFN_matBindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_matGenTextures)(GLsizei, GLuint *);

static unsigned *g_matKey;              // fingerprints, sorted
static unsigned char (*g_matVal)[2];    // class, level
static int g_nMat = -1;                 // -1 = materials.txt not read yet
static GLuint g_matTex, g_matAtlas;
static int g_matTries;
static LARGE_INTEGER g_matNextTry;
static LARGE_INTEGER g_freq;           // (defined with the frame timer below)
#include "emissions.h"
#include "sss_masks.h"
static FILETIME g_materialsStamp, g_emissionsStamp, g_sssMasksStamp;

static FILETIME material_file_stamp(const char *relative)
{
    wchar_t path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA data;
    FILETIME missing = {0};
    pack_path(path, relative);
    return GetFileAttributesExW(path, GetFileExInfoStandard, &data) ? data.ftLastWriteTime : missing;
}

static void materials_load(void)
{
    emissions_load();
    sss_masks_load();
    g_materialsStamp = material_file_stamp("materials.txt");
    g_emissionsStamp = material_file_stamp("emitters.txt");
    g_sssMasksStamp = material_file_stamp("sss-masks.bin");
    g_nMat = 0;
    char *txt = read_file("materials.txt", NULL);
    if (!txt) { logf_("materials: no materials.txt, blocks stay plain"); return; }
    int cap = 0;
    for (const char *c = txt; *c; c++) if (*c == '\n') cap++;
    g_matKey = (unsigned *)malloc(sizeof(unsigned) * (cap + 1));
    g_matVal = malloc(2 * (cap + 1));
    char *save = NULL;
    for (char *line = strtok_s(txt, "\n", &save); line; line = strtok_s(NULL, "\n", &save)) {
        unsigned h;
        int cls, lvl;
        if (line[0] == '#' || sscanf(line, "%x %d %d", &h, &cls, &lvl) != 3 || g_nMat > cap) continue;
        g_matKey[g_nMat] = h;
        g_matVal[g_nMat][0] = (unsigned char)cls;
        g_matVal[g_nMat][1] = (unsigned char)lvl;
        g_nMat++;
    }
    free(txt);
    logf_("materials: %d block textures", g_nMat);
}

// Hot reload: read materials.txt again and rebuild the map at the next terrain draw.
static void materials_reset(void)
{
    FILETIME materials = material_file_stamp("materials.txt");
    FILETIME emissions = material_file_stamp("emitters.txt");
    FILETIME masks = material_file_stamp("sss-masks.bin");
    if (g_nMat >= 0 && CompareFileTime(&materials, &g_materialsStamp) == 0
        && CompareFileTime(&emissions, &g_emissionsStamp) == 0
        && CompareFileTime(&masks, &g_sssMasksStamp) == 0) return;
    free(g_matKey);
    free(g_matVal);
    g_matKey = NULL;
    g_matVal = NULL;
    g_nMat = -1;
    g_matAtlas = 0;
    g_matTries = 0;
}

static int materials_find(unsigned h)
{
    int lo = 0, hi = g_nMat - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_matKey[mid] == h) return mid;
        if (g_matKey[mid] < h) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void *mat_proc(const char *name)
{
    void *p = (void *)real_wglGetProcAddress(name);
    if (!p || (uintptr_t)p <= 3 || p == (void *)-1) p = (void *)GetProcAddress(g_real, name);
    return p;
}

// Reads the atlas and (re)builds g_matTex. Leaves MAT_UNIT bound to g_matTex.
static void materials_build(GLuint atlas)
{
    static PFN_matGetTexLevelParameteriv getLevel;
    static PFN_matGetTexImage getImage;
    static PFN_matPixelStorei pixelStore;
    static PFN_matTexImage3D texImage3D;
    static PFN_matBindBuffer bindBuffer;
    static PFN_matGenTextures genTextures;
    static PFN_glTexParameteri texParam;
    if (!getLevel) {
        getLevel = (PFN_matGetTexLevelParameteriv)mat_proc("glGetTexLevelParameteriv");
        getImage = (PFN_matGetTexImage)mat_proc("glGetTexImage");
        pixelStore = (PFN_matPixelStorei)mat_proc("glPixelStorei");
        texImage3D = (PFN_matTexImage3D)mat_proc("glTexImage3D");
        bindBuffer = (PFN_matBindBuffer)mat_proc("glBindBuffer");
        genTextures = (PFN_matGenTextures)mat_proc("glGenTextures");
        texParam = (PFN_glTexParameteri)mat_proc("glTexParameteri");
    }
    if (!getLevel || !getImage || !pixelStore || !texImage3D || !bindBuffer || !genTextures || !texParam) return;
    g_matAtlas = atlas;
    g_matTries++;

    r_glActiveTexture(GL_TEXTURE0 + MAT_UNIT);
    r_glBindTexture(GL_TEXTURE_2D_ARRAY_, atlas);
    GLint w = 0, h = 0, d = 0, fmt = 0;
    getLevel(GL_TEXTURE_2D_ARRAY_, 0, GL_TEXTURE_WIDTH_, &w);
    getLevel(GL_TEXTURE_2D_ARRAY_, 0, GL_TEXTURE_HEIGHT_, &h);
    getLevel(GL_TEXTURE_2D_ARRAY_, 0, GL_TEXTURE_DEPTH_, &d);
    getLevel(GL_TEXTURE_2D_ARRAY_, 0, GL_TEXTURE_INTERNAL_FORMAT_, &fmt);
    int tx = w / 32, ty = h / 32;
    unsigned char *px = (tx > 0 && ty > 0 && d > 0) ? (unsigned char *)malloc((size_t)w * h * d * 4) : NULL;
    unsigned char *map = px ? (unsigned char *)calloc((size_t)tx * ty * d, 4) : NULL;
    unsigned char *emissionMap = px ? calloc((size_t)tx * ty * d * EMISSION_VARIANTS * 2, 4) : NULL;
    int found = 0, flipped = 0;
    if (map) {
        GLint packBuf = 0, packAlign = 4;
        const GLenum packParameters[5] = {0x0d02, 0x806c, 0x0d04, 0x0d03, 0x806b};
        GLint packValues[5];
        for (int parameter = 0; parameter < 5; parameter++) {
            r_glGetIntegerv(packParameters[parameter], &packValues[parameter]);
            pixelStore(packParameters[parameter], 0);
        }
        r_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_, &packBuf);
        r_glGetIntegerv(GL_PACK_ALIGNMENT_, &packAlign);
        if (packBuf) bindBuffer(GL_PIXEL_PACK_BUFFER_, 0);
        pixelStore(GL_PACK_ALIGNMENT_, 4);
        getImage(GL_TEXTURE_2D_ARRAY_, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int parameter = 0; parameter < 5; parameter++) pixelStore(packParameters[parameter], packValues[parameter]);
        pixelStore(GL_PACK_ALIGNMENT_, packAlign);
        if (packBuf) bindBuffer(GL_PIXEL_PACK_BUFFER_, (GLuint)packBuf);
        // Same fingerprint as BuildMaterials: FNV-1a over RGBA, top row first, transparent
        // texels as 0. Rows are tried in both orders (top row first in memory or last).
        for (int l = 0; l < d; l++)
            for (int j = 0; j < ty; j++)
                for (int i = 0; i < tx; i++) {
                    unsigned hf = 2166136261u, hr = 2166136261u;
                    for (int pass = 0; pass < 2; pass++)
                        for (int y = 0; y < 32; y++) {
                            int row = pass ? 31 - y : y;
                            const unsigned char *p = px + (((size_t)l * h + (size_t)j * 32 + row) * w + (size_t)i * 32) * 4;
                            for (int x = 0; x < 32; x++, p += 4) {
                                unsigned char c[4] = {p[0], p[1], p[2], p[3]};
                                if (!c[3]) c[0] = c[1] = c[2] = 0;
                                for (int k = 0; k < 4; k++) {
                                    if (pass) { hr ^= c[k]; hr *= 16777619u; } else { hf ^= c[k]; hf *= 16777619u; }
                                }
                            }
                        }
                    int m = materials_find(hf);
                    if (m < 0 && (m = materials_find(hr)) >= 0) flipped++;
                    emissions_tile(hf, hr, emissionMap, tx, ty, l, i, j);
                    unsigned char *o = map + (((size_t)l * ty + j) * tx + i) * 4;
                    unsigned mask = sss_mask_tile(hf, hr);
                    o[2] = (unsigned char)mask;
                    o[3] = (unsigned char)(mask >> 8);
                    if (m < 0) continue;
                    o[0] = g_matVal[m][0];
                    o[1] = g_matVal[m][1];
                    found++;
                }
        if (!g_matTex) genTextures(1, &g_matTex);
        r_glBindTexture(GL_TEXTURE_2D_ARRAY_, g_matTex);
        GLint unpackBuf = 0, unpackAlign = 4, unpackRow = 0, unpackImgH = 0;
        const GLenum unpackSkips[3] = {0x0cf4, 0x0cf3, 0x806d};
        GLint unpackSkipValues[3];
        for (int index = 0; index < 3; index++) {
            r_glGetIntegerv(unpackSkips[index], &unpackSkipValues[index]);
            pixelStore(unpackSkips[index], 0);
        }
        r_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_, &unpackBuf);
        r_glGetIntegerv(GL_UNPACK_ALIGNMENT_, &unpackAlign);
        r_glGetIntegerv(GL_UNPACK_ROW_LENGTH_, &unpackRow);
        r_glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT_, &unpackImgH);
        if (unpackBuf) bindBuffer(GL_PIXEL_UNPACK_BUFFER_, 0);
        pixelStore(GL_UNPACK_ALIGNMENT_, 1);
        pixelStore(GL_UNPACK_ROW_LENGTH_, 0);
        pixelStore(GL_UNPACK_IMAGE_HEIGHT_, 0);
        texImage3D(GL_TEXTURE_2D_ARRAY_, 0, GL_RGBA8, tx, ty, d, 0, GL_RGBA, GL_UNSIGNED_BYTE, map);
        size_t maskBytes = (size_t)(g_sssMaskCount + 1) * 1024;
        unsigned char *maskPixels = calloc(maskBytes, 1);
        if (maskPixels) {
            for (unsigned index = 0; index < g_sssMaskCount; index++)
                memcpy(maskPixels + (size_t)(index + 1) * 1024, g_sssMasks[index].pixels, 1024);
            if (!g_sssMaskTexture) genTextures(1, &g_sssMaskTexture);
            r_glBindTexture(GL_TEXTURE_2D_ARRAY_, g_sssMaskTexture);
            texImage3D(GL_TEXTURE_2D_ARRAY_, 0, 0x8229, 32, 32, g_sssMaskCount + 1, 0, 0x1903, GL_UNSIGNED_BYTE, maskPixels);
            texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAX_LEVEL, 0);
            free(maskPixels);
            r_glBindTexture(GL_TEXTURE_2D_ARRAY_, g_matTex);
        }
        if (emissionMap) {
            if (!g_emissionTexture) genTextures(1, &g_emissionTexture);
            r_glBindTexture(GL_TEXTURE_2D_ARRAY_, g_emissionTexture);
            texImage3D(GL_TEXTURE_2D_ARRAY_, 0, GL_RGBA8, tx, ty, d * EMISSION_VARIANTS * 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, emissionMap);
            texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAX_LEVEL, 0);
            r_glBindTexture(GL_TEXTURE_2D_ARRAY_, g_matTex);
        }
        pixelStore(GL_UNPACK_ALIGNMENT_, unpackAlign);
        pixelStore(GL_UNPACK_ROW_LENGTH_, unpackRow);
        pixelStore(GL_UNPACK_IMAGE_HEIGHT_, unpackImgH);
        for (int index = 0; index < 3; index++) pixelStore(unpackSkips[index], unpackSkipValues[index]);
        if (unpackBuf) bindBuffer(GL_PIXEL_UNPACK_BUFFER_, (GLuint)unpackBuf);
        texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        texParam(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    free(px);
    free(map);
    free(emissionMap);
    logf_("materials: atlas %u is %dx%dx%d (format 0x%x), %d of %d tiles recognised%s", atlas, w, h, d, fmt,
          found, tx * ty * d, flipped ? " (rows bottom-up)" : "");
    // Too few: the atlas may still be filling up (or the game changed); look again later.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_matNextTry.QuadPart = found < 40 ? now.QuadPart + g_freq.QuadPart * 5 : 0;
    r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
}

// First draw of a terrain program: make sure the material map matches the atlas on its
// uTexture unit (rebuilt when the atlas changes, or retried while it was still filling up).
static void materials_draw_check(void)
{
    ProgRec *p = g_matProgRec;
    g_matProgRec = NULL;
    if (g_nMat < 0) materials_load();
    if (g_nMat <= 0 && g_sssMaskCount == 0 && g_emissionCount == 0) return;
    GLint unit = 0;   // a sampler the game never sets reads unit 0
    for (int k = 0; k < p->nuni; k++)
        if (p->uni[k].fn == U_1I && p->uni[k].data && !strcmp(p->uni[k].name, "uTexture")) unit = *(GLint *)p->uni[k].data;
    static int logged;
    if (logged < 3 && !g_matAtlas) {
        logged++;
        logf_("materials: %s uTexture unit %d -> tex %u target 0x%x", p->pname, unit,
              unit >= 0 && unit < MAX_UNITS ? g_units[unit].tex : 0, unit >= 0 && unit < MAX_UNITS ? g_units[unit].target : 0);
    }
    if (unit >= 0 && unit < MAX_UNITS && g_units[unit].tex && g_units[unit].target == GL_TEXTURE_2D_ARRAY_) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        GLuint atlas = g_units[unit].tex;
        if (atlas != g_matAtlas || (g_matNextTry.QuadPart && now.QuadPart > g_matNextTry.QuadPart && g_matTries < 12)) {
            materials_build(atlas);
            materials_use(p);
            g_matProgRec = NULL;
        }
    }
}

// Terrain program in use: bind the material map; check the atlas at its first draw.
static void materials_use(ProgRec *p)
{
    g_matProgRec = p;
    if (p->locSSSReady >= 0) r_glUniform1i(p->locSSSReady, g_sssMaskTexture != 0 && g_matTex != 0 && g_sssMaskCount > 0);
    if (p->locSSSMasks >= 0 && g_sssMaskTexture) {
        r_glActiveTexture(GL_TEXTURE0 + SSS_MASK_UNIT);
        r_glBindTexture(GL_TEXTURE_2D_ARRAY_, g_sssMaskTexture);
        if (r_glBindSampler) r_glBindSampler(SSS_MASK_UNIT, 0);
        g_units[SSS_MASK_UNIT].target = GL_TEXTURE_2D_ARRAY_;
        g_units[SSS_MASK_UNIT].tex = g_sssMaskTexture;
        g_units[SSS_MASK_UNIT].sampler = 0;
        r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
    }
    GLint emissionReady = r_glGetUniformLocation(g_curProg, "uHookEmissionReady");
    if (emissionReady >= 0) r_glUniform1i(emissionReady, g_emissionTexture != 0);
    if (g_emissionTexture) {
        r_glActiveTexture(GL_TEXTURE0 + EMISSION_UNIT);
        r_glBindTexture(GL_TEXTURE_2D_ARRAY_, g_emissionTexture);
        if (r_glBindSampler) r_glBindSampler(EMISSION_UNIT, 0);
        g_units[EMISSION_UNIT].target = GL_TEXTURE_2D_ARRAY_;
        g_units[EMISSION_UNIT].tex = g_emissionTexture;
        g_units[EMISSION_UNIT].sampler = 0;
        r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
    }
    if (!g_matTex) return;
    r_glActiveTexture(GL_TEXTURE0 + MAT_UNIT);
    r_glBindTexture(GL_TEXTURE_2D_ARRAY_, g_matTex);
    if (r_glBindSampler) r_glBindSampler(MAT_UNIT, 0);
    g_units[MAT_UNIT].target = GL_TEXTURE_2D_ARRAY_;
    g_units[MAT_UNIT].tex = g_matTex;
    g_units[MAT_UNIT].sampler = 0;
    r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
}

static void vox_use(ProgRec *p)
{
    if (!g_voxState) vox_create();
    if (g_voxState != 1) return;
    if (p->locVoxCol >= 0 && !g_voxTex[1]) {
        PFN_glTexImage3D image = (PFN_glTexImage3D)real_wglGetProcAddress("glTexImage3D");
        PFN_glBindBuffer bindBuffer = (PFN_glBindBuffer)real_wglGetProcAddress("glBindBuffer");
        PFN_glGenTextures generate = (PFN_glGenTextures)GetProcAddress(g_real, "glGenTextures");
        PFN_glTexParameteri parameter = (PFN_glTexParameteri)GetProcAddress(g_real, "glTexParameteri");
        void *zero = calloc((size_t)VOX_X * VOX_Y * VOX_Z, 4);
        if (!image || !bindBuffer || !generate || !parameter || !zero) { free(zero); return; }
        GLint previousTexture = 0, previousBuffer = 0;
        r_glGetIntegerv(GL_TEXTURE_BINDING_3D, &previousTexture);
        r_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &previousBuffer);
        if (previousBuffer) bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        generate(1, &g_voxTex[1]);
        r_glBindTexture(GL_TEXTURE_3D, g_voxTex[1]);
        parameter(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        parameter(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        parameter(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
        image(GL_TEXTURE_3D, 0, GL_RGBA8, VOX_X, VOX_Y, VOX_Z, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        r_glBindTexture(GL_TEXTURE_3D, (GLuint)previousTexture);
        if (previousBuffer) bindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)previousBuffer);
        free(zero);
    }
    r_glBindImageTexture(VOX_UNIT_GEO, g_voxTex[0], 0, 1, 0, GL_READ_WRITE, GL_R32UI);
    r_glBindImageTexture(VOX_UNIT_COL, g_voxTex[1], 0, 1, 0, GL_READ_WRITE, GL_RGBA8);
    r_glBindImageTexture(VOX_UNIT_LIGHTS, g_voxLights, 0, 0, 0, GL_READ_WRITE, GL_R32UI);
    if (strcmp(g_voxLastUser, p->pname)) {
        r_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        snprintf(g_voxLastUser, sizeof(g_voxLastUser), "%s", p->pname);
    }
}

// ----------------------------------------------------------------------------------------
// DLSS (hook/aud_dlss.c). With AU_DLSS the pack's PostEffectFS (the last pass at render
// size) also writes the frame's colour, depth and camera motion into textures shared with a
// Vulkan device (image units 2-4). DLSS upscales them to the window in place of Hytale's own
// scale-to-window draw, so Hytale's Render Scale setting is the DLSS render size. The DLSS runtime (nvngx_dlss.dll) is loaded from shaderpack\dlss.

#if AU_HAS_DLSS
#include "aud_dlss.h"
#ifndef _WIN32
__attribute__((used)) static const char linux_dlss_marker[] = "AURORA_LINUX_DLSS_V1";
#endif
#if AU_HAS_FRAMEGEN
#include "aud_framegen.h"
#endif
#define GL_WRITE_ONLY 0x88B9
#define GL_R32F 0x822E
#define GL_RG16F 0x822F
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SCISSOR_TEST 0x0C11
#define GL_READ_FRAMEBUFFER_BINDING_ 0x8CAA
#define GL_READ_FRAMEBUFFER_ 0x8CA8
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define DLSS_IMG_COLOR 2
#define DLSS_IMG_DEPTH 3
#define DLSS_IMG_MOTION 4
// Ray Reconstruction guides (PostEffectFS from the G-buffer).
#define DLSS_IMG_ALBEDO 5
#define DLSS_IMG_SPECULAR 6
#define DLSS_IMG_NORMALS 7
#define GL_RGBA8_ 0x8058
typedef void (APIENTRY *PFN_dlBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLuint, GLenum);
typedef void (APIENTRY *PFN_dlGenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_dlBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_dlFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLboolean (APIENTRY *PFN_dlIsEnabled)(GLenum);
typedef void (APIENTRY *PFN_dlEnable)(GLenum);
static int g_dlssW, g_dlssH, g_dlssReset;
static int g_dlssNativeW, g_dlssNativeH;
static int g_dlssRR;              // the configured feature is Ray Reconstruction
static int g_dlssPreset;          // the configured model preset (NGX value, 0 = default)
static GLuint g_dlssReadFbo;
static struct {
    PFN_dlBlitFramebuffer Blit;
    PFN_dlGenFramebuffers GenFb;
    PFN_dlBindFramebuffer BindFb;
    PFN_dlFramebufferTexture2D FbTex;
    PFN_dlIsEnabled IsEnabled;
    PFN_dlEnable Enable, Disable;
} dl;

#ifndef _WIN32
static int g_renderAppliedScale = 100;
static int linux_physical_size(int *width, int *height);
#endif

static void *dlss_getproc(const char *name)
{
#ifndef _WIN32
    return (void *)linux_gl_proc(name);
#else
    void *p = (void *)real_wglGetProcAddress(name);
    if (!p || (uintptr_t)p <= 3 || p == (void *)-1) p = (void *)GetProcAddress(g_real, name);
    return p;
#endif
}

// Sizes of the configured feature (render = PostEffectFS viewport, output = the present's
// viewport) and the present size seen last frame.
static int g_dlssOW, g_dlssOH, g_presentW, g_presentH;

static int dlss_quality(int renderW, int outW)
{
    float r = (float)renderW / (float)outW;
    if (r > 0.99f) return 5;    // DLAA
    if (r > 0.60f) return 2;    // Quality (67%)
    if (r > 0.54f) return 1;    // Balanced (58%)
    if (r > 0.40f) return 0;    // Performance (50%)
    return 3;                   // Ultra Performance (33%)
}

static int dlss_ready(void)
{
    if (g_dlssState == 0) {
        g_dlssState = -1;
        wchar_t dir[MAX_PATH];
        pack_path(dir, "dlss");
        CreateDirectoryW(dir, NULL);
        if (!r_glBindImageTexture) r_glBindImageTexture = (PFN_glBindImageTexture)real_wglGetProcAddress("glBindImageTexture");
        if (!r_glMemoryBarrier) r_glMemoryBarrier = (PFN_glMemoryBarrier)real_wglGetProcAddress("glMemoryBarrier");
        dl.Blit = (PFN_dlBlitFramebuffer)dlss_getproc("glBlitFramebuffer");
        dl.GenFb = (PFN_dlGenFramebuffers)dlss_getproc("glGenFramebuffers");
        dl.BindFb = (PFN_dlBindFramebuffer)dlss_getproc("glBindFramebuffer");
        dl.FbTex = (PFN_dlFramebufferTexture2D)dlss_getproc("glFramebufferTexture2D");
        dl.IsEnabled = (PFN_dlIsEnabled)dlss_getproc("glIsEnabled");
        dl.Enable = (PFN_dlEnable)dlss_getproc("glEnable");
        dl.Disable = (PFN_dlEnable)dlss_getproc("glDisable");
        int available;
#if AU_HAS_FRAMEGEN
        AudVulkanContext context;
        available = afg_vulkan_context(&context) ? aud_init_shared(logf_, dlss_getproc, dir, dir, &context) : aud_init(logf_, dlss_getproc, dir, dir);
#else
        available = aud_init(logf_, dlss_getproc, dir, dir);
#endif
        if (r_glBindImageTexture && r_glMemoryBarrier && dl.Blit && dl.GenFb && dl.BindFb && dl.FbTex &&
            dl.IsEnabled && dl.Enable && dl.Disable && available)
            g_dlssState = 1;
#ifndef _WIN32
        if (g_dlssState < 0) InterlockedExchange(&g_reloadRequested, 1);
#endif
        logf_("dlss: %s", g_dlssState == 1 ? "ready" : "unavailable, the game upscales as usual");
    }
    return g_dlssState == 1;
}

static int render_physical_size(int *width, int *height)
{
#ifdef _WIN32
    HDC (WINAPI *currentDC)(void) = (HDC (WINAPI *)(void))GetProcAddress(g_real, "wglGetCurrentDC");
    HWND window = currentDC ? WindowFromDC(currentDC()) : NULL;
    RECT client;
    if (!window || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return 0;
    *width = client.right;
    *height = client.bottom;
    return 1;
#else
    return linux_physical_size(width, height);
#endif
}

#include "dlss_sharpen.h"
#include "dlss_resolve.h"

#if AU_HAS_FRAMEGEN
#include "aud_framegen.h"
static int g_framegenState;
static int g_framegenCaptured = -1;
static void framegen_shutdown(void)
{
    AudVulkanContext context;
    if (afg_vulkan_context(&context)) {
        afg_wait(); aud_shutdown();
        g_dlssState = 0;
        g_dlssW = g_dlssH = g_dlssOW = g_dlssOH = g_dlssNativeW = g_dlssNativeH = 0;
    }
    afg_shutdown();
}
static int framegen_prepare(int width, int height)
{
    if (!g_setFrameGen || g_temporalFrame != g_frame) return 0;
    HDC (WINAPI *currentDC)(void) = (HDC (WINAPI *)(void))GetProcAddress(g_real, "wglGetCurrentDC");
    HWND window = currentDC ? WindowFromDC(currentDC()) : NULL;
    if (g_framegenState == 0) {
        AudVulkanContext context;
        if (!afg_vulkan_context(&context)) {
            aud_shutdown(); g_dlssState = 0;
            g_dlssW = g_dlssH = g_dlssOW = g_dlssOH = g_dlssNativeW = g_dlssNativeH = 0;
        }
        wchar_t directory[MAX_PATH]; pack_path(directory, "streamline");
        g_framegenState = afg_init(logf_, dlss_getproc, directory, window) ? 1 : -1;
        if (g_framegenState < 0) { logf_("framegen: %s", afg_status()); framegen_shutdown(); }
    }
    int outputWidth, outputHeight;
    if (g_framegenState != 1 || !render_physical_size(&outputWidth, &outputHeight)) return 0;
    if (!afg_configure(width, height, outputWidth, outputHeight)) {
        logf_("framegen: %s; falling back to OpenGL", afg_status());
        framegen_shutdown(); g_framegenState = -1;
        return 0;
    }
    return 1;
}

static void framegen_inputs(GLuint depth, GLuint motion, int linearDepth)
{
    if (g_framegenState != 1 || !g_setFrameGen) return;
    r_glMemoryBarrier(0xffffffffu);
    if (afg_capture_inputs(depth, motion, linearDepth, g_temporalScene + 48)) g_framegenCaptured = g_frame;
}

static void framegen_before_ui(ProgRec *program)
{
    if (g_framegenCaptured != g_frame || !g_setFrameGen || !program || strcmp(program->pname, "Batcher2DFS")) return;
    GLint framebuffer = -1;
    r_glGetIntegerv(0x8ca6, &framebuffer);
    if (framebuffer == 0) afg_capture_hudless();
}

static void framegen_after_scene(ProgRec *program)
{
    if (g_framegenCaptured != g_frame || !g_setFrameGen || !program
        || (strcmp(program->pname, "PostEffectFS") && strcmp(program->pname, "TemporalAAFS") && strcmp(program->pname, "BasicFS"))) return;
    GLint framebuffer = -1;
    r_glGetIntegerv(0x8ca6, &framebuffer);
    if (framebuffer == 0) afg_capture_hudless();
}

static int framegen_swap(void)
{
    if (!g_setFrameGen || g_temporalFrame != g_frame || g_framegenCaptured != g_frame) {
        if (g_framegenState == 1 && g_framegenCaptured >= 0) { afg_stop(); g_framegenCaptured = -1; }
        return 0;
    }
    int presented = afg_present((unsigned)g_frame, g_temporalScene, g_temporalPreviousScene, g_sceneJitter, !g_temporalValid, g_setFrameGen);
    if (!presented && strstr(afg_status(), "failed")) {
        framegen_shutdown(); g_framegenState = -1; g_framegenCaptured = -1;
    }
    return presented;
}
#else
static void framegen_before_ui(ProgRec *program) { (void)program; }
static void framegen_after_scene(ProgRec *program) { (void)program; }
static int framegen_swap(void) { return 0; }
#endif

// PostEffectFS: writes the DLSS inputs at render size (image units 2-4).
static void dlss_draw(GLenum mode, GLint first, GLsizei count)
{
    if (g_dlssDiagnosticFrames == 1) dlss_diagnostic_errors("before capture");
#if AU_HAS_FRAMEGEN
    GLint inputViewport[4];
    r_glGetIntegerv(GL_VIEWPORT, inputViewport);
    int framegen = framegen_prepare(inputViewport[2], inputViewport[3]);
    if (!g_setDlssEnabled) {
        ProgRec *program = prog_rec(g_curProg, 0);
        if (!r_glBindImageTexture) r_glBindImageTexture = (PFN_glBindImageTexture)dlss_getproc("glBindImageTexture");
        if (!r_glMemoryBarrier) r_glMemoryBarrier = (PFN_glMemoryBarrier)dlss_getproc("glMemoryBarrier");
        if (framegen && r_glBindImageTexture && r_glMemoryBarrier) {
            r_glBindImageTexture(2, afg_texture(AFG_COLOR), 0, 0, 0, GL_WRITE_ONLY, GL_RGBA16F);
            r_glBindImageTexture(3, afg_texture(AFG_DEPTH), 0, 0, 0, GL_WRITE_ONLY, GL_R32F);
            r_glBindImageTexture(4, afg_texture(AFG_MOTION), 0, 0, 0, GL_WRITE_ONLY, GL_RG16F);
        } else framegen = 0;
        if (program && program->locDlssActive >= 0) r_glUniform1i(program->locDlssActive, framegen);
        if (program && program->locDlssRR >= 0) r_glUniform1i(program->locDlssRR, 0);
        if (framegen) dlss_set_viewport(g_curProg, inputViewport);
        r_glDrawArrays(mode, first, count);
        if (framegen) framegen_inputs(afg_texture(AFG_DEPTH), afg_texture(AFG_MOTION), 0);
        framegen_after_scene(program);
        return;
    }
#endif
    if (!g_setDlssEnabled || !dlss_ready()) {
        ProgRec *pp = prog_rec(g_curProg, 0);
        if (pp && pp->locDlssActive >= 0) r_glUniform1i(pp->locDlssActive, 0);
        r_glDrawArrays(mode, first, count);
        return;
    }
    GLint vp[4] = {0};
    r_glGetIntegerv(GL_VIEWPORT, vp);
    GLint drawFb = -1;
    r_glGetIntegerv(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &drawFb);
    // At Render Scale 100 PostEffectFS draws straight into the window: render and output size
    // are the same (DLAA) and DLSS runs right after this draw.
    int direct = drawFb == 0;
    if (direct) {
        g_presentW = vp[2];
        g_presentH = vp[3];
        render_physical_size(&g_presentW, &g_presentH);
    }
    int rr = g_setDlssMode == 2 && aud_rr_available();
    int renderWidth = vp[2], renderHeight = vp[3];
    if (renderWidth < 32) renderWidth = 32;
    if (renderHeight < 32) renderHeight = 32;
    // Model presets: sharpest = transformer K (Super Resolution) / E (Ray Reconstruction),
    // less ghosting = J / D.
    int pick = (int)(rr ? g_setDlssRRPreset : g_setDlssPreset);
    int preset = pick == 1 ? (rr ? 5 : 11) : pick == 2 ? (rr ? 4 : 10) : 0;
    if (g_presentW > 0 && (renderWidth != g_dlssW || renderHeight != g_dlssH || vp[2] != g_dlssNativeW || vp[3] != g_dlssNativeH || g_presentW != g_dlssOW || g_presentH != g_dlssOH ||
                           rr != g_dlssRR || preset != g_dlssPreset)) {
        int q = dlss_quality(renderWidth, g_presentW);
        g_dlssRR = rr;
        g_dlssPreset = preset;
#if AU_HAS_FRAMEGEN
        afg_wait();
#endif
        if (!aud_configure(renderWidth, renderHeight, g_presentW, g_presentH, q, 1, rr, preset)) {
            g_dlssState = -1;
#ifndef _WIN32
            InterlockedExchange(&g_reloadRequested, 1);
#endif
            r_glDrawArrays(mode, first, count);
            return;
        }
        g_dlssW = renderWidth;
        g_dlssH = renderHeight;
        g_dlssNativeW = vp[2];
        g_dlssNativeH = vp[3];
        g_dlssOW = g_presentW;
        g_dlssOH = g_presentH;
        g_dlssReset = 2;
        dlss_check_arm(10);
    }
    // Units 2-4 hold the path tracer's history between its passes: they must get the DLSS
    // textures or nothing (writes to an empty unit are dropped), never stay as they are.
    int active = g_dlssOW > 0 && vp[2] == g_dlssNativeW && vp[3] == g_dlssNativeH;
    r_glBindImageTexture(DLSS_IMG_COLOR, active ? aud_texture(AUD_TEX_COLOR) : 0, 0, 0, 0, GL_WRITE_ONLY, GL_RGBA16F);
    r_glBindImageTexture(DLSS_IMG_DEPTH, active ? aud_texture(AUD_TEX_DEPTH) : 0, 0, 0, 0, GL_WRITE_ONLY, GL_R32F);
    r_glBindImageTexture(DLSS_IMG_MOTION, active ? aud_texture(AUD_TEX_MOTION) : 0, 0, 0, 0, GL_WRITE_ONLY, GL_RG16F);
    if (g_dlssRR) {
        r_glBindImageTexture(DLSS_IMG_ALBEDO, active ? aud_texture(AUD_TEX_ALBEDO) : 0, 0, 0, 0, GL_WRITE_ONLY, GL_RGBA8_);
        r_glBindImageTexture(DLSS_IMG_SPECULAR, active ? aud_texture(AUD_TEX_SPECULAR) : 0, 0, 0, 0, GL_WRITE_ONLY, GL_RGBA8_);
        r_glBindImageTexture(DLSS_IMG_NORMALS, active ? aud_texture(AUD_TEX_NORMALS) : 0, 0, 0, 0, GL_WRITE_ONLY, GL_RGBA16F);
    }

    ProgRec *pp = prog_rec(g_curProg, 0);
    DLSSMaskState maskState = {0};
    if (active) {
        maskState = dlss_mask_begin();
        if (!maskState.valid) {
            if (pp && pp->locDlssActive >= 0) r_glUniform1i(pp->locDlssActive, 0);
            r_glDrawArrays(mode, first, count);
            return;
        }
    }
    if (pp && pp->locDlssActive >= 0) r_glUniform1i(pp->locDlssActive, active);
    if (pp && pp->locDlssRR >= 0) r_glUniform1i(pp->locDlssRR, active && g_dlssRR);
    if (active) {
        dlss_set_viewport(g_curProg, vp);
        GLint vertexArray = 0;
        r_glGetIntegerv(0x85b5, &vertexArray);
        g_dlssInputVAO = (GLuint)vertexArray;
        g_dlssInputMode = mode;
        g_dlssInputFirst = first;
        g_dlssInputCount = count;
        memcpy(g_dlssInputViewport, vp, sizeof g_dlssInputViewport);
    }
    r_glDrawArrays(mode, first, count);
    dlss_mask_end(maskState);
    if (g_dlssDiagnosticFrames == 1) dlss_diagnostic_errors("capture");
#if AU_HAS_FRAMEGEN
    if (framegen && active) framegen_inputs(aud_texture(AUD_TEX_DEPTH), aud_texture(AUD_TEX_MOTION), g_dlssRR);
#endif
    if (direct) {
        g_dlssPending = 1;
        if (active) dlss_present_rect(vp[0], vp[1], vp[2], vp[3], "PostEffectFS (straight into the window)", 1);
        g_dlssPending = 0;
    } else {
        g_dlssPending = 1;
    }
}

// Hytale puts the finished frame on the window either with a BasicFS draw (scaling it, Render
// Scale below 100) or a framebuffer blit; the first of those after PostEffectFS is replaced
// by DLSS's output. Returns 1 when it did.
static int dlss_present_rect(int x, int y, int w, int h, const char *how, int direct)
{
    if (render_physical_size(&w, &h)) x = y = 0;
    g_dlssPending = 0;
    g_postPresent = 1;
    g_presentW = w;
    g_presentH = h;
    static char loggedHow[64];
    if (strcmp(loggedHow, how)) {
        snprintf(loggedHow, sizeof(loggedHow), "%s", how);
        logf_("dlss: frame presented by %s (%dx%d)", how, w, h);
    }
    if (g_dlssState != 1 || g_dlssOW != w || g_dlssOH != h) return 0;
    GLint vp[4] = {x, y, w, h};
    dlss_check_before(vp, direct);
    r_glMemoryBarrier(0xFFFFFFFFu);
    g_jitterPx[0] = g_sceneJitter[0] * g_dlssW * 0.5f;
    g_jitterPx[1] = g_sceneJitter[1] * g_dlssH * 0.5f;
    if (!aud_evaluate(g_jitterPx[0] * (float)g_setJitterSX, g_jitterPx[1] * (float)g_setJitterSY,
                      (float)g_setMotionSX, (float)g_setMotionSY, g_dlssReset > 0 || g_setDlssDebug == 2)) {
#ifndef _WIN32
        g_dlssState = -1; // Restore native size on the next SDL resize sync.
        InterlockedExchange(&g_reloadRequested, 1);
#endif
        return 0;
    }
    if (g_dlssReset > 0) g_dlssReset--;
    g_dlssPresentedFrame = g_frame;

    GLint readFb = 0;
    r_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_, &readFb);
    if (!g_dlssReadFbo) dl.GenFb(1, &g_dlssReadFbo);
    dl.BindFb(GL_READ_FRAMEBUFFER_, g_dlssReadFbo);
    dl.FbTex(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, aud_texture(g_setDlssDebug == 1 ? AUD_TEX_COLOR : g_setDlssDebug == 3 ? AUD_TEX_DEPTH : g_setDlssDebug == 4 ? AUD_TEX_MOTION : AUD_TEX_OUTPUT), 0);
    GLboolean scissor = dl.IsEnabled(GL_SCISSOR_TEST);
    if (scissor) dl.Disable(GL_SCISSOR_TEST);
    if (g_setDlssDebug != 0 || !dlss_resolve(aud_texture(AUD_TEX_OUTPUT), vp)) {
        int inputView = g_setDlssDebug == 1 || g_setDlssDebug == 3 || g_setDlssDebug == 4;
        dl.Blit(0, 0, inputView ? g_dlssW : g_dlssOW, inputView ? g_dlssH : g_dlssOH,
            vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    if (scissor) dl.Enable(GL_SCISSOR_TEST);
    dl.BindFb(GL_READ_FRAMEBUFFER_, (GLuint)readFb);
    if (g_dlssDiagnosticFrames > 0 && --g_dlssDiagnosticFrames == 0) {
        dlss_diagnostic_errors("evaluate and resolve");
        dlss_diagnose(vp);
    }
    dlss_check_frame(vp);
#if AU_HAS_FRAMEGEN
    if (g_setFrameGen && g_framegenCaptured == g_frame) afg_capture_hudless();
#endif
    return 1;
}

// Below Render Scale 100 PostEffectFS draws offscreen and Hytale then scales the frame into
// the window with BasicFS (or TemporalAAFS draws it there): that draw is replaced.
static int dlss_present(void)
{
    ProgRec *p = g_curProg ? prog_rec(g_curProg, 0) : NULL;
    if (!p || (strcmp(p->pname, "BasicFS") && strcmp(p->pname, "TemporalAAFS"))) return 0;
    GLint fbo = -1;
    r_glGetIntegerv(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &fbo);
    if (fbo != 0) return 0;
    GLint vp[4] = {0};
    r_glGetIntegerv(GL_VIEWPORT, vp);
    return dlss_present_rect(vp[0], vp[1], vp[2], vp[3], p && p->pname[0] ? p->pname : "a draw", 0);
}

static int dlss_present_blit(GLint x0, GLint y0, GLint x1, GLint y1)
{
    GLint fbo = -1;
    r_glGetIntegerv(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &fbo);
    if (fbo != 0) return 0;
    return dlss_present_rect(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, abs(x1 - x0), abs(y1 - y0), "a framebuffer blit", 0);
}
#else
static void dlss_draw(GLenum mode, GLint first, GLsizei count) { r_glDrawArrays(mode, first, count); }
static void framegen_before_ui(ProgRec *program) { (void)program; }
static void framegen_after_scene(ProgRec *program) { (void)program; }
static int framegen_swap(void) { return 0; }
static int dlss_present(void) { g_dlssPending = 0; return 0; }
static int dlss_present_blit(GLint x0, GLint y0, GLint x1, GLint y1) { (void)x0; (void)y0; (void)x1; (void)y1; return 0; }
#endif

typedef void (APIENTRY *PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLuint, GLenum);
static PFN_glBlitFramebuffer r_glBlitFramebuffer;
static void APIENTRY hook_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLuint mask, GLenum filter)
{
    if (g_dlssPending && (mask & 0x4000u /*GL_COLOR_BUFFER_BIT*/) && dlss_present_blit(dx0, dy0, dx1, dy1)) return;
    r_glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
#if AU_HAS_FRAMEGEN
    if (g_setFrameGen && g_framegenCaptured == g_frame && (mask & 0x4000u)) {
        GLint framebuffer = -1;
        r_glGetIntegerv(0x8ca6, &framebuffer);
        if (framebuffer == 0) afg_capture_hudless();
    }
#endif
}

// Camera jitter for DLSS: a Halton(2,3) sub-pixel offset per frame, added to gl_Position by
// the pack's vertex shaders (uHookJitter, in NDC). g_jitterPx is what this frame's geometry
// used (passed to DLSS); the next frame's offset is chosen when PostEffectFS starts.

static float halton(int i, int base)
{
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= base) { f /= (float)base; r += f * (float)(i % base); }
    return r;
}

static void jitter_next_frame(void)
{
#if AU_HAS_DLSS
    if (g_setDlssJitter != 0 && g_dlssState == 1 && g_dlssProg && g_dlssW > 0 && g_frame - g_dlssPresentedFrame <= 2) {
            int phases = g_dlssW > 0 && g_dlssH > 0 ? (int)ceilf(8.0f * ((float)g_dlssOW / g_dlssW) * ((float)g_dlssOH / g_dlssH)) : 8;
            if (phases < 8) phases = 8;
            if (phases > 128) phases = 128;
            int i = 1 + g_frame % phases;
        g_jitterNextPx[0] = halton(i, 2) - 0.5f;
        g_jitterNextPx[1] = halton(i, 3) - 0.5f;
        g_jitterNdc[0] = 2.0f * g_jitterNextPx[0] / (float)g_dlssW;
        g_jitterNdc[1] = 2.0f * g_jitterNextPx[1] / (float)g_dlssH;
        return;
    }
#endif
    g_jitterNextPx[0] = g_jitterNextPx[1] = 0.0f;
    g_jitterNdc[0] = g_jitterNdc[1] = 0.0f;
}

#include "temporal.h"

// ----------------------------------------------------------------------------------------
// Uniform forwarding (shaderpack/forward.txt):  <SourceProgram> <srcUniform> <DestProgram|*> <dstUniform>
// The last value the game set on the source program is replayed into the destination
// program whenever it is used (e.g. SkyFS.uSunPosition -> DeferredFS.uHookSunPos).

typedef struct { char src[64], srcUni[64], dst[64], dstUni[64]; GLuint srcProg; } FwdRule;
static FwdRule g_fwd[16];
static int g_nFwd;

static void load_forward_cfg(void)
{
    g_nFwd = 0;
    char *txt = read_file("forward.txt", NULL);
    if (!txt) return;
    char *save = NULL;
    for (char *line = strtok_s(txt, "\n", &save); line; line = strtok_s(NULL, "\n", &save)) {
        if (line[0] == '#' || g_nFwd >= 16) continue;
        FwdRule r;
        memset(&r, 0, sizeof(r));
        if (sscanf(line, "%63s %63s %63s %63s", r.src, r.srcUni, r.dst, r.dstUni) == 4) g_fwd[g_nFwd++] = r;
    }
    free(txt);
}

static void replay_uniform_at(UniformRec *u, GLint loc)
{
    GLint saved = u->real;
    u->real = loc;
    replay_uniform(u);
    u->real = saved;
}

static LARGE_INTEGER g_t0, g_freq;
static int g_frame;
static int g_traceRemaining;

#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
typedef void (APIENTRY *PFN_glGetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint *);
static PFN_glGetFramebufferAttachmentParameteriv r_glGetFramebufferAttachmentParameteriv;

static void trace_use(GLuint prog, ProgRec *p)
{
    GLint fbo = 0;
    r_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    char fmt[128] = "";
    if (fbo && r_glGetFramebufferAttachmentParameteriv) {
        size_t o = 0;
        for (int a = 0; a < 3; a++) {
            GLint type = 0, bits[4] = {0};
            r_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + a, 0x8CD0 /*OBJECT_TYPE*/, &type);
            if (!type) break;
            for (int c = 0; c < 4; c++)
                r_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + a, 0x8212 + c, &bits[c]);
            o += snprintf(fmt + o, sizeof(fmt) - o, " c%d:%d/%d/%d/%d", a, bits[0], bits[1], bits[2], bits[3]);
        }
    }
    GLint vp[4] = {0};
    r_glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
    logf_("TRACE use %u %s fbo=%d vp=%dx%d%s", prog, p ? p->pname : "?", fbo, vp[2], vp[3], fmt);
}

static void check_extras(GLuint prog, ProgRec *p)
{
    p->extrasChecked = 1;
    p->hasExtras = 0;
    for (int i = 0; i < g_nCtex; i++) {
        p->ctexLoc[i] = r_glGetUniformLocation(prog, g_ctex[i].sampler);
        if (p->ctexLoc[i] >= 0) { r_glUniform1i(p->ctexLoc[i], (GLint)g_ctex[i].unit); p->hasExtras = 1; }
    }
    for (int i = 0; i < g_nFwd; i++) {
        p->fwdLoc[i] = -1;
        if (strcmp(g_fwd[i].dst, "*") && strcmp(g_fwd[i].dst, p->pname)) continue;
        p->fwdLoc[i] = r_glGetUniformLocation(prog, g_fwd[i].dstUni);
        if (p->fwdLoc[i] >= 0) p->hasExtras = 1;
    }
    p->locFrame = r_glGetUniformLocation(prog, "uHookFrame");
    p->locTime = r_glGetUniformLocation(prog, "uHookTime");
    p->locWetness = r_glGetUniformLocation(prog, "uHookWetness");
    if (p->locFrame >= 0 || p->locTime >= 0 || p->locWetness >= 0) p->hasExtras = 1;
    p->locJitter = r_glGetUniformLocation(prog, "uHookJitter");
    p->locUIFragmentScale = r_glGetUniformLocation(prog, "uHookUIFragmentScale");
    if (p->locUIFragmentScale >= 0) { r_glUniform2f(p->locUIFragmentScale, 1.0f, 1.0f); p->hasExtras = 1; }
    p->locPreviousVP = r_glGetUniformLocation(prog, "uHookPreviousViewProjection");
    p->locCurrentProjection = r_glGetUniformLocation(prog, "uHookCurrentProjection");
    p->locGICompact = r_glGetUniformLocation(prog, "uHookGICompact");
    p->locGISourceSize = r_glGetUniformLocation(prog, "uHookGISourceSize");
    p->locGIPreviousInverse = r_glGetUniformLocation(prog, "uHookGIPreviousInverseProjection");
    p->locGIPreviousDepth = r_glGetUniformLocation(prog, "uHookGIPreviousDepth");
    p->giCompact = p->locGICompact >= 0;
    if (p->giCompact) { r_glUniform1i(p->locGICompact, 1); p->hasExtras = 1; }
    p->locPreviousCamera = r_glGetUniformLocation(prog, "uHookPreviousCamera");
    p->locSceneJitter = r_glGetUniformLocation(prog, "uHookSceneJitter");
    p->locPreviousJitter = r_glGetUniformLocation(prog, "uHookPreviousJitter");
    p->locGeometryJitter = r_glGetUniformLocation(prog, "uHookGeometryJitter");
    p->locPreviousGeometryJitter = r_glGetUniformLocation(prog, "uHookPreviousGeometryJitter");
    p->locHistoryValid = r_glGetUniformLocation(prog, "uHookHistoryValid");
    if (p->locPreviousVP >= 0 || p->locSceneJitter >= 0 || p->locHistoryValid >= 0) p->hasExtras = 1;
    if (p->locJitter >= 0) p->hasExtras = 1;
    p->locVoxGeo = r_glGetUniformLocation(prog, "auVoxGeo");
    p->locVoxCol = r_glGetUniformLocation(prog, "auVoxCol");
    if (p->locVoxGeo >= 0) r_glUniform1i(p->locVoxGeo, VOX_UNIT_GEO);
    if (p->locVoxCol >= 0) r_glUniform1i(p->locVoxCol, VOX_UNIT_COL);
    GLint locLights = r_glGetUniformLocation(prog, "auLights");
    if (locLights >= 0) r_glUniform1i(locLights, VOX_UNIT_LIGHTS);
    if (p->locVoxGeo >= 0 || p->locVoxCol >= 0) p->hasExtras = 1;
    static const struct { const char *name; GLint unit; } giUni[9] = {
        {"auGIOut", GI_IMG_RAD}, {"auGIOutPos", GI_IMG_POS}, {"auGIOutLight", GI_IMG_LIGHT},
        {"auGIHist", GI_TEX_RAD}, {"auGIHistPos", GI_TEX_POS}, {"auGIHistLight", GI_TEX_LIGHT},
        {"auGIRaw", GI_TEX_RAW_RAD}, {"auGIRawPos", GI_TEX_RAW_POS}, {"auGIRawLight", GI_TEX_RAW_LIGHT}};
    p->usesGI = 0;
    p->locPTPass = r_glGetUniformLocation(prog, "uHookPTPass");
    p->locLocalLightTiles = r_glGetUniformLocation(prog, "uHookLocalLightTiles");
    p->giDenoise = r_glGetUniformLocation(prog, "auGIRaw") >= 0;
    p->giTemporal = r_glGetUniformLocation(prog, "auGIHist") >= 0;
    p->giLegacy = p->giTemporal && !p->giDenoise;
    p->giLights = r_glGetUniformLocation(prog, "auGIOutLight") >= 0;
    if (p->locPTPass >= 0) { p->usesGI = 1; p->hasExtras = 1; }
    for (int i = 0; i < 9; i++) {
        GLint loc = r_glGetUniformLocation(prog, giUni[i].name);
        if (loc >= 0) { r_glUniform1i(loc, giUni[i].unit); p->usesGI = 1; p->hasExtras = 1; }
    }
    if (!p->usesGI && g_giProg == prog) { g_giProg = 0; gi_release(); }
    if (!strcmp(p->pname, "DeferredFS"))
        logf_("GI pipeline: voxel=%d denoise=%d temporal=%d fullscreen draws=%d local-light cull=%d",
              p->locVoxGeo >= 0, p->giDenoise, p->giTemporal,
              1 + (p->locVoxCol >= 0), p->locLocalLightTiles >= 0);
    static const struct { const char *name; GLint unit; } dlssUni[7] = {
        {"auDlssColor", 2}, {"auDlssDepth", 3}, {"auDlssMotion", 4},
        {"auDlssAlbedo", 5}, {"auDlssSpecular", 6}, {"auDlssNormals", 7}, {"auDlssResponsive", 0}};
    p->usesDlss = 0;
    p->locDlssActive = r_glGetUniformLocation(prog, "uHookDlssActive");
    p->locDlssRR = r_glGetUniformLocation(prog, "uHookDlssRR");
    p->locMaterials = r_glGetUniformLocation(prog, "uHookMaterials");
    p->locSSSMasks = r_glGetUniformLocation(prog, "uHookSSSMasks");
    p->locSSSReady = r_glGetUniformLocation(prog, "uHookSSSReady");
    if (p->locSSSMasks >= 0) { r_glUniform1i(p->locSSSMasks, SSS_MASK_UNIT); p->hasExtras = 1; }
    p->locMaterialLOD = r_glGetUniformLocation(prog, "uHookTextureLodBias");
    if (p->locMaterialLOD >= 0) p->hasExtras = 1;
    p->locNativeEmission = r_glGetUniformLocation(prog, "uHookEmission");
    if (p->locNativeEmission >= 0) { r_glUniform1i(p->locNativeEmission, EMISSION_UNIT); p->hasExtras = 1; }
    if (p->locMaterials >= 0) { r_glUniform1i(p->locMaterials, MAT_UNIT); p->hasExtras = 1; }
    for (int i = 0; i < 7; i++) {
        GLint loc = r_glGetUniformLocation(prog, dlssUni[i].name);
        if (loc >= 0) { r_glUniform1i(loc, dlssUni[i].unit); p->usesDlss = 1; p->hasExtras = 1; }
    }
    if (!p->usesDlss && g_dlssProg == prog) g_dlssProg = 0;
}

// Minecraft-style "wetness": follows the rain signal with Optifine's default half-lives
// (30 s getting wet, 10 s drying). The rain signal is the sky's top gradient saturation the
// shaders use too (AU_Common.glsl GetRainFactor); the rainbows need wetness.
static float g_wetness;
static LARGE_INTEGER g_wetLast;

static float smooth_step(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

static void update_wetness(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = g_wetLast.QuadPart ? (double)(now.QuadPart - g_wetLast.QuadPart) / (double)g_freq.QuadPart : 0.0;
    g_wetLast = now;
    if (dt <= 0.0 || dt > 1.0) return;
    for (int i = 0; i < g_nFwd; i++) {
        if (strcmp(g_fwd[i].srcUni, "uTopGradientColor")) continue;
        ProgRec *src = prog_rec(g_fwd[i].srcProg, 0);
        if (!src) return;
        for (int k = 0; k < src->nuni; k++) {
            UniformRec *u = &src->uni[k];
            if (strcmp(u->name, "uTopGradientColor") || !u->data || u->bytes < 3 * sizeof(float)) continue;
            const float *c = (const float *)u->data;
            float mx = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
            float mn = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
            if (mx < 0.08f) return;   // night sky: no colour to read the weather from
            float rain = smooth_step(0.40f, 0.20f, (mx - mn) / mx);
            double halfLife = rain > g_wetness ? 30.0 : 10.0;
            g_wetness += (float)((rain - g_wetness) * (1.0 - pow(0.5, dt / halfLife)));
            return;
        }
        return;
    }
}

static void apply_extras(ProgRec *p)
{
    for (int i = 0; i < g_nCtex; i++) {
        if (p->ctexLoc[i] < 0 || !g_ctex[i].tex) continue;
        r_glActiveTexture(GL_TEXTURE0 + g_ctex[i].unit);
        r_glBindTexture(GL_TEXTURE_2D, g_ctex[i].tex);
        if (r_glBindSampler) r_glBindSampler(g_ctex[i].unit, 0);
        g_units[g_ctex[i].unit].target = GL_TEXTURE_2D;
        g_units[g_ctex[i].unit].tex = g_ctex[i].tex;
        g_units[g_ctex[i].unit].sampler = 0;
        r_glActiveTexture(GL_TEXTURE0 + g_activeUnit);
    }
    for (int i = 0; i < g_nFwd; i++) {
        if (p->fwdLoc[i] < 0) continue;
        ProgRec *src = prog_rec(g_fwd[i].srcProg, 0);
        if (!src) continue;
        for (int k = 0; k < src->nuni; k++) {
            if (!strcmp(src->uni[k].name, g_fwd[i].srcUni)) { replay_uniform_at(&src->uni[k], p->fwdLoc[i]); break; }
        }
    }
    if (p->locVoxGeo >= 0 || p->locVoxCol >= 0) vox_use(p);
    if (p->locMaterialLOD >= 0) {
        float bias = 0.0f;
#if AU_HAS_DLSS
        if (g_setDlssEnabled && g_dlssW > 0 && g_dlssOW > 0) bias = log2f((float)g_dlssW / (float)g_dlssOW);
#endif
        r_glUniform1f(p->locMaterialLOD, bias);
    }
    if (p->locMaterials >= 0 || p->locNativeEmission >= 0) materials_use(p);
    if (p->locJitter >= 0) {
        if (g_postPresent || !temporal_geometry(p)) r_glUniform2f(p->locJitter, 0.0f, 0.0f);
        else r_glUniform2f(p->locJitter, g_jitterNdc[0], g_jitterNdc[1]);
    }
    if (p->usesGI) g_giProg = g_curProg;
    if (p->usesDlss) g_dlssProg = g_curProg;
    if (p->locFrame >= 0) r_glUniform1i(p->locFrame, g_frame);
    if (p->locWetness >= 0) r_glUniform1f(p->locWetness, g_wetness);
    if (p->locTime >= 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        r_glUniform1f(p->locTime, (float)((double)(now.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart));
    }
}

// ----------------------------------------------------------------------------------------
// Profiling: FPS every 10 s in hook.log; programs/_profile logs one frame's GPU time per pass
// (GL timestamps taken at every program switch).

#define GL_TIMESTAMP 0x8E28
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
typedef void (APIENTRY *PFN_glGenQueries)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glQueryCounter)(GLuint, GLenum);
typedef void (APIENTRY *PFN_glGetQueryObjectui64v)(GLuint, GLenum, uint64_t *);
typedef void (APIENTRY *PFN_glGetQueryObjectiv)(GLuint, GLenum, GLint *);

#define PROF_MAX 1024
static int g_profState;      // 0 idle, 1 armed, 2 recording, 3 collecting
static int g_profN, g_profWait;
static GLuint g_profQ[PROF_MAX];
static char g_profName[PROF_MAX][48];
static LARGE_INTEGER g_fpsT0;
static int g_fpsFrames;

static void profile_frame_marker(void)
{
    // FPS
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_fpsT0.QuadPart) g_fpsT0 = now;
    g_fpsFrames++;
    double secs = (double)(now.QuadPart - g_fpsT0.QuadPart) / (double)g_freq.QuadPart;
    if (secs >= 10.0) {
        logf_("fps %.1f (%.2f ms)", g_fpsFrames / secs, secs * 1000.0 / g_fpsFrames);
        g_fpsFrames = 0;
        g_fpsT0 = now;
    }

    if (g_profState == 1) {
        static PFN_glGenQueries gen;
        if (!gen) gen = (PFN_glGenQueries)real_wglGetProcAddress("glGenQueries");
        if (!g_profQ[0] && gen) gen(PROF_MAX, g_profQ);
        g_profState = 2;
        g_profN = 0;
    } else if (g_profState == 2) {
        g_profState = 3;
        g_profWait = 0;
    }
}

static void profile_use(const char *name)
{
    static PFN_glQueryCounter qc;
    if (!qc) qc = (PFN_glQueryCounter)real_wglGetProcAddress("glQueryCounter");
    if (g_profState == 2 && g_profN < PROF_MAX && qc) {
        qc(g_profQ[g_profN], GL_TIMESTAMP);
        snprintf(g_profName[g_profN], 48, "%.47s", name && name[0] ? name : "(other)");
        g_profN++;
    }
}

static void profile_collect(void)
{
    if (g_profState != 3 || ++g_profWait < 4) return;
    static PFN_glGetQueryObjectui64v get;
    if (!get) get = (PFN_glGetQueryObjectui64v)real_wglGetProcAddress("glGetQueryObjectui64v");
    if (!get || g_profN < 2) { g_profState = 0; return; }
    static uint64_t t[PROF_MAX];
    for (int i = 0; i < g_profN; i++) get(g_profQ[i], GL_QUERY_RESULT, &t[i]);
    // Aggregate durations per program name.
    char names[128][48];
    double ms[128];
    int counts[128];
    int nn = 0;
    double total = 0.0;
    for (int i = 0; i + 1 < g_profN; i++) {
        double d = (double)(t[i + 1] - t[i]) / 1.0e6;
        total += d;
        int k = 0;
        while (k < nn && strcmp(names[k], g_profName[i])) k++;
        if (k == nn && nn < 128) { snprintf(names[nn], 48, "%.47s", g_profName[i]); ms[nn] = 0; counts[nn] = 0; nn++; }
        if (k < 128) { ms[k] += d; counts[k]++; }
    }
    logf_("PROFILE frame GPU %.2f ms over %d passes:", total, g_profN - 1);
    for (int pass = 0; pass < nn; pass++) {
        int best = -1;
        for (int k = 0; k < nn; k++) if (counts[k] >= 0 && (best < 0 || ms[k] > ms[best])) best = k;
        if (best < 0) break;
        logf_("PROFILE %8.3f ms  %3dx  %s", ms[best], counts[best], names[best]);
        counts[best] = -1;
    }
    g_profState = 0;
}

// Reads "#define NAME value" from include/AU_Settings.glsl (def when missing).
static double settings_num(const char *txt, const char *name, double def)
{
    if (!txt) return def;
    size_t n = strlen(name);
    for (const char *c = strstr(txt, "#define "); c; c = strstr(c + 1, "#define ")) {
        const char *q = c + 8;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, name, n) || (q[n] != ' ' && q[n] != '\t')) continue;
        return atof(q + n);
    }
    return def;
}

#include "menu_hotkey.h"

static void settings_load(void)
{
    char *txt = read_file("include\\AU_Settings.glsl", NULL);
    menu_hotkey_set(settings_num(txt, "AU_MENU_KEY", 36));
    g_setDlssJitter = settings_num(txt, "AU_DLSS_JITTER", 1);
    g_setJitterSX = settings_num(txt, "AU_DLSS_JITTER_SX", 1);
    g_setJitterSY = settings_num(txt, "AU_DLSS_JITTER_SY", 1);
    g_setMotionSX = settings_num(txt, "AU_DLSS_MOTION_SX", 1);
    g_setMotionSY = settings_num(txt, "AU_DLSS_MOTION_SY", 1);
    g_setDlssDebug = settings_num(txt, "AU_DLSS_DEBUG", 0);
    int voxelGI = settings_num(txt, "AU_PATH_TRACING", 0) > 0.5 && settings_num(txt, "AU_PRESET", 2) >= 2;
    int modeDenoising = settings_num(txt, voxelGI ? "AU_PT_DENOISE" : "AU_SSGI_DENOISE", 1) > 0.5;
    g_setDlssMode = settings_num(txt, "AU_GI_DENOISER", 1) == 2 && modeDenoising ? 2 : 1;
    g_setDlssPreset = settings_num(txt, "AU_DLSS_PRESET", 0);
    g_setDlssRRPreset = settings_num(txt, "AU_DLSS_RR_PRESET", g_setDlssPreset);
    g_setDlssSharpness = fmin(1.0, fmax(0.0, settings_num(txt, "AU_DLSS_SHARPNESS", 0.2)));
    g_setDlssEnabled = settings_num(txt, "AU_DLSS", 0) > 0.5;
    int previousFrameGen = g_setFrameGen;
    g_setFrameGen = (int)settings_num(txt, "AU_DLSS_FRAMEGEN", 0);
    if (g_setFrameGen < 0 || g_setFrameGen > 5) g_setFrameGen = 0;
#if AU_HAS_FRAMEGEN
    if (!previousFrameGen && g_setFrameGen && g_framegenState < 0) g_framegenState = 0;
#else
    (void)previousFrameGen;
#endif
    g_setRenderScale = (int)settings_num(txt, "AU_RENDER_SCALE", 0);
    if (g_setRenderScale < 0 || g_setRenderScale > 100) g_setRenderScale = 0;
    g_setGIHalf = 0;
    g_setVoxelColors = settings_num(txt, "AU_PATH_TRACING", 0) > 0.5 && settings_num(txt, "AU_PRESET", 2) >= 2;
    if (!g_setVoxelColors && g_voxTex[1]) {
        PFN_glDeleteTextures deleteTextures = (PFN_glDeleteTextures)GetProcAddress(g_real, "glDeleteTextures");
        if (deleteTextures) {
            if (r_glBindImageTexture) r_glBindImageTexture(VOX_UNIT_COL, 0, 0, 1, 0, GL_READ_WRITE, GL_RGBA8);
            deleteTextures(1, &g_voxTex[1]);
            g_voxTex[1] = 0;
        }
    }
    if (!g_setVoxelColors && settings_num(txt, "AU_LOCAL_LIGHTS", 1) < 0.5 && g_voxState == 1) {
        PFN_glDeleteTextures deleteTextures = (PFN_glDeleteTextures)GetProcAddress(g_real, "glDeleteTextures");
        if (deleteTextures) {
            if (r_glBindImageTexture) {
                r_glBindImageTexture(VOX_UNIT_GEO, 0, 0, 1, 0, GL_READ_WRITE, GL_R32UI);
                r_glBindImageTexture(VOX_UNIT_LIGHTS, 0, 0, 0, 0, GL_READ_WRITE, GL_R32UI);
            }
            deleteTextures(2, g_voxTex);
            deleteTextures(1, &g_voxLights);
            g_voxTex[0] = g_voxTex[1] = g_voxLights = 0;
            g_voxState = 0;
            g_voxLastUser[0] = 0;
        }
    }
    free(txt);
}

static void materials_reset(void);

static void do_reload(void)
{
    g_temporalHavePrevious = 0;
    g_temporalValid = 0;
    settings_load();
    materials_reset();
    EnterCriticalSection(&g_lock);
    g_enabled = !g_toggleOff && !pack_flag("programs\\_disabled");
    load_index();
    load_shares();
    load_textures_cfg();
    load_forward_cfg();
    if (pack_flag("programs\\_profile")) {
        wchar_t path[MAX_PATH];
        pack_path(path, "programs\\_profile");
        DeleteFileW(path);
        if (g_profState == 0) g_profState = 1;
    }
    if (pack_flag("programs\\_trace")) {
        wchar_t path[MAX_PATH];
        pack_path(path, "programs\\_trace");
        DeleteFileW(path);
        g_traceRemaining = 800;
    }
#if AU_HAS_DLSS
    if (pack_flag("programs\\_dlss_diagnose")) {
        wchar_t path[MAX_PATH];
        pack_path(path, "programs\\_dlss_diagnose");
        DeleteFileW(path);
        g_dlssDiagnosticFrames = 3;
        logf_("dlss diagnostic: armed for the third DLSS frame");
    }
    dlss_check_arm(30);
#endif
    vox_clear();
    gi_release();
    int relinked = 0, failed = 0;
    for (size_t id = 1; id < g_progCap; id++) {
        ProgRec *p = &g_progs[id];
        if (!p->used) continue;
        p->extrasChecked = 0;
        if (!p->linked || !p->nsh) continue;
        int r = relink_program((GLuint)id, p, 1);
        if (r == -1) {
            relink_program((GLuint)id, p, 0);
            failed++;
        } else if (r == 0) {
            failed++;
        } else if (r == 2) {
            relinked++;
        }
    }
    r_glUseProgram(g_curProg);
    logf_("reload done: relinked=%d failed=%d", relinked, failed);
    LeaveCriticalSection(&g_lock);
}

static void APIENTRY hook_glUseProgram(GLuint prog)
{
    if (g_reloadRequested && InterlockedExchange(&g_reloadRequested, 0)) {
        g_curProg = prog;
        do_reload();
    }
    EnterCriticalSection(&g_lock);
    g_curProg = prog;
    g_matProgRec = NULL;
    r_glUseProgram(prog);
    ProgRec *p = prog ? prog_rec(prog, 0) : NULL;
    if (g_profState == 2) profile_use(p ? p->pname : "");
    if (p && p->linked) {
        if (!p->blkChecked && g_nKnownBlocks) apply_known_blocks(prog, p);
        if (g_nShares) apply_shares(prog, p);
        for (int i = 0; i < g_nFwd; i++) if (!strcmp(g_fwd[i].src, p->pname)) g_fwd[i].srcProg = prog;
        if (!strcmp(p->pname, "PostEffectFS")) { update_wetness(); profile_frame_marker(); profile_collect(); }
        if (g_nCtex) ensure_textures();
        if (!p->extrasChecked) check_extras(prog, p);
        if (p->hasExtras) apply_extras(p);
    }
    if (g_traceRemaining > 0) { g_traceRemaining--; trace_use(prog, p); }
    LeaveCriticalSection(&g_lock);
}

#ifdef _WIN32
static int is_game_foreground(void)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI watcher_thread(LPVOID arg)
{
    (void)arg;
    wchar_t dirs[2][MAX_PATH];
    pack_path(dirs[0], "programs");
    pack_path(dirs[1], "include");
    HANDLE h[2];
    for (int i = 0; i < 2; i++) {
        CreateDirectoryW(dirs[i], NULL);
        h[i] = FindFirstChangeNotificationW(dirs[i], FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    }
    int pauseDown = 0;
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, h, FALSE, 50);
        if (r == WAIT_TIMEOUT) {
            // Pause key toggles the shaderpack on/off (only while the game has focus).
            int down = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
            if (down && !pauseDown && is_game_foreground() && !g_menuBinding) {
                g_toggleOff = !g_toggleOff;
                logf_("shaderpack %s (Pause key)", g_toggleOff ? "OFF" : "ON");
                InterlockedExchange(&g_reloadRequested, 1);
            }
            pauseDown = down;
            continue;
        }
        if (r >= WAIT_OBJECT_0 + 2) { Sleep(1000); continue; }
        // Debounce bursts of writes from editors.
        Sleep(400);
        for (int i = 0; i < 2; i++) {
            FindNextChangeNotification(h[i]);
            while (WaitForSingleObject(h[i], 0) == WAIT_OBJECT_0) FindNextChangeNotification(h[i]);
        }
        InterlockedExchange(&g_reloadRequested, 1);
    }
    return 0;
}

// ----------------------------------------------------------------------------------------
#endif // _WIN32 watcher

// In-game options menu (Home key). Lists the #defines named in shaderpack/options.txt and
// edits their values in include/AU_Settings.glsl, which the file watcher then hot-reloads.
// Drawn as an overlay at SwapBuffers, after the game's own UI.

#define MENU_MAX 256
#define MENU_TAB_MAX 24
#define MENU_SETTINGS "include\\AU_Settings.glsl"
enum { MO_HEADER, MO_BOOL, MO_ENUM, MO_RANGE, MO_PACK, MO_INFO, MO_ACTION, MO_INTERNAL, MO_SECTION, MO_PROFILE, MO_KEY };
#define MENU_SELECTABLE(m) ((m)->kind != MO_HEADER && (m)->kind != MO_INFO && (m)->kind != MO_INTERNAL && (m)->kind != MO_SECTION)
typedef struct {
    int kind;
    int tab;
    char define[48];
    wchar_t label[64];
    wchar_t help[192];
    double min, max, step, def, value;
    int nEnum;
    double enumVal[24];
    wchar_t enumLabel[24][64];
    int decimals, isFloat, present;
} MenuOpt;

typedef struct {
    wchar_t label[64];
    int selected, top, count;
} MenuTab;

typedef struct {
    int x, y, width, height, pad, rail, title, footer, row, rows, contentTop, controlX;
    int footerButtonTop, footerButtonBottom;
} MenuLayout;

static MenuOpt g_mo[MENU_MAX];
static int g_nMo, g_moSel = -1, g_moTop;
static MenuTab g_menuTabs[MENU_TAB_MAX];
static int g_menuTabCount, g_menuTab, g_menuVisible[MENU_MAX], g_menuVisibleCount;
static wchar_t g_menuSearch[64];
static int g_menuSearchFocus, g_menuDragging = -1, g_menuRawMouse;
static double g_menuLastClick;
static double g_menuResetArmedAt, g_menuResetUntil;
static float g_menuPointerX, g_menuPointerY;
static MenuLayout g_menuLayout;
static volatile LONG g_menuOpen;
static volatile LONG g_menuTAAWarningPending;
static int g_menuMouseMode;
static int g_menuPresetNaming, g_menuProfileChoice = -1;
static void menu_profiles_load(MenuOpt *option);
static void menu_profile_action(const char *name);
static void menu_profile_finish(void);
static void menu_profile_action_display(const char *name, wchar_t *out, int cap);
static int menu_profile_delete_armed(void);
static void menu_profile_disarm_delete(void);
static int g_menuSDLReady;
static volatile LONG g_menuKeyEdges;
#ifdef _WIN32
static const int g_menuKeys[] = {VK_HOME, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_PRIOR, VK_NEXT, VK_ESCAPE};
#endif
static int g_menuDirty = 1;
static double g_menuWriteAt;
static wchar_t g_menuStatus[96];
static int g_menuBindReady;
static volatile LONG g_menuBindEvent;
static unsigned char g_menuBindDown[256];
static CRITICAL_SECTION g_menuLock;
#ifdef _WIN32
static HWND g_gameHwnd;
static WNDPROC g_prevWndProc;
#endif

static int menu_contains(const wchar_t *text, const wchar_t *query)
{
    if (!*query) return 1;
    for (int start = 0; text[start]; start++) {
        int offset = 0;
        while (query[offset] && text[start + offset]
               && towlower(text[start + offset]) == towlower(query[offset])) offset++;
        if (!query[offset]) return 1;
    }
    return 0;
}

static int menu_visible_index(int option)
{
    for (int index = 0; index < g_menuVisibleCount; index++)
        if (g_menuVisible[index] == option) return index;
    return -1;
}

static void menu_rebuild_rows(void)
{
    g_menuVisibleCount = 0;
    for (int index = 0; index < g_nMo; index++) {
        MenuOpt *option = &g_mo[index];
        if (option->kind == MO_HEADER || option->kind == MO_INTERNAL) continue;
        if (*g_menuSearch && !g_menuPresetNaming) {
            wchar_t identifier[64];
            if (option->kind == MO_PROFILE && option->value >= 0 && option->value < 12)
                _snwprintf(identifier, 64, L"%ls %ls", option->value < 6 ? L"RTX" : L"Non-RTX", option->label);
            else MultiByteToWideChar(CP_UTF8, 0, option->define, -1, identifier, 64);
            identifier[63] = 0;
            if (!menu_contains(option->label, g_menuSearch) && !menu_contains(identifier, g_menuSearch)) continue;
        } else if (option->tab != g_menuTab) continue;
        g_menuVisible[g_menuVisibleCount++] = index;
    }
    if (menu_visible_index(g_moSel) < 0) {
        g_moSel = -1;
        for (int index = 0; index < g_menuVisibleCount; index++)
            if (MENU_SELECTABLE(&g_mo[g_menuVisible[index]])) { g_moSel = g_menuVisible[index]; break; }
    }
    g_menuDirty = 1;
}

static void menu_activate_tab(int tab)
{
    if (tab < 0 || tab >= g_menuTabCount) return;
    if (g_menuTab >= 0 && g_menuTab < g_menuTabCount) {
        g_menuTabs[g_menuTab].selected = g_moSel;
        g_menuTabs[g_menuTab].top = g_moTop;
    }
    g_menuTab = tab;
    g_menuSearch[0] = 0;
    g_menuPresetNaming = 0;
    g_menuSearchFocus = 0;
    g_menuDragging = -1;
    g_moSel = g_menuTabs[tab].selected;
    g_moTop = g_menuTabs[tab].top;
    menu_rebuild_rows();
}

static double now_sec(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_freq.QuadPart;
}

static char *menu_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void to_wide(wchar_t *out, int cap, const char *s)
{
#ifdef _WIN32
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = 0;
#else
    au_to_wide(out, (size_t)cap, s);
#endif
    out[cap - 1] = 0;
}

static int decimals_of(const char *num)
{
    const char *d = strchr(num, '.');
    if (!d) return 0;
    int n = 0;
    d++;
    while (d[n] >= '0' && d[n] <= '9') n++;
    return n;
}

// Value token of the first "#define NAME value" line, or NULL. *tokEnd = end of the token.
static char *find_define_value(char *txt, const char *name, char **tokEnd)
{
    size_t nl = strlen(name);
    for (char *p = txt; (p = strstr(p, "#define ")) != NULL; p += 8) {
        if (p != txt && p[-1] != '\n') continue;
        char *q = p + 8;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, name, nl) || (q[nl] != ' ' && q[nl] != '\t')) continue;
        q += nl;
        while (*q == ' ' || *q == '\t') q++;
        char *e = q;
        while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
        if (tokEnd) *tokEnd = e;
        return q;
    }
    return NULL;
}

// options.txt:  "@ Section"  or  "DEFINE | Label | kind | default"
//   kind = bool | enum value:Label ... (underscores show as spaces) | range min max step
//   DEFINE "!PACK" is the shaderpack on/off switch (same as the Pause key).
static void menu_load(void)
{
    g_menuBinding = 0;
    menu_profile_disarm_delete();
    char *opt = read_file("options.txt", NULL);
    char *set = read_file(MENU_SETTINGS, NULL);
    g_nMo = 0;
    g_menuTabCount = 0;
    memset(g_menuTabs, 0, sizeof(g_menuTabs));
    char *save = NULL;
    for (char *line = opt ? strtok_s(opt, "\n", &save) : NULL; line && g_nMo < MENU_MAX; line = strtok_s(NULL, "\n", &save)) {
        line = menu_trim(line);
        if (!*line || *line == '#') continue;
        MenuOpt *m = &g_mo[g_nMo];
        memset(m, 0, sizeof(*m));
        if (*line == '@') {
            if (g_menuTabCount >= MENU_TAB_MAX) continue;
            m->kind = MO_HEADER;
            to_wide(m->label, 64, menu_trim(line + 1));
            MenuTab *tab = &g_menuTabs[g_menuTabCount++];
            memcpy(tab->label, m->label, sizeof(tab->label));
            tab->selected = -1;
            g_nMo++;
            continue;
        }
        char *f[5] = {0};
        int nf = 0;
        char *s2 = NULL;
        for (char *t = strtok_s(line, "|", &s2); t && nf < 5; t = strtok_s(NULL, "|", &s2)) f[nf++] = menu_trim(t);
        if (nf < 3) continue;
        m->tab = g_menuTabCount > 0 ? g_menuTabCount - 1 : 0;
        if (g_menuTabCount > 0) g_menuTabs[m->tab].count++;
        if (nf >= 5) to_wide(m->help, 192, f[4]);
        snprintf(m->define, sizeof(m->define), "%s", f[0]);
#ifndef _WIN32
        if (!strcmp(m->define, "AU_DLSS_FRAMEGEN") || !strcmp(m->define, "!DLSS_FG_INFO")) continue;
#if !AU_HAS_DLSS
        if (!strncmp(m->define, "AU_DLSS", 7) || !strcmp(m->define, "!DLSS_INFO") || !strcmp(m->define, "AU_RENDER_SCALE")) continue;
#endif
#endif
        to_wide(m->label, 64, f[1]);
        char *kind = f[2];
        if (!strcmp(m->define, "!PROFILE")) {
            menu_profiles_load(m);
            continue;
        }
        if (!strcmp(kind, "action")) {
            m->kind = MO_ACTION;
            m->present = 1;
            g_nMo++;
            continue;
        }
        if (!strcmp(kind, "info")) {
            m->kind = MO_INFO;
            m->present = 1;
            g_nMo++;
            continue;
        }
        if (!strcmp(m->define, "!PACK")) {
            m->kind = MO_PACK;
            m->present = 1;
            m->value = g_toggleOff ? 0.0 : 1.0;
            m->def = 1.0;
            g_nMo++;
            continue;
        }
        if (!strcmp(kind, "key")) {
            m->kind = MO_KEY;
        } else if (!strcmp(kind, "internal")) {
            m->kind = MO_INTERNAL;
        } else if (!strncmp(kind, "bool", 4)) {
            m->kind = MO_BOOL;
            m->max = 1.0;
            m->step = 1.0;
        } else if (!strncmp(kind, "enum", 4)) {
            m->kind = MO_ENUM;
            char *s3 = NULL;
            for (char *t = strtok_s(kind + 4, " ", &s3); t && m->nEnum < 24; t = strtok_s(NULL, " ", &s3)) {
                char *c = strchr(t, ':');
                if (!c) continue;
                *c = 0;
                m->enumVal[m->nEnum] = atof(t);
                to_wide(m->enumLabel[m->nEnum], 64, c + 1);
                for (wchar_t *w = m->enumLabel[m->nEnum]; *w; w++) if (*w == L'_') *w = L' ';
                m->nEnum++;
            }
            if (!m->nEnum) continue;
        } else if (!strncmp(kind, "range", 5)) {
            char a[32], b[32], c[32];
            if (sscanf(kind + 5, "%31s %31s %31s", a, b, c) != 3) continue;
            m->kind = MO_RANGE;
            m->min = atof(a);
            m->max = atof(b);
            m->step = atof(c);
            if (m->step <= 0.0) m->step = 1.0;
            m->decimals = decimals_of(c);
            m->isFloat = strchr(a, '.') || strchr(b, '.') || strchr(c, '.');
        } else {
            continue;
        }
        if (nf >= 4 && *f[3]) {
            m->def = atof(f[3]);
            if (strchr(f[3], '.')) m->isFloat = 1;
            if (decimals_of(f[3]) > m->decimals) m->decimals = decimals_of(f[3]);
        }
        char *e = NULL;
        char *v = set ? find_define_value(set, m->define, &e) : NULL;
        if (v) {
            char tok[32];
            size_t n = (size_t)(e - v);
            if (n > 31) n = 31;
            memcpy(tok, v, n);
            tok[n] = 0;
            m->value = atof(tok);
            m->present = 1;
            if (strchr(tok, '.')) m->isFloat = 1;
            if (decimals_of(tok) > m->decimals) m->decimals = decimals_of(tok);
        }
        if (!strcmp(m->define, "AU_MENU_KEY")) {
            menu_hotkey_set(m->present ? m->value : 36);
            m->value = g_menuHotkey;
            m->present = 1;
        }
        if (nf < 4 || !*f[3]) m->def = m->value;
        g_nMo++;
    }
    free(opt);
    free(set);
    if (g_menuTab >= g_menuTabCount) g_menuTab = 0;
    g_moTop = 0;
    menu_rebuild_rows();
}

static void format_value(const MenuOpt *m, double v, char *out, size_t cap)
{
    if (m->kind == MO_RANGE && m->isFloat) snprintf(out, cap, "%.*f", m->decimals < 1 ? 1 : m->decimals, v);
    else snprintf(out, cap, "%d", (int)floor(v + 0.5));
}

static void display_value(const MenuOpt *m, wchar_t *out, int cap)
{
    if (m->kind == MO_KEY) {
        if (g_menuBinding && g_moSel >= 0 && m == &g_mo[g_moSel]) _snwprintf(out, cap, L"Press a key...");
        else menu_hotkey_name((int)m->value, out, cap);
        out[cap - 1] = 0;
        return;
    }
    if (m->kind == MO_ACTION) {
        if (!strcmp(m->define, "!PROFILE_APPLY")) {
            _snwprintf(out, cap, L"Apply profile");
            out[cap - 1] = 0;
            return;
        }
        if (!strcmp(m->define, "!PROFILE_SAVE")) {
            _snwprintf(out, cap, L"%ls", g_menuPresetNaming ? L"Save name" : L"Save preset...");
            out[cap - 1] = 0;
            return;
        }
        if (!strcmp(m->define, "!PROFILE_EXPORT") || !strcmp(m->define, "!PROFILE_DELETE")) {
            menu_profile_action_display(m->define, out, cap);
            return;
        }
        _snwprintf(out, cap, L"%ls", now_sec() < g_menuResetUntil ? L"Confirm reset" : L"Reset all");
        out[cap - 1] = 0;
        return;
    }
    if (m->kind == MO_INFO) {
        if (!strcmp(m->define, "!TAA_INFO")) {
            _snwprintf(out, cap, L"Disable Hytale TAA");
            out[cap - 1] = 0;
            return;
        }
        if (!strcmp(m->define, "!DLSS_DENOISER_INFO")) {
            _snwprintf(out, cap, L"%ls", g_setDlssMode == 2 ? L"NVIDIA RR requested" : L"Original");
            out[cap - 1] = 0;
            return;
        }
        if (!strcmp(m->define, "!DLSS_FG_INFO")) {
#if AU_HAS_FRAMEGEN
            to_wide(out, cap, g_setFrameGen ? afg_status() : "Off");
#else
            _snwprintf(out, cap, L"Backend not built"); out[cap - 1] = 0;
#endif
            return;
        }
#if AU_HAS_DLSS
        static const wchar_t *modes[] = {L"Performance", L"Balanced", L"Quality", L"Ultra Perf", L"Ultra Quality", L"DLAA"};
        int q = g_dlssOW > 0 ? dlss_quality(g_dlssW, g_dlssOW) : -1;
        if (g_dlssState == 1 && g_dlssOW > 0 && g_frame - g_dlssPresentedFrame <= 30)
            _snwprintf(out, cap, L"%dx%d  %d%%  %ls", g_dlssW, g_dlssH, (int)(100.0 * g_dlssW / g_dlssOW + 0.5),
                       q >= 0 && q <= 5 ? modes[q] : L"");
        else
            _snwprintf(out, cap, L"%ls", g_dlssState == -1 ? L"unavailable" : L"not running");
#else
        _snwprintf(out, cap, L"n/a");
#endif
        out[cap - 1] = 0;
        return;
    }
    if (!m->present) { _snwprintf(out, cap, L"n/a"); out[cap - 1] = 0; return; }
    if ((!strcmp(m->define, "AU_DLSS_PRESET") || !strcmp(m->define, "AU_DLSS_RR_PRESET")) && m->value > 0.5) {
        int reconstruction = !strcmp(m->define, "AU_DLSS_RR_PRESET");
        _snwprintf(out, cap, L"%ls (%ls)", reconstruction ? (m->value < 1.5 ? L"E" : L"D") : (m->value < 1.5 ? L"K" : L"J"),
                   m->value < 1.5 ? L"sharpest" : L"less ghosting");
        out[cap - 1] = 0;
        return;
    }
    if (m->kind == MO_BOOL || m->kind == MO_PACK) { _snwprintf(out, cap, L"%ls", m->value > 0.5 ? L"On" : L"Off"); out[cap - 1] = 0; return; }
    if (m->kind == MO_ENUM)
        for (int i = 0; i < m->nEnum; i++)
            if (fabs(m->enumVal[i] - m->value) < 1e-6) { _snwprintf(out, cap, L"%ls", m->enumLabel[i]); out[cap - 1] = 0; return; }
    char buf[32];
    format_value(m, m->value, buf, sizeof(buf));
    to_wide(out, cap, buf);
}

// Writes the menu's values into the settings file, keeping everything else as is.
static void menu_save(void)
{
    size_t n = 0;
    char *set = read_file(MENU_SETTINGS, &n);
    if (!set) return;
    int changed = 0;
    for (int i = 0; i < g_nMo; i++) {
        MenuOpt *m = &g_mo[i];
        if (m->kind == MO_HEADER || m->kind == MO_PACK || m->kind == MO_INFO || m->kind == MO_ACTION
            || m->kind == MO_SECTION || m->kind == MO_PROFILE || !m->present) continue;
        char *e = NULL;
        char *v = find_define_value(set, m->define, &e);
        if (!v) {
            if (!strcmp(m->define, "AU_MENU_KEY")) {
                char line[64];
                int length = snprintf(line, sizeof line, "\n#define AU_MENU_KEY %d\n", (int)m->value);
                char *updated = realloc(set, n + length + 1);
                if (!updated) { free(set); return; }
                set = updated;
                memcpy(set + n, line, length + 1);
                n += length;
                changed++;
            }
            continue;
        }
        // Same number already there (maybe written differently, "3.0" vs "3.00"): leave it.
        char tok[32];
        size_t tl = (size_t)(e - v) < 31 ? (size_t)(e - v) : 31;
        memcpy(tok, v, tl);
        tok[tl] = 0;
        if (fabs(atof(tok) - m->value) < 1e-9) continue;
        char buf[32];
        format_value(m, m->value, buf, sizeof(buf));
        size_t pre = (size_t)(v - set), oldLen = (size_t)(e - v), newLen = strlen(buf);
        char *ns = (char *)malloc(n - oldLen + newLen + 1);
        memcpy(ns, set, pre);
        memcpy(ns + pre, buf, newLen);
        memcpy(ns + pre + newLen, e, n - pre - oldLen + 1);
        n = n - oldLen + newLen;
        free(set);
        set = ns;
        changed++;
    }
    if (changed) {
        write_file(MENU_SETTINGS, set, n);
        logf_("menu: wrote %d setting(s)", changed);
    }
    menu_hotkey_set(settings_num(set, "AU_MENU_KEY", 36));
    free(set);
}

static void menu_bind_key_event(int key)
{
    int scan = menu_hotkey_scan(key);
    if (g_menuBinding && (g_menuBindReady || key == 0x1b) && scan > 0 && scan < 224)
        InterlockedExchange(&g_menuBindEvent, key);
}

static int menu_bind_poll(const unsigned char *pressed, int focused)
{
    if (!g_menuBinding) return 0;
    int eventKey = (int)InterlockedExchange(&g_menuBindEvent, 0);
    if (!focused || !g_menuOpen || pressed[0x1b] || eventKey == 0x1b) {
        g_menuBinding = 0;
        _snwprintf(g_menuStatus, 96, L"Key binding cancelled");
        g_menuDirty = 1;
        return 1;
    }
    if (!g_menuBindReady) {
        for (int key = 8; key < 256; key++) if (pressed[key]) return 1;
        memset(g_menuBindDown, 0, sizeof g_menuBindDown);
        g_menuBindReady = 1;
        _snwprintf(g_menuStatus, 96, L"Press a key; Escape cancels");
        g_menuDirty = 1;
        return 1;
    }
    for (int key = 8; key < 256; key++) {
        int edge = (pressed[key] && !g_menuBindDown[key]) || eventKey == key;
        g_menuBindDown[key] = pressed[key];
        if (!edge) continue;
        if (menu_hotkey_valid(key)) {
            for (int index = 0; index < g_nMo; index++)
                if (g_mo[index].kind == MO_KEY) g_mo[index].value = key;
            g_menuBinding = 0;
            g_menuWriteAt = 0;
            menu_save();
            wchar_t name[32]; menu_hotkey_name(key, name, 32);
            _snwprintf(g_menuStatus, 96, L"Menu hotkey saved: %ls", name);
        } else {
            if (key == 0x13 || (key >= 0x21 && key <= 0x22) || (key >= 0x25 && key <= 0x28))
                _snwprintf(g_menuStatus, 96, L"That key is reserved. Choose another; Escape cancels");
            g_menuDirty = 1;
            continue;
        }
        g_menuDirty = 1;
        break;
    }
    return 1;
}

static void menu_reset_all(void)
{
    double time = now_sec();
    if (time >= g_menuResetUntil) {
        g_menuResetArmedAt = time;
        g_menuResetUntil = time + 5.0;
        _snwprintf(g_menuStatus, 96, L"Click again to reset all categories");
        g_menuDirty = 1;
        return;
    }
    if (time - g_menuResetArmedAt < 0.5) return;
    for (int index = 0; index < g_nMo; index++) {
        MenuOpt *option = &g_mo[index];
        if (!option->present || option->kind == MO_HEADER || option->kind == MO_INFO || option->kind == MO_ACTION
            || option->kind == MO_SECTION || option->kind == MO_PROFILE) continue;
        option->value = option->def;
        if (option->kind == MO_PACK) g_toggleOff = option->def < 0.5;
    }
    g_menuResetUntil = 0.0;
    g_menuWriteAt = 0.0;
    g_menuDragging = -1;
    g_menuProfileChoice = -1;
    menu_save();
    InterlockedExchange(&g_reloadRequested, 1);
    _snwprintf(g_menuStatus, 96, L"All categories restored to defaults");
    g_menuDirty = 1;
}

#include "profiles.h"

static void menu_move(int dir)
{
    int selected = menu_visible_index(g_moSel);
    for (int index = selected + dir; index >= 0 && index < g_menuVisibleCount; index += dir)
        if (MENU_SELECTABLE(&g_mo[g_menuVisible[index]])) {
            g_moSel = g_menuVisible[index];
            g_menuDirty = 1;
            return;
        }
}

static void menu_section(int dir)
{
    if (g_menuTabCount > 0)
        menu_activate_tab((g_menuTab + dir + g_menuTabCount) % g_menuTabCount);
}

static void menu_change(int dir, int reset)
{
    if (g_moSel < 0) return;
    MenuOpt *m = &g_mo[g_moSel];
    if (m->kind == MO_PROFILE) {
        if (dir > 0 && !reset) {
            g_menuProfileChoice = (int)m->value;
            menu_profile_apply();
        }
        return;
    }
    if (m->kind == MO_ACTION && dir > 0 && !strncmp(m->define, "!PROFILE_", 9)) {
        menu_profile_action(m->define);
        return;
    }
    if (!m->present || !MENU_SELECTABLE(m) || m->kind == MO_ACTION) return;
    if (m->kind == MO_KEY && !reset) {
        if (dir > 0) {
            g_menuBinding = 1;
            g_menuBindReady = 0;
            InterlockedExchange(&g_menuBindEvent, 0);
            g_menuSearchFocus = 0;
            g_menuDragging = -1;
            _snwprintf(g_menuStatus, 96, L"Release held keys, then press your new menu key");
            g_menuDirty = 1;
        }
        return;
    }
    double v = m->value;
    if (reset) {
        v = m->def;
    } else if (m->kind == MO_BOOL || m->kind == MO_PACK) {
        v = v > 0.5 ? 0.0 : 1.0;
    } else if (m->kind == MO_ENUM) {
        int k = 0;
        for (int i = 0; i < m->nEnum; i++) if (fabs(m->enumVal[i] - v) < 1e-6) k = i;
        k += dir;
        if (k < 0) k = 0;
        if (k >= m->nEnum) k = m->nEnum - 1;
        v = m->enumVal[k];
    } else {
        v = m->min + floor((v + dir * m->step - m->min) / m->step + 0.5) * m->step;
        if (v < m->min) v = m->min;
        if (v > m->max) v = m->max;
    }
    if (fabs(v - m->value) < 1e-9) return;
    m->value = v;
    if (strcmp(m->define, "AU_MENU_KEY")) g_menuProfileChoice = -1;
    if (!strcmp(m->define, "AU_PATH_TRACING") && v > 0.5) menu_profile_set("AU_PRESET", 2);
    if (m->kind == MO_PACK) {
        g_toggleOff = v < 0.5;
        logf_("shaderpack %s (menu)", g_toggleOff ? "OFF" : "ON");
        InterlockedExchange(&g_reloadRequested, 1);
    } else {
        g_menuWriteAt = now_sec() + 0.35;   // coalesce held keys into one reload
        _snwprintf(g_menuStatus, 96, L"Applying...");
    }
    g_menuDirty = 1;
}

static void menu_scroll(int direction)
{
    if (g_menuBinding) return;
    int maximum = g_menuVisibleCount - g_menuLayout.rows;
    if (maximum < 0) maximum = 0;
    g_moTop += direction;
    if (g_moTop < 0) g_moTop = 0;
    if (g_moTop > maximum) g_moTop = maximum;
    int selected = menu_visible_index(g_moSel);
    if (selected < g_moTop || selected >= g_moTop + g_menuLayout.rows) {
        for (int index = g_moTop; index < g_menuVisibleCount && index < g_moTop + g_menuLayout.rows; index++)
            if (MENU_SELECTABLE(&g_mo[g_menuVisible[index]])) { g_moSel = g_menuVisible[index]; break; }
    }
    g_menuDirty = 1;
}

static void menu_pointer_range(int optionIndex)
{
    MenuOpt *option = &g_mo[optionIndex];
    float left = (float)(g_menuLayout.x + g_menuLayout.controlX + g_menuLayout.row / 2);
    float right = (float)(g_menuLayout.x + g_menuLayout.width - g_menuLayout.pad - g_menuLayout.row / 2);
    if (right <= left || option->kind != MO_RANGE || !option->present) return;
    double fraction = (g_menuPointerX - left) / (right - left);
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    double value = option->min + floor(fraction * (option->max - option->min) / option->step + 0.5) * option->step;
    if (value > option->max) value = option->max;
    if (fabs(value - option->value) < 1e-9) return;
    option->value = value;
    g_menuProfileChoice = -1;
    g_menuWriteAt = now_sec() + 0.35;
    _snwprintf(g_menuStatus, 96, L"Applying...");
    g_menuDirty = 1;
}

static void menu_pointer(int clicked)
{
    MenuLayout *layout = &g_menuLayout;
    if (layout->width <= 0 || layout->height <= 0) return;
    if (g_menuSDLReady && (g_menuPointerX < layout->x || g_menuPointerX >= layout->x + layout->width
        || g_menuPointerY < layout->y || g_menuPointerY >= layout->y + layout->height)) {
        if (g_menuDragging >= 0) menu_pointer_range(g_menuDragging);
        return;
    }
    if (g_menuPointerX < layout->x) g_menuPointerX = (float)layout->x;
    if (g_menuPointerX >= layout->x + layout->width) g_menuPointerX = (float)(layout->x + layout->width - 1);
    if (g_menuPointerY < layout->y) g_menuPointerY = (float)layout->y;
    if (g_menuPointerY >= layout->y + layout->height) g_menuPointerY = (float)(layout->y + layout->height - 1);
    if (g_menuDragging >= 0) menu_pointer_range(g_menuDragging);
    g_menuDirty = 1;
    if (!clicked || now_sec() - g_menuLastClick < 0.12) return;
    g_menuLastClick = now_sec();
    int pointerX = (int)g_menuPointerX - layout->x;
    int pointerY = (int)g_menuPointerY - layout->y;
    if (pointerY < layout->title && pointerX > layout->width - layout->row) {
        g_menuOpen = 0;
        g_menuDragging = -1;
        return;
    }
    if (g_menuBinding) return;
    if (pointerX < layout->rail && pointerY >= layout->title) {
        int tabHeight = (layout->height - layout->title - layout->footer - layout->pad) / (g_menuTabCount > 0 ? g_menuTabCount : 1);
        if (tabHeight < 1) tabHeight = 1;
        int tab = (pointerY - layout->title - layout->pad / 2) / tabHeight;
        if (tab >= 0 && tab < g_menuTabCount) menu_activate_tab(tab);
        return;
    }
    if (pointerY >= layout->title && pointerY < layout->contentTop) {
        g_menuSearchFocus = 1;
        return;
    }
    if (pointerY >= layout->height - layout->footer) {
        int resetTop = layout->footerButtonTop;
        int resetBottom = layout->footerButtonBottom;
        if (pointerX > layout->width - layout->row * 3 && pointerX < layout->width - layout->pad
            && pointerY >= resetTop && pointerY <= resetBottom) {
            if (g_moSel >= 0 && g_mo[g_moSel].kind == MO_PROFILE) menu_change(1, 0);
            else if (g_moSel >= 0 && g_mo[g_moSel].kind == MO_ACTION) {
                if (!strncmp(g_mo[g_moSel].define, "!PROFILE_", 9)) menu_profile_action(g_mo[g_moSel].define);
                else menu_reset_all();
            }
            else menu_change(0, 1);
        }
        return;
    }
    int row = (pointerY - layout->contentTop) / layout->row;
    int visible = g_moTop + row;
    if (pointerY < layout->contentTop || row >= layout->rows || visible < 0 || visible >= g_menuVisibleCount) return;
    int optionIndex = g_menuVisible[visible];
    if (!MENU_SELECTABLE(&g_mo[optionIndex])) return;
    g_moSel = optionIndex;
    g_menuSearchFocus = 0;
    MenuOpt *option = &g_mo[optionIndex];
    if (option->kind == MO_PROFILE) {
        menu_change(1, 0);
        return;
    }
    if (option->kind == MO_ACTION && !strncmp(option->define, "!PROFILE_", 9)) {
        menu_profile_action(option->define);
        return;
    }
    if (option->kind == MO_ACTION) {
        int buttonTop = layout->contentTop + row * layout->row + layout->row / 2;
        if (pointerX >= layout->controlX + layout->pad && pointerX <= layout->width - layout->pad * 2
            && pointerY >= buttonTop && pointerY <= buttonTop + layout->row / 2 - layout->row * 8 / 58) {
            if (!strncmp(option->define, "!PROFILE_", 9)) menu_profile_action(option->define);
            else menu_reset_all();
        }
        return;
    }
    if (option->kind == MO_RANGE && pointerY < layout->contentTop + row * layout->row + layout->row / 2) return;
    if (pointerX < layout->controlX || !option->present) return;
    if (option->kind == MO_RANGE) {
        if (pointerX < layout->controlX + layout->row / 2) menu_change(-1, 0);
        else if (pointerX > layout->width - layout->pad - layout->row / 2) menu_change(1, 0);
        else if (pointerX < layout->width - layout->pad - layout->row / 2) {
            g_menuDragging = optionIndex;
            menu_pointer_range(optionIndex);
        }
    } else {
        int direction = option->kind == MO_ENUM && pointerX < layout->controlX + layout->row / 2 ? -1 : 1;
        menu_change(direction, 0);
    }
}

#ifdef _WIN32
static int menu_edit_mode(int foreground, SHORT altState)
{
    return g_menuOpen && foreground && (g_menuBinding || (altState & 0x8000) != 0);
}

#include "menu_sdl.h"
#endif

static RenderUIState render_ui_begin(void)
{
    RenderUIState state = {0};
#if AU_HAS_DLSS
    if (g_renderAppliedScale >= 100 || !g_postPresent) return state;
    GLint framebuffer = -1;
    r_glGetIntegerv(0x8ca6, &framebuffer);
    if (framebuffer != 0) return state;
    ProgRec *program = g_curProg ? prog_rec(g_curProg, 0) : NULL;
    if (!program || (strcmp(program->pname, "Batcher2DFS") && strcmp(program->pname, "TextFS") && strcmp(program->pname, "BasicFS"))) return state;
    int width = 0, height = 0;
    if (!render_physical_size(&width, &height)) return state;
    r_glGetIntegerv(GL_VIEWPORT, state.viewport);
    r_glGetIntegerv(0x0c10, state.scissor);
    if (state.viewport[2] <= 0 || state.viewport[3] <= 0) return state;
    float horizontal = (float)width / state.viewport[2], vertical = (float)height / state.viewport[3];
    void (APIENTRY *viewport)(GLint, GLint, GLsizei, GLsizei) = (void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei))GetProcAddress(g_real, "glViewport");
    void (APIENTRY *scissor)(GLint, GLint, GLsizei, GLsizei) = (void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei))GetProcAddress(g_real, "glScissor");
    viewport(0, 0, width, height);
    scissor((int)roundf(state.scissor[0] * horizontal), (int)roundf(state.scissor[1] * vertical),
            (int)roundf(state.scissor[2] * horizontal), (int)roundf(state.scissor[3] * vertical));
    state.fragmentLocation = program->locUIFragmentScale;
    if (state.fragmentLocation >= 0) r_glUniform2f(state.fragmentLocation, 1.0f / horizontal, 1.0f / vertical);
    state.active = 1;
#endif
    return state;
}

static void render_ui_end(RenderUIState state)
{
    if (!state.active) return;
    void (APIENTRY *viewport)(GLint, GLint, GLsizei, GLsizei) = (void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei))GetProcAddress(g_real, "glViewport");
    void (APIENTRY *scissor)(GLint, GLint, GLsizei, GLsizei) = (void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei))GetProcAddress(g_real, "glScissor");
    viewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
    scissor(state.scissor[0], state.scissor[1], state.scissor[2], state.scissor[3]);
    if (state.fragmentLocation >= 0) r_glUniform2f(state.fragmentLocation, 1.0f, 1.0f);
}

#ifdef _WIN32
static LRESULT CALLBACK menu_wndproc(HWND window, UINT message, WPARAM parameter, LPARAM data)
{
    if (g_menuBinding && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !(data & ((LPARAM)1 << 30)))
        menu_bind_key_event((int)parameter);
    if (!g_menuBinding && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !(data & ((LPARAM)1 << 30)))
        for (int index = 0; index < (int)(sizeof(g_menuKeys) / sizeof(g_menuKeys[0])); index++)
            if (parameter == (WPARAM)(index == 0 ? g_menuHotkey : g_menuKeys[index]) && (menu_shortcut_key((int)parameter) || g_menuOpen))
                InterlockedOr(&g_menuKeyEdges, (LONG)1 << index);
    if (message == WM_SYSKEYDOWN && parameter == VK_F4) {
        g_menuOpen = 0;
        g_menuDragging = -1;
    }
    if (g_menuBinding && (message == WM_CHAR || message == WM_SYSCHAR)) return 0;
    if (g_menuOpen && message == WM_SYSCOMMAND && (parameter & 0xfff0) == SC_KEYMENU) return 0;
    if (g_menuSDLReady && (message == WM_INPUT || message == WM_MOUSEMOVE || message == WM_MOUSEWHEEL
        || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_RBUTTONDOWN
        || message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_SETCURSOR))
        return CallWindowProcW(g_prevWndProc, window, message, parameter, data);
    if (menu_edit_mode(is_game_foreground(), GetAsyncKeyState(VK_MENU))) {
        if (message == WM_INPUT) {
            RAWINPUT input;
            UINT size = sizeof(input);
            UINT received = GetRawInputData((HRAWINPUT)data, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
            if (received != (UINT)-1 && input.header.dwType == RIM_TYPEKEYBOARD && (input.data.keyboard.Flags & RI_KEY_BREAK))
                return CallWindowProcW(g_prevWndProc, window, message, parameter, data);
            if (received != (UINT)-1 && input.header.dwType == RIM_TYPEMOUSE) {
                EnterCriticalSection(&g_menuLock);
                g_menuRawMouse = 1;
                if (!(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    g_menuPointerX += input.data.mouse.lLastX;
                    g_menuPointerY += input.data.mouse.lLastY;
                } else {
                    POINT point;
                    if (GetCursorPos(&point) && ScreenToClient(window, &point)) {
                        g_menuPointerX = (float)point.x;
                        g_menuPointerY = (float)point.y;
                    }
                }
                if (input.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) g_menuDragging = -1;
                menu_pointer((input.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0);
                if (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                    menu_scroll(-((short)input.data.mouse.usButtonData / WHEEL_DELTA) * 3);
                LeaveCriticalSection(&g_menuLock);
            }
            return DefWindowProcW(window, message, parameter, data);
        }
        if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEWHEEL) {
            EnterCriticalSection(&g_menuLock);
            if (!g_menuRawMouse) {
                if (message != WM_MOUSEWHEEL) {
                    g_menuPointerX = (float)(short)LOWORD(data);
                    g_menuPointerY = (float)(short)HIWORD(data);
                }
                if (message == WM_LBUTTONUP) g_menuDragging = -1;
                if (message == WM_MOUSEWHEEL) menu_scroll(-GET_WHEEL_DELTA_WPARAM(parameter) / WHEEL_DELTA * 3);
                else menu_pointer(message == WM_LBUTTONDOWN);
            }
            LeaveCriticalSection(&g_menuLock);
            return 0;
        }
        if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) return 0;
        if (message == WM_SETCURSOR) { SetCursor(NULL); return 1; }
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && parameter == VK_RETURN && g_menuPresetNaming) {
            EnterCriticalSection(&g_menuLock);
            menu_profile_finish();
            LeaveCriticalSection(&g_menuLock);
            return 0;
        }
        if (message == WM_CHAR || message == WM_SYSCHAR) {
            EnterCriticalSection(&g_menuLock);
            if (g_menuSearchFocus && parameter == 8 && *g_menuSearch) {
                g_menuSearch[wcslen(g_menuSearch) - 1] = 0;
                g_moTop = 0;
                if (!g_menuPresetNaming) menu_rebuild_rows();
            } else if (g_menuSearchFocus && parameter >= 32 && parameter < 65536) {
                size_t length = wcslen(g_menuSearch);
                if (length < 63) {
                    g_menuSearch[length] = (wchar_t)parameter;
                    g_menuSearch[length + 1] = 0;
                    g_moTop = 0;
                    if (!g_menuPresetNaming) menu_rebuild_rows();
                }
            }
            LeaveCriticalSection(&g_menuLock);
            return 0;
        }
        if (message == WM_KEYDOWN || (message == WM_SYSKEYDOWN && parameter != VK_F4)) return 0;
    }
    if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
        && menu_shortcut_key((int)parameter)) return 0;
    return CallWindowProcW(g_prevWndProc, window, message, parameter, data);
}

static DWORD WINAPI menu_input_thread(LPVOID argument)
{
    (void)argument;
    enum { KEY_COUNT = sizeof(g_menuKeys) / sizeof(g_menuKeys[0]) };
    int keys[KEY_COUNT];
    memcpy(keys, g_menuKeys, sizeof keys);
    int down[KEY_COUNT] = {0};
    double next[KEY_COUNT] = {0};
    for (;;) {
        Sleep(15);
        if (InterlockedExchange(&g_menuTAAWarningPending, 0)) {
            MessageBoxW(g_gameHwnd,
                L"Disable TAA in Hytale's Graphics settings before using Aurora Shaders.\n\nHytale's TAA can cause scene shaking and ghosting. Aurora's own anti-aliasing and DLSS controls are separate.",
                L"Aurora Shaders - Hytale TAA", MB_OK | MB_ICONWARNING);
            write_file("taa-warning-seen", "1", 1);
        }
        double time = now_sec();
        EnterCriticalSection(&g_menuLock);
        if (g_menuWriteAt > 0.0 && time >= g_menuWriteAt) {
            g_menuWriteAt = 0.0;
            menu_save();
            _snwprintf(g_menuStatus, 96, L"Saved - shaders reloading");
            g_menuDirty = 1;
        }
        LeaveCriticalSection(&g_menuLock);
        int foreground = is_game_foreground();
        LONG edges = InterlockedExchange(&g_menuKeyEdges, 0);
        EnterCriticalSection(&g_menuLock);
        int binding = g_menuBinding;
        if (binding) {
            unsigned char pressed[256] = {0};
            for (int key = 8; key < 256; key++) pressed[key] = foreground && (GetAsyncKeyState(key) & 0x8000) != 0;
            menu_bind_poll(pressed, foreground);
        }
        int mouseMode = menu_edit_mode(foreground, GetAsyncKeyState(VK_MENU));
        if (mouseMode != g_menuMouseMode) {
            g_menuMouseMode = mouseMode;
            if (!mouseMode) g_menuSearchFocus = 0;
            g_menuRawMouse = 0;
            g_menuDragging = -1;
            g_menuDirty = 1;
        }
        LeaveCriticalSection(&g_menuLock);
        keys[0] = g_menuHotkey;
        if (binding) {
            for (int index = 0; index < KEY_COUNT; index++) down[index] = foreground && (GetAsyncKeyState(keys[index]) & 0x8000) != 0;
            InterlockedExchange(&g_menuKeyEdges, 0);
            continue;
        }
        for (int index = 0; index < KEY_COUNT; index++) {
            int pressed = foreground && (GetAsyncKeyState(keys[index]) & 0x8000) != 0;
            int fire = foreground && (edges & ((LONG)1 << index)), repeat = 0;
            if (pressed && !down[index]) { if (!g_prevWndProc) fire = 1; next[index] = time + 0.35; }
            else if (pressed && index >= 1 && index <= 4 && time >= next[index]) {
                fire = repeat = 1;
                next[index] = time + 0.06;
            }
            down[index] = pressed;
            if (!fire) continue;
            EnterCriticalSection(&g_menuLock);
            if (index == 0 && !g_menuPresetNaming && !g_menuSearchFocus) {
                g_menuOpen = !g_menuOpen;
                if (g_menuOpen) {
                    menu_load();
                    if (!pack_flag("taa-warning-seen")) InterlockedExchange(&g_menuTAAWarningPending, 1);
                    g_menuStatus[0] = 0;
                    g_menuRawMouse = 0;
                    g_menuDragging = -1;
                    g_menuPointerX = (float)(g_menuLayout.x + g_menuLayout.width / 2);
                    g_menuPointerY = (float)(g_menuLayout.y + g_menuLayout.height / 2);
                }
                g_menuDirty = 1;
            } else if (g_menuOpen) {
                int sliding = g_moSel >= 0 && g_mo[g_moSel].kind == MO_RANGE;
                switch (keys[index]) {
                case VK_UP: g_menuSearchFocus = 0; menu_move(-1); break;
                case VK_DOWN: g_menuSearchFocus = 0; menu_move(1); break;
                case VK_LEFT: if (!g_menuSearchFocus && (!repeat || sliding)) menu_change(-1, 0); break;
                case VK_RIGHT: if (!g_menuSearchFocus && (!repeat || sliding)) menu_change(1, 0); break;
                case VK_PRIOR: menu_section(-1); break;
                case VK_NEXT: menu_section(1); break;
                case VK_ESCAPE: menu_profile_cancel(); g_menuOpen = 0; g_menuDragging = -1; break;
                }
            }
            LeaveCriticalSection(&g_menuLock);
        }
    }
    return 0;
}

#endif // _WIN32 input

// ---- Panel (GDI on Windows, Cairo on Linux, top-down BGRA pixels) ----

#ifdef _WIN32

static HDC g_ovDC;
static HBITMAP g_ovBmp;
static unsigned char *g_ovBits;
static int g_ovW, g_ovH, g_ovTexW, g_ovTexH, g_ovScreenH, g_ovScreenW, g_ovUpload;
static HFONT g_fText, g_fBold, g_fSmall;
static int g_fPx;

static HFONT menu_font(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void menu_fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void menu_text(HDC dc, const wchar_t *s, int x0, int y0, int x1, int y1, HFONT f, COLORREF c, UINT align)
{
    RECT r = { x0, y0, x1, y1 };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void menu_round(HDC dc, int left, int top, int right, int bottom, int radius, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, left, top, right, bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
}

#else
#include "linux_panel.h"
#endif

static void menu_render(int screenWidth, int screenHeight)
{
    double scale = screenHeight / 1080.0;
    if (scale < 0.85) scale = 0.85;
    if (scale > 2.0) scale = 2.0;
    MenuLayout *layout = &g_menuLayout;
    layout->pad = (int)(12 * scale);
    layout->width = (int)(540 * scale);
    if (layout->width > screenWidth - layout->pad * 2) layout->width = screenWidth - layout->pad * 2;
    layout->height = (int)(640 * scale);
    if (layout->height > screenHeight - layout->pad * 2) layout->height = screenHeight - layout->pad * 2;
    layout->footerButtonTop = layout->height - (int)(31 * scale);
    layout->footerButtonBottom = layout->height - (int)(7 * scale);
    layout->rail = (int)(148 * scale);
    layout->title = (int)(60 * scale);
    layout->footer = (int)(104 * scale);
    layout->row = (int)(58 * scale);
    if (g_menuTab < g_menuTabCount && !wcscmp(g_menuTabs[g_menuTab].label, L"Profiles") && !*g_menuSearch)
        layout->row = (int)(38 * scale);
    layout->contentTop = layout->title + (int)(88 * scale);
    layout->rows = (layout->height - layout->footer - layout->contentTop) / layout->row;
    if (layout->rows < 1) layout->rows = 1;
    layout->controlX = layout->rail + layout->pad;
    layout->x = layout->pad;
    layout->y = (screenHeight - layout->height) / 2;
    int selected = menu_visible_index(g_moSel);
    if (selected >= 0) {
        if (selected < g_moTop) g_moTop = selected;
        if (selected >= g_moTop + layout->rows) g_moTop = selected - layout->rows + 1;
    }
    int maximum = g_menuVisibleCount - layout->rows;
    if (maximum < 0) maximum = 0;
    if (g_moTop > maximum) g_moTop = maximum;
    if (g_moTop < 0) g_moTop = 0;
    int width = layout->width, height = layout->height, pad = layout->pad;
    if (width <= 0 || height <= 0) return;
#ifdef _WIN32
    if (!g_ovDC) g_ovDC = CreateCompatibleDC(NULL);
    if (width != g_ovW || height != g_ovH || !g_ovBmp) {
        static HBITMAP baseBitmap;
        if (g_ovBmp) {
            SelectObject(g_ovDC, baseBitmap);
            DeleteObject(g_ovBmp);
        }
        BITMAPINFO bitmap;
        memset(&bitmap, 0, sizeof(bitmap));
        bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
        bitmap.bmiHeader.biWidth = width * 2;
        bitmap.bmiHeader.biHeight = -height * 2;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        g_ovBmp = CreateDIBSection(g_ovDC, &bitmap, DIB_RGB_COLORS, (void **)&g_ovBits, NULL, 0);
        if (!g_ovBmp) { g_ovBits = NULL; return; }
        HGDIOBJ previousBitmap = SelectObject(g_ovDC, g_ovBmp);
        if (!baseBitmap) baseBitmap = (HBITMAP)previousBitmap;
        g_ovW = width;
        g_ovH = height;
        g_ovTexW = width * 2;
        g_ovTexH = height * 2;
        SetMapMode(g_ovDC, MM_ANISOTROPIC);
        SetWindowExtEx(g_ovDC, width, height, NULL);
        SetViewportExtEx(g_ovDC, g_ovTexW, g_ovTexH, NULL);
    }
    int fontPixels = (int)(17 * scale);
    if (fontPixels != g_fPx) {
        SelectObject(g_ovDC, GetStockObject(DEFAULT_GUI_FONT));
        if (g_fText) { DeleteObject(g_fText); DeleteObject(g_fBold); DeleteObject(g_fSmall); }
        g_fText = menu_font(fontPixels, FW_NORMAL);
        g_fBold = menu_font(fontPixels, FW_SEMIBOLD);
        g_fSmall = menu_font((int)(14 * scale), FW_NORMAL);
        g_fPx = fontPixels;
    }
    HDC dc = g_ovDC;
    SetBkMode(dc, TRANSPARENT);
#else
    if (!linux_panel_resize(width, height)) return;
    cairo_identity_matrix(g_ovDC);
    cairo_scale(g_ovDC, 2.0, 2.0);
    g_ovTexW = width * 2;
    g_ovTexH = height * 2;
    g_fText = (LinuxFont){17 * scale, 0};
    g_fBold = (LinuxFont){17 * scale, 1};
    g_fSmall = (LinuxFont){14 * scale, 0};
    cairo_t *dc = g_ovDC;
#endif
    menu_fill(dc, 0, 0, width, height, RGB(13, 20, 29));
    menu_fill(dc, 0, layout->title, layout->rail, height - layout->footer, RGB(10, 16, 24));
    for (int stripe = 0; stripe < layout->title; stripe++)
        menu_fill(dc, 0, stripe, width, stripe + 1, RGB(22 - stripe * 5 / layout->title, 36 - stripe * 9 / layout->title, 49 - stripe * 12 / layout->title));
    menu_fill(dc, pad, layout->title - 2, width - pad, layout->title - 1, RGB(47, 85, 91));
    menu_text(dc, L"Aurora", pad + (int)(6 * scale), (int)(4 * scale), width / 2, layout->title / 2 + (int)(6 * scale),
              g_fBold, RGB(158, 243, 221), DT_LEFT);
    menu_text(dc, L"Shader settings", pad + (int)(6 * scale), layout->title / 2, width / 2, layout->title - (int)(5 * scale),
              g_fSmall, RGB(129, 153, 174), DT_LEFT);
    int badgeLeft = width - (int)(150 * scale), badgeRight = width - layout->row;
    menu_round(dc, badgeLeft, (int)(17 * scale), badgeRight, (int)(42 * scale), (int)(16 * scale),
               g_menuMouseMode ? RGB(26, 75, 69) : RGB(30, 42, 58));
    menu_text(dc, g_menuMouseMode ? L"EDITING" : L"LIVE VIEW", badgeLeft, (int)(17 * scale), badgeRight, (int)(42 * scale),
              g_fSmall, g_menuMouseMode ? RGB(154, 243, 211) : RGB(159, 188, 224), DT_CENTER);
    menu_text(dc, L"\x00D7", width - layout->row, 0, width - pad / 2, layout->title, g_fBold, RGB(173, 195, 218), DT_CENTER);
    int tabHeight = (height - layout->title - layout->footer - pad) / (g_menuTabCount > 0 ? g_menuTabCount : 1);
    if (tabHeight < 1) tabHeight = 1;
    for (int tab = 0; tab < g_menuTabCount; tab++) {
        int top = layout->title + pad / 2 + tab * tabHeight;
        int active = tab == g_menuTab && !*g_menuSearch;
        if (active) {
            menu_round(dc, pad / 2, top + 3, layout->rail - pad / 2, top + tabHeight - 3, (int)(12 * scale), RGB(27, 53, 60));
            menu_fill(dc, pad / 2, top + tabHeight / 3, pad / 2 + (int)(3 * scale), top + tabHeight * 2 / 3, RGB(112, 228, 200));
        }
        menu_text(dc, g_menuTabs[tab].label, pad + (int)(4 * scale), top, layout->rail - pad / 2, top + tabHeight,
                  g_fSmall, active ? RGB(175, 248, 226) : RGB(140, 162, 184), DT_LEFT);
    }
    int contentLeft = layout->rail + pad, contentRight = width - pad;
    int searchTop = layout->title + (int)(12 * scale), searchBottom = searchTop + (int)(32 * scale);
    menu_round(dc, contentLeft, searchTop, contentRight, searchBottom, (int)(14 * scale),
               g_menuSearchFocus ? RGB(31, 60, 68) : RGB(23, 34, 47));
    wchar_t query[96];
    _snwprintf(query, 96, L"%ls%ls%ls", g_menuPresetNaming ? L"Name: " : L"",
              *g_menuSearch ? g_menuSearch : g_menuPresetNaming ? L"Type preset name..." : L"Search settings...",
              g_menuSearchFocus ? L" |" : L"");
    query[95] = 0;
    menu_text(dc, query, contentLeft + pad, searchTop, contentRight - pad, searchBottom, g_fSmall,
              *g_menuSearch ? RGB(232, 243, 248) : RGB(124, 151, 177), DT_LEFT);
    int headingTop = searchBottom + (int)(6 * scale);
    menu_text(dc, *g_menuSearch ? L"Search results" : g_menuTabs[g_menuTab].label, contentLeft + pad / 2, headingTop,
              contentRight - (int)(65 * scale), layout->contentTop - (int)(4 * scale), g_fBold, RGB(220, 236, 247), DT_LEFT);
    wchar_t count[32];
    int shownCount = g_menuVisibleCount;
    if (!wcscmp(g_menuTabs[g_menuTab].label, L"Profiles")) {
        shownCount = 0;
        for (int index = 0; index < g_menuVisibleCount; index++)
            if (g_mo[g_menuVisible[index]].kind == MO_PROFILE) shownCount++;
    }
    _snwprintf(count, 32, L"%d", shownCount);
    menu_text(dc, count, contentRight - (int)(65 * scale), headingTop, contentRight - pad / 2,
              layout->contentTop - (int)(4 * scale), g_fSmall, RGB(123, 150, 176), DT_RIGHT);
    for (int row = 0; row < layout->rows && g_moTop + row < g_menuVisibleCount; row++) {
        int optionIndex = g_menuVisible[g_moTop + row];
        MenuOpt *option = &g_mo[optionIndex];
        int top = layout->contentTop + row * layout->row;
        int active = optionIndex == g_moSel;
        if (option->kind == MO_ACTION && !strncmp(option->define, "!PROFILE_", 9)) {
            wchar_t actionLabel[96];
            if (!strcmp(option->define, "!PROFILE_SAVE")) _snwprintf(actionLabel, 96, L"%ls", g_menuPresetNaming ? L"Save name" : L"Save current settings as...");
            else display_value(option, actionLabel, 96);
            actionLabel[95] = 0;
            int confirming = !strcmp(option->define, "!PROFILE_DELETE") && menu_profile_delete_armed();
            menu_round(dc, contentLeft, top + 2, contentRight, top + layout->row - 4, (int)(8 * scale), confirming ? RGB(126, 52, 44) : RGB(83, 69, 39));
            if (active) menu_fill(dc, contentLeft + (int)(4 * scale), top + (int)(8 * scale), contentLeft + (int)(6 * scale),
                                  top + layout->row - (int)(10 * scale), RGB(139, 235, 206));
            menu_text(dc, actionLabel, contentLeft + pad, top,
                      contentRight - pad, top + layout->row, g_fSmall, RGB(246, 209, 105), DT_CENTER);
            continue;
        }
        if (option->kind == MO_SECTION) {
            menu_text(dc, option->label, contentLeft + pad, top, contentRight - pad, top + layout->row,
                      g_fBold, RGB(153, 202, 201), DT_LEFT);
            continue;
        }
        if (option->kind == MO_PROFILE) {
            int chosen = (int)option->value == g_menuProfileChoice;
            menu_round(dc, contentLeft, top + 2, contentRight, top + layout->row - 4, (int)(8 * scale),
                       chosen ? RGB(30, 70, 62) : active ? RGB(29, 47, 61) : RGB(20, 30, 42));
            if (active) menu_fill(dc, contentLeft + (int)(4 * scale), top + (int)(8 * scale), contentLeft + (int)(6 * scale),
                                  top + layout->row - (int)(10 * scale), RGB(139, 235, 206));
            int indicator = (int)(12 * scale);
            int indicatorLeft = contentLeft + pad, indicatorTop = top + (layout->row - indicator) / 2;
            menu_round(dc, indicatorLeft, indicatorTop, indicatorLeft + indicator, indicatorTop + indicator, indicator,
                       chosen ? RGB(139, 235, 206) : RGB(65, 84, 99));
            wchar_t profileLabel[96];
            _snwprintf(profileLabel, 96, L"%ls%ls", *g_menuSearch && option->value >= 0 && option->value < 12 ? option->value < 6 ? L"RTX " : L"Non-RTX " : L"", option->label);
            profileLabel[95] = 0;
            menu_text(dc, profileLabel, indicatorLeft + indicator + pad, top, contentRight - (int)(74 * scale), top + layout->row,
                      g_fText, chosen ? RGB(206, 252, 232) : RGB(220, 232, 244), DT_LEFT);
            menu_text(dc, chosen ? L"Active" : L"Apply", contentRight - (int)(74 * scale), top, contentRight - pad, top + layout->row,
                      g_fSmall, chosen ? RGB(139, 235, 206) : RGB(246, 209, 105), DT_RIGHT);
            continue;
        }
        int modified = option->present && MENU_SELECTABLE(option) && option->kind != MO_ACTION && fabs(option->value - option->def) > 1e-9;
        menu_round(dc, contentLeft, top + 2, contentRight, top + layout->row - 4, (int)(12 * scale),
                   active ? RGB(29, 47, 61) : RGB(20, 30, 42));
        if (active) menu_fill(dc, contentLeft + (int)(4 * scale), top + (int)(12 * scale), contentLeft + (int)(6 * scale),
                              top + layout->row - (int)(14 * scale), RGB(92, 191, 176));
        int labelRight = contentRight - pad;
        if (option->kind == MO_BOOL || option->kind == MO_PACK || option->kind == MO_RANGE) labelRight -= (int)(65 * scale);
        menu_text(dc, option->label, contentLeft + pad, top + (int)(4 * scale), labelRight, top + layout->row / 2,
                  g_fText, option->present ? RGB(220, 232, 244) : RGB(112, 132, 150), DT_LEFT);
        wchar_t value[64];
        display_value(option, value, 64);
        COLORREF valueColor = modified ? RGB(156, 229, 211) : RGB(246, 209, 105);
        if (option->kind == MO_ACTION || option->kind == MO_KEY) {
            int buttonLeft = contentLeft + pad, buttonRight = contentRight - pad;
            int buttonTop = top + layout->row / 2, buttonBottom = top + layout->row - (int)(8 * scale);
            int confirming = !strcmp(option->define, "!PROFILE_DELETE") ? menu_profile_delete_armed()
                           : !strcmp(option->define, "!RESET_ALL") && now_sec() < g_menuResetUntil;
            menu_round(dc, buttonLeft, buttonTop, buttonRight, buttonBottom, (int)(8 * scale),
                       confirming ? RGB(126, 52, 44) : RGB(83, 69, 39));
            menu_text(dc, value, buttonLeft, buttonTop, buttonRight, buttonBottom, g_fSmall, valueColor, DT_CENTER);
        } else if (option->present && (option->kind == MO_BOOL || option->kind == MO_PACK)) {
            int toggleWidth = (int)(58 * scale), toggleHeight = (int)(24 * scale);
            int toggleLeft = contentRight - pad - toggleWidth, toggleTop = top + (layout->row - toggleHeight) / 2;
            int enabled = option->value > 0.5;
            menu_round(dc, toggleLeft, toggleTop, contentRight - pad, toggleTop + toggleHeight, toggleHeight,
                       enabled ? (modified ? RGB(41, 123, 105) : RGB(117, 92, 37)) : RGB(44, 58, 73));
            int knob = toggleHeight - (int)(6 * scale);
            int knobLeft = enabled ? contentRight - pad - knob - (int)(3 * scale) : toggleLeft + (int)(3 * scale);
            menu_round(dc, knobLeft, toggleTop + (int)(3 * scale), knobLeft + knob, toggleTop + (int)(3 * scale) + knob, knob,
                       modified ? RGB(226, 242, 242) : RGB(246, 209, 105));
            menu_text(dc, enabled ? L"ON" : L"OFF", contentLeft + pad, top + layout->row / 2,
                      toggleLeft - pad, top + layout->row - (int)(6 * scale), g_fSmall, valueColor, DT_LEFT);
        } else if (option->present && option->kind == MO_RANGE) {
            int trackLeft = layout->controlX + layout->row / 2, trackRight = width - pad - layout->row / 2;
            int trackY = top + layout->row * 3 / 4;
            double fraction = option->max > option->min ? (option->value - option->min) / (option->max - option->min) : 0.0;
            if (fraction < 0.0) fraction = 0.0;
            if (fraction > 1.0) fraction = 1.0;
            int handle = trackLeft + (int)(fraction * (trackRight - trackLeft));
            menu_fill(dc, trackLeft, trackY - 1, trackRight, trackY + 2, RGB(48, 65, 82));
            menu_fill(dc, trackLeft, trackY - 1, handle, trackY + 2, RGB(81, 184, 170));
            double defaultFraction = option->max > option->min ? (option->def - option->min) / (option->max - option->min) : 0.0;
            if (defaultFraction < 0.0) defaultFraction = 0.0;
            if (defaultFraction > 1.0) defaultFraction = 1.0;
            int defaultMarker = trackLeft + (int)(defaultFraction * (trackRight - trackLeft));
            menu_fill(dc, defaultMarker - 1, trackY - (int)(5 * scale), defaultMarker + 1, trackY + (int)(5 * scale), RGB(246, 209, 105));
            int radius = (int)(4 * scale);
            menu_round(dc, handle - radius, trackY - radius, handle + radius, trackY + radius, radius * 2, valueColor);
            menu_text(dc, L"\x2212", layout->controlX, top + layout->row / 2, trackLeft, top + layout->row - 4, g_fText, RGB(150, 174, 193), DT_CENTER);
            menu_text(dc, L"+", trackRight, top + layout->row / 2, contentRight, top + layout->row - 4, g_fText, RGB(150, 174, 193), DT_CENTER);
            menu_text(dc, value, labelRight, top + 4, contentRight - pad, top + layout->row / 2, g_fSmall, valueColor, DT_RIGHT);
        } else {
            int lowerTop = top + layout->row / 2, lowerBottom = top + layout->row - (int)(5 * scale);
            if (option->kind == MO_ENUM) {
                menu_text(dc, L"\x2039", layout->controlX, lowerTop, layout->controlX + layout->row / 2, lowerBottom, g_fText, RGB(134, 165, 191), DT_CENTER);
                menu_text(dc, L"\x203A", contentRight - layout->row / 2, lowerTop, contentRight, lowerBottom, g_fText, RGB(134, 165, 191), DT_CENTER);
            }
            menu_text(dc, value, layout->controlX + layout->row / 2, lowerTop, contentRight - layout->row / 2, lowerBottom,
                      g_fSmall, valueColor, option->kind == MO_ENUM ? DT_CENTER : DT_LEFT);
        }
    }
    if (!g_menuVisibleCount) menu_text(dc, L"No matching settings", contentLeft + pad, layout->contentTop,
                                      contentRight, layout->contentTop + layout->row, g_fText, RGB(142, 168, 190), DT_LEFT);
    if (g_menuVisibleCount > layout->rows) {
        int area = layout->rows * layout->row, thumbHeight = area * layout->rows / g_menuVisibleCount;
        int thumbTop = layout->contentTop + (maximum ? g_moTop * (area - thumbHeight) / maximum : 0);
        menu_fill(dc, width - pad / 3, thumbTop, width - pad / 3 + 2, thumbTop + thumbHeight, RGB(87, 153, 157));
    }
    int footerTop = height - layout->footer;
    menu_fill(dc, pad, footerTop, width - pad, footerTop + 1, RGB(37, 57, 73));
#ifdef _WIN32
    const wchar_t *mouseHelp = L"Hold ALT: mouse   |   Arrows: edit   |   PgUp / PgDn: tabs";
#else
    const wchar_t *mouseHelp = L"Hold CTRL: mouse   |   Arrows: edit   |   PgUp / PgDn: tabs";
#endif
    menu_text(dc, mouseHelp, pad, footerTop + 3,
              width - pad, footerTop + (int)(25 * scale), g_fSmall, RGB(130, 161, 185), DT_LEFT);
    if (g_moSel >= 0) {
        MenuOpt *option = &g_mo[g_moSel];
        wchar_t help[256];
        if (option->help[0]) _snwprintf(help, 256, L"%ls", option->help);
        else _snwprintf(help, 256, L"%hs", option->define);
        help[255] = 0;
#ifdef _WIN32
        RECT helpRect = {pad, footerTop + (int)(30 * scale), width - pad, footerTop + (int)(69 * scale)};
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, RGB(160, 181, 199));
        DrawTextW(dc, help, -1, &helpRect, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
#else
        menu_text(dc, help, pad, footerTop + (int)(30 * scale), width - pad,
                  footerTop + (int)(69 * scale), g_fSmall, RGB(160, 181, 199), DT_LEFT);
#endif
    }
    wchar_t defaultText[96] = L"Auto-save enabled";
    if (g_moSel >= 0 && g_mo[g_moSel].kind == MO_PROFILE) {
        _snwprintf(defaultText, 96, L"Click or press Right to apply");
    } else if (g_moSel >= 0 && MENU_SELECTABLE(&g_mo[g_moSel]) && g_mo[g_moSel].kind != MO_ACTION) {
        MenuOpt defaultOption = g_mo[g_moSel];
        defaultOption.value = defaultOption.def;
        wchar_t defaultValue[64];
        display_value(&defaultOption, defaultValue, 64);
        _snwprintf(defaultText, 96, L"Default: %ls", defaultValue);
        defaultText[95] = 0;
    }
    menu_text(dc, defaultText, pad, height - (int)(30 * scale),
              width - layout->row * 3, height - 4, g_fSmall, RGB(246, 209, 105), DT_LEFT);
    menu_round(dc, width - layout->row * 3, layout->footerButtonTop, width - pad, layout->footerButtonBottom, (int)(12 * scale), RGB(30, 47, 62));
    menu_text(dc, g_moSel >= 0 && g_mo[g_moSel].kind == MO_PROFILE ? L"Apply profile"
              : g_moSel >= 0 && g_mo[g_moSel].kind == MO_ACTION ? L"Run action" : L"Reset selected", width - layout->row * 3, layout->footerButtonTop, width - pad, layout->footerButtonBottom,
              g_fSmall, RGB(164, 199, 218), DT_CENTER);
    int pointerX = (int)g_menuPointerX - layout->x, pointerY = (int)g_menuPointerY - layout->y;
#ifdef _WIN32
    POINT pointer[3] = {{pointerX, pointerY}, {pointerX + (int)(5 * scale), pointerY + (int)(18 * scale)},
                        {pointerX + (int)(12 * scale), pointerY + (int)(12 * scale)}};
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(BLACK_PEN));
    if (g_menuMouseMode && !g_menuSDLReady) Polygon(dc, pointer, 3);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    GdiFlush();
#else
    cairo_surface_flush(g_ovBmp);
#endif
    width = g_ovTexW;
    height = g_ovTexH;
    int radius = (int)(24 * scale);
    for (int pixel = 0; pixel < width * height; pixel++) g_ovBits[pixel * 4 + 3] = 248;
    for (int pixelY = 0; pixelY < radius; pixelY++)
        for (int pixelX = 0; pixelX < radius; pixelX++) {
            int cornerX = radius - pixelX, cornerY = radius - pixelY;
            float alpha = 248.0f;
            float distance = sqrtf((float)(cornerX * cornerX + cornerY * cornerY));
            alpha = distance >= radius ? 0.0f : (distance > radius - 1 ? alpha * (radius - distance) : alpha);
            g_ovBits[(pixelY * width + pixelX) * 4 + 3] = (unsigned char)alpha;
            g_ovBits[(pixelY * width + width - pixelX - 1) * 4 + 3] = (unsigned char)alpha;
            g_ovBits[((height - pixelY - 1) * width + pixelX) * 4 + 3] = (unsigned char)alpha;
            g_ovBits[((height - pixelY - 1) * width + width - pixelX - 1) * 4 + 3] = (unsigned char)alpha;
        }
}

// ---- Overlay draw (GL) ----

#define OV_GL_BGRA 0x80E1
#define OV_GL_TRIANGLE_STRIP 0x0005
#define OV_GL_BLEND 0x0BE2
#define OV_GL_DEPTH_TEST 0x0B71
#define OV_GL_CULL_FACE 0x0B44
#define OV_GL_SCISSOR_TEST 0x0C11
#define OV_GL_STENCIL_TEST 0x0B90
#define OV_GL_FRAMEBUFFER_SRGB 0x8DB9
#define OV_GL_RASTERIZER_DISCARD 0x8C89
#define OV_GL_SRC_ALPHA 0x0302
#define OV_GL_ONE_MINUS_SRC_ALPHA 0x0303
#define OV_GL_FUNC_ADD 0x8006
#define OV_GL_DRAW_FRAMEBUFFER 0x8CA9
#define OV_GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define OV_GL_VERTEX_ARRAY_BINDING 0x85B5
#define OV_GL_ACTIVE_TEXTURE 0x84E0
#define OV_GL_SAMPLER_BINDING 0x8919
#define OV_GL_VIEWPORT 0x0BA2
#define OV_GL_BLEND_SRC_RGB 0x80C9
#define OV_GL_BLEND_DST_RGB 0x80C8
#define OV_GL_BLEND_SRC_ALPHA 0x80CB
#define OV_GL_BLEND_DST_ALPHA 0x80CA
#define OV_GL_BLEND_EQUATION_RGB 0x8009
#define OV_GL_BLEND_EQUATION_ALPHA 0x883D
#define OV_GL_COLOR_WRITEMASK 0x0C23
#define OV_GL_PIXEL_UNPACK_BUFFER 0x88EC
#define OV_GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define OV_GL_UNPACK_ROW_LENGTH 0x0CF2
#define OV_GL_UNPACK_SKIP_ROWS 0x0CF3
#define OV_GL_UNPACK_SKIP_PIXELS 0x0CF4
#define OV_UNIT 31

typedef void (APIENTRY *OV_GetIntegerv)(GLenum, GLint *);
typedef void (APIENTRY *OV_GetBooleanv)(GLenum, GLboolean *);
typedef GLboolean (APIENTRY *OV_IsEnabled)(GLenum);
typedef void (APIENTRY *OV_Cap)(GLenum);
typedef void (APIENTRY *OV_Viewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *OV_GenTextures)(GLsizei, GLuint *);
typedef void (APIENTRY *OV_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *OV_TexParameteri)(GLenum, GLenum, GLint);
typedef void (APIENTRY *OV_PixelStorei)(GLenum, GLint);
typedef void (APIENTRY *OV_ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef GLuint (APIENTRY *OV_CreateShader)(GLenum);
typedef void (APIENTRY *OV_ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRY *OV_CompileShader)(GLuint);
typedef GLuint (APIENTRY *OV_CreateProgram)(void);
typedef void (APIENTRY *OV_AttachShader)(GLuint, GLuint);
typedef void (APIENTRY *OV_LinkProgram)(GLuint);
typedef void (APIENTRY *OV_GetProgramiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *OV_UseProgram)(GLuint);
typedef GLint (APIENTRY *OV_GetUniformLocation)(GLuint, const GLchar *);
typedef void (APIENTRY *OV_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *OV_Uniform1i)(GLint, GLint);
typedef void (APIENTRY *OV_GenVertexArrays)(GLsizei, GLuint *);
typedef void (APIENTRY *OV_BindVertexArray)(GLuint);
typedef void (APIENTRY *OV_BindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *OV_ActiveTexture)(GLenum);
typedef void (APIENTRY *OV_BindSampler)(GLuint, GLuint);
typedef void (APIENTRY *OV_BindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *OV_BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void (APIENTRY *OV_BlendEquationSeparate)(GLenum, GLenum);

static struct {
    int state;   // 0 = not tried, 1 = ready, -1 = failed
    GLuint prog, vao, tex;
    GLint locRect, locTex;
    OV_GetIntegerv GetIntegerv;
    OV_GetBooleanv GetBooleanv;
    OV_IsEnabled IsEnabled;
    OV_Cap Enable, Disable;
    OV_Viewport Viewport;
    OV_GenTextures GenTextures;
    OV_TexImage2D TexImage2D;
    OV_TexParameteri TexParameteri;
    OV_PixelStorei PixelStorei;
    OV_ColorMask ColorMask;
    OV_CreateShader CreateShader;
    OV_ShaderSource ShaderSource;
    OV_CompileShader CompileShader;
    OV_CreateProgram CreateProgram;
    OV_AttachShader AttachShader;
    OV_LinkProgram LinkProgram;
    OV_GetProgramiv GetProgramiv;
    OV_UseProgram UseProgram;
    OV_GetUniformLocation GetUniformLocation;
    OV_Uniform4f Uniform4f;
    OV_Uniform1i Uniform1i;
    OV_GenVertexArrays GenVertexArrays;
    OV_BindVertexArray BindVertexArray;
    OV_BindFramebuffer BindFramebuffer;
    OV_ActiveTexture ActiveTexture;
    OV_BindSampler BindSampler;
    OV_BindBuffer BindBuffer;
    OV_BlendFuncSeparate BlendFuncSeparate;
    OV_BlendEquationSeparate BlendEquationSeparate;
} ov;

static int ov_init(void)
{
    if (ov.state) return ov.state > 0;
    ov.state = -1;
#define OV_GL11(field, name) ov.field = (void *)GetProcAddress(g_real, name); if (!ov.field) { logf_("menu: missing %s", name); return 0; }
#define OV_EXT(field, name) ov.field = (void *)real_wglGetProcAddress(name); if (!ov.field) { logf_("menu: missing %s", name); return 0; }
    OV_GL11(GetIntegerv, "glGetIntegerv") OV_GL11(GetBooleanv, "glGetBooleanv") OV_GL11(IsEnabled, "glIsEnabled")
    OV_GL11(Enable, "glEnable") OV_GL11(Disable, "glDisable") OV_GL11(Viewport, "glViewport")
    OV_GL11(GenTextures, "glGenTextures") OV_GL11(TexImage2D, "glTexImage2D") OV_GL11(TexParameteri, "glTexParameteri")
    OV_GL11(PixelStorei, "glPixelStorei") OV_GL11(ColorMask, "glColorMask")
    OV_EXT(CreateShader, "glCreateShader") OV_EXT(ShaderSource, "glShaderSource") OV_EXT(CompileShader, "glCompileShader")
    OV_EXT(CreateProgram, "glCreateProgram") OV_EXT(AttachShader, "glAttachShader") OV_EXT(LinkProgram, "glLinkProgram")
    OV_EXT(GetProgramiv, "glGetProgramiv") OV_EXT(UseProgram, "glUseProgram") OV_EXT(GetUniformLocation, "glGetUniformLocation")
    OV_EXT(Uniform4f, "glUniform4f") OV_EXT(Uniform1i, "glUniform1i") OV_EXT(GenVertexArrays, "glGenVertexArrays")
    OV_EXT(BindVertexArray, "glBindVertexArray") OV_EXT(BindFramebuffer, "glBindFramebuffer")
    OV_EXT(ActiveTexture, "glActiveTexture") OV_EXT(BindSampler, "glBindSampler") OV_EXT(BindBuffer, "glBindBuffer")
    OV_EXT(BlendFuncSeparate, "glBlendFuncSeparate") OV_EXT(BlendEquationSeparate, "glBlendEquationSeparate")
#undef OV_GL11
#undef OV_EXT
    static const char *vs =
        "#version 410 core\n"
        "uniform vec4 uRect;\n"
        "out vec2 vUV;\n"
        "void main() {\n"
        "    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
        "    vUV = vec2(c.x, 1.0 - c.y);\n"
        "    gl_Position = vec4(mix(uRect.xy, uRect.zw, c), 0.0, 1.0);\n"
        "}\n";
    static const char *fs =
        "#version 410 core\n"
        "uniform sampler2D uTex;\n"
        "in vec2 vUV;\n"
        "out vec4 outColor;\n"
        "void main() { outColor = texture(uTex, vUV); }\n";
    GLuint v = ov.CreateShader(GL_VERTEX_SHADER), f = ov.CreateShader(GL_FRAGMENT_SHADER);
    ov.ShaderSource(v, 1, &vs, NULL);
    ov.ShaderSource(f, 1, &fs, NULL);
    ov.CompileShader(v);
    ov.CompileShader(f);
    ov.prog = ov.CreateProgram();
    ov.AttachShader(ov.prog, v);
    ov.AttachShader(ov.prog, f);
    ov.LinkProgram(ov.prog);
    GLint ok = 0;
    ov.GetProgramiv(ov.prog, GL_LINK_STATUS, &ok);
    if (!ok) { logf_("menu: overlay program failed to link"); return 0; }
    ov.locRect = ov.GetUniformLocation(ov.prog, "uRect");
    ov.locTex = ov.GetUniformLocation(ov.prog, "uTex");
    ov.GenVertexArrays(1, &ov.vao);
    ov.GenTextures(1, &ov.tex);
    ov.state = 1;
    logf_("menu: overlay ready");
    return 1;
}

static void menu_draw(int sw, int sh)
{
    GLint prog, vao, fbo, active, tex, sampler, vp[4], unpackBuf, rowLen, skipRows, skipPix, align;
    GLint bsRGB, bdRGB, bsA, bdA, beRGB, beA;
    GLboolean cm[4];
    ov.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    ov.GetIntegerv(OV_GL_VERTEX_ARRAY_BINDING, &vao);
    ov.GetIntegerv(OV_GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    ov.GetIntegerv(OV_GL_ACTIVE_TEXTURE, &active);
    ov.GetIntegerv(OV_GL_VIEWPORT, vp);
    ov.GetIntegerv(OV_GL_BLEND_SRC_RGB, &bsRGB);
    ov.GetIntegerv(OV_GL_BLEND_DST_RGB, &bdRGB);
    ov.GetIntegerv(OV_GL_BLEND_SRC_ALPHA, &bsA);
    ov.GetIntegerv(OV_GL_BLEND_DST_ALPHA, &bdA);
    ov.GetIntegerv(OV_GL_BLEND_EQUATION_RGB, &beRGB);
    ov.GetIntegerv(OV_GL_BLEND_EQUATION_ALPHA, &beA);
    ov.GetBooleanv(OV_GL_COLOR_WRITEMASK, cm);
    static const GLenum caps[] = { OV_GL_BLEND, OV_GL_DEPTH_TEST, OV_GL_CULL_FACE, OV_GL_SCISSOR_TEST, OV_GL_STENCIL_TEST,
                                   OV_GL_FRAMEBUFFER_SRGB, OV_GL_RASTERIZER_DISCARD };
    GLboolean capOn[7];
    for (int i = 0; i < 7; i++) capOn[i] = ov.IsEnabled(caps[i]);

    ov.ActiveTexture(GL_TEXTURE0 + OV_UNIT);
    ov.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    ov.GetIntegerv(OV_GL_SAMPLER_BINDING, &sampler);
    r_glBindTexture(GL_TEXTURE_2D, ov.tex);
    ov.BindSampler(OV_UNIT, 0);
    if (g_ovUpload) {
        ov.GetIntegerv(OV_GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuf);
        ov.GetIntegerv(OV_GL_UNPACK_ROW_LENGTH, &rowLen);
        ov.GetIntegerv(OV_GL_UNPACK_SKIP_ROWS, &skipRows);
        ov.GetIntegerv(OV_GL_UNPACK_SKIP_PIXELS, &skipPix);
        ov.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        ov.BindBuffer(OV_GL_PIXEL_UNPACK_BUFFER, 0);
        ov.PixelStorei(OV_GL_UNPACK_ROW_LENGTH, 0);
        ov.PixelStorei(OV_GL_UNPACK_SKIP_ROWS, 0);
        ov.PixelStorei(OV_GL_UNPACK_SKIP_PIXELS, 0);
        ov.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        ov.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_ovTexW, g_ovTexH, 0, OV_GL_BGRA, GL_UNSIGNED_BYTE, g_ovBits);
        ov.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        ov.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ov.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ov.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        ov.BindBuffer(OV_GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuf);
        ov.PixelStorei(OV_GL_UNPACK_ROW_LENGTH, rowLen);
        ov.PixelStorei(OV_GL_UNPACK_SKIP_ROWS, skipRows);
        ov.PixelStorei(OV_GL_UNPACK_SKIP_PIXELS, skipPix);
        ov.PixelStorei(GL_UNPACK_ALIGNMENT, align);
        g_ovUpload = 0;
    }

    ov.BindFramebuffer(OV_GL_DRAW_FRAMEBUFFER, 0);
    ov.Viewport(0, 0, sw, sh);
    for (int i = 1; i < 7; i++) ov.Disable(caps[i]);
    ov.Enable(OV_GL_BLEND);
    ov.BlendFuncSeparate(OV_GL_SRC_ALPHA, OV_GL_ONE_MINUS_SRC_ALPHA, 1, OV_GL_ONE_MINUS_SRC_ALPHA);
    ov.BlendEquationSeparate(OV_GL_FUNC_ADD, OV_GL_FUNC_ADD);
    ov.ColorMask(1, 1, 1, 1);
    ov.UseProgram(ov.prog);
    int px = g_menuLayout.x, py = g_menuLayout.y;
    float x0 = -1.0f + 2.0f * px / sw, x1 = -1.0f + 2.0f * (px + g_ovW) / sw;
    float y0 = 1.0f - 2.0f * (py + g_ovH) / sh, y1 = 1.0f - 2.0f * py / sh;
    ov.Uniform4f(ov.locRect, x0, y0, x1, y1);
    ov.Uniform1i(ov.locTex, OV_UNIT);
    ov.BindVertexArray(ov.vao);
    r_glDrawArrays(OV_GL_TRIANGLE_STRIP, 0, 4);

    // Restore the game's state.
    ov.BindVertexArray((GLuint)vao);
    ov.UseProgram((GLuint)prog);
    r_glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    ov.BindSampler(OV_UNIT, (GLuint)sampler);
    ov.ActiveTexture((GLenum)active);
    ov.BindFramebuffer(OV_GL_DRAW_FRAMEBUFFER, (GLuint)fbo);
    ov.Viewport(vp[0], vp[1], vp[2], vp[3]);
    ov.BlendFuncSeparate((GLenum)bsRGB, (GLenum)bdRGB, (GLenum)bsA, (GLenum)bdA);
    ov.BlendEquationSeparate((GLenum)beRGB, (GLenum)beA);
    ov.ColorMask(cm[0], cm[1], cm[2], cm[3]);
    for (int i = 0; i < 7; i++) { if (capOn[i]) ov.Enable(caps[i]); else ov.Disable(caps[i]); }
}

#ifdef _WIN32
static void menu_frame(HDC hdc)
{
    menu_sdl_install();
    if (!g_gameHwnd) {
        HWND w = WindowFromDC(hdc);
        if (w) {
            g_gameHwnd = w;
            g_prevWndProc = (WNDPROC)GetWindowLongPtrW(w, GWLP_WNDPROC);
            SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)menu_wndproc);
            logf_("menu: game window %p, menu hotkey 0x%x", (void *)w, (unsigned)g_menuHotkey);
        }
    }
    if (!g_menuOpen || !g_gameHwnd) return;
    RECT rc;
    GetClientRect(g_gameHwnd, &rc);
    int sw = rc.right - rc.left, sh = rc.bottom - rc.top;
    if (sw <= 0 || sh <= 0 || !ov_init()) return;
    EnterCriticalSection(&g_menuLock);
    if (g_menuDirty || sh != g_ovScreenH || sw != g_ovScreenW) {
        menu_render(sw, sh);
        g_ovScreenH = sh;
        g_ovScreenW = sw;
        g_ovUpload = 1;
        g_menuDirty = 0;
    }
    if (g_ovBits) menu_draw(sw, sh);
    LeaveCriticalSection(&g_menuLock);
}

typedef BOOL (WINAPI *PFN_wglSwapBuffers)(HDC);
static PFN_wglSwapBuffers r_wglSwapBuffers;

__declspec(dllexport) BOOL WINAPI wglSwapBuffers(HDC hdc)
{
    g_postPresent = 0;
    menu_frame(hdc);
    int generated = framegen_swap();
    g_frame++;
    if (generated) return TRUE;
    return r_wglSwapBuffers(hdc);
}

#endif // _WIN32 presentation

// Shared dispatch for WGL and SDL3.
static PROC hook_get_proc(const char *name)
{
    PROC p = real_wglGetProcAddress(name);
    if (!p || !name) return p;
#define HOOK(fn) if (!strcmp(name, #fn)) { r_##fn = (PFN_##fn)p; return (PROC)hook_##fn; }
    HOOK(glShaderSource) HOOK(glLinkProgram) HOOK(glDeleteProgram) HOOK(glDeleteShader) HOOK(glUseProgram)
    HOOK(glGetUniformLocation) HOOK(glUniformBlockBinding)
    HOOK(glUniform1f) HOOK(glUniform2f) HOOK(glUniform3f) HOOK(glUniform4f) HOOK(glUniform1i) HOOK(glUniform2i)
    HOOK(glUniform3i) HOOK(glUniform4i) HOOK(glUniform3ui) HOOK(glUniform1fv) HOOK(glUniform2fv) HOOK(glUniform3fv)
    HOOK(glUniform4fv) HOOK(glUniform1iv) HOOK(glUniform1uiv) HOOK(glUniformMatrix2fv) HOOK(glUniformMatrix3fv)
    HOOK(glUniformMatrix4fv) HOOK(glActiveTexture) HOOK(glBindSampler)
    HOOK(glDrawArraysInstanced) HOOK(glDrawElementsInstanced) HOOK(glBlitFramebuffer)
    HOOK(glBufferData) HOOK(glBufferSubData)
    HOOK(glMapBufferRange) HOOK(glUnmapBuffer)
#undef HOOK
    if (!strcmp(name, "glBindTexture")) { r_glBindTexture = (PFN_glBindTexture)p; return (PROC)glBindTexture; }
    if (!strcmp(name, "glDrawArrays")) { r_glDrawArrays = (PFN_glDrawArrays)p; return (PROC)glDrawArrays; }
    if (!strcmp(name, "glDrawElements")) { r_glDrawElements = (PFN_glDrawElements)p; return (PROC)glDrawElements; }
    if (!strcmp(name, "glGetFramebufferAttachmentParameteriv"))
        r_glGetFramebufferAttachmentParameteriv = (PFN_glGetFramebufferAttachmentParameteriv)p;
    return p;
}

#ifdef _WIN32

__declspec(dllexport) PROC WINAPI wglGetProcAddress(LPCSTR name)
{
    return hook_get_proc(name);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        return TRUE;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);

        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        wcscat(sys, L"\\opengl32.dll");
        g_real = LoadLibraryW(sys);
        if (!g_real) return FALSE;
        real_wglGetProcAddress = (PFN_wglGetProcAddress)GetProcAddress(g_real, "wglGetProcAddress");
        r_glBindTexture = (PFN_glBindTexture)GetProcAddress(g_real, "glBindTexture");
        r_glDrawArrays = (PFN_glDrawArrays)GetProcAddress(g_real, "glDrawArrays");
        r_glDrawElements = (PFN_glDrawElements)GetProcAddress(g_real, "glDrawElements");
        r_wglSwapBuffers = (PFN_wglSwapBuffers)GetProcAddress(g_real, "wglSwapBuffers");
        int missing = stubs_resolve(g_real);

        GetModuleFileNameW(inst, g_packDir, MAX_PATH);
        wchar_t *slash = wcsrchr(g_packDir, L'\\');
        if (slash) *slash = 0;
        wcscat(g_packDir, L"\\shaderpack");

        wchar_t path[MAX_PATH];
        pack_path(path, "hook.log");
        g_log = _wfopen(path, L"w");
        pack_path(path, "dump");
        g_dump = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        pack_path(path, "errors");
        CreateDirectoryW(path, NULL);
        logf_("hytale shader hook loaded, missing stubs=%d dump=%d", missing, g_dump);
        g_enabled = !pack_flag("programs\\_disabled");
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);
        settings_load();
        load_index();
        load_shares();
        load_textures_cfg();
        load_forward_cfg();
        CreateThread(NULL, 0, watcher_thread, NULL, 0, NULL);
        InitializeCriticalSection(&g_menuLock);
        CreateThread(NULL, 0, menu_input_thread, NULL, 0, NULL);
    }
    return TRUE;
}

#else
#include "linux_runtime.h"
#endif
