#define SSS_MASK_UNIT HOOK_UNIT_BASE
#define SSS_MASK_MAX_RECORDS 2047

typedef struct { unsigned hash; unsigned char pixels[1024]; } SSSMaskRecord;
static SSSMaskRecord *g_sssMasks;
static unsigned g_sssMaskCount;
static GLuint g_sssMaskTexture;

static void sss_masks_load(void)
{
    free(g_sssMasks);
    g_sssMasks = NULL;
    g_sssMaskCount = 0;
    size_t length = 0;
    char *data = read_file("sss-masks.bin", &length);
    if (!data) return;
    unsigned count = 0;
    if (length >= 12) memcpy(&count, data + 8, 4);
    if (length < 12 || memcmp(data, "AUSSS001", 8) || count > SSS_MASK_MAX_RECORDS
        || length != 12 + (size_t)count * sizeof(SSSMaskRecord)) {
        logf_("SSS: invalid mask file; foliage keeps its original shadows");
        free(data);
        return;
    }
    if (count) {
        g_sssMasks = malloc((size_t)count * sizeof(*g_sssMasks));
        if (!g_sssMasks) { free(data); return; }
        memcpy(g_sssMasks, data + 12, (size_t)count * sizeof(*g_sssMasks));
        for (unsigned index = 1; index < count; index++) {
            if (g_sssMasks[index - 1].hash >= g_sssMasks[index].hash) {
                free(g_sssMasks);
                g_sssMasks = NULL;
                free(data);
                logf_("SSS: unsorted mask file; masks not loaded");
                return;
            }
        }
    }
    g_sssMaskCount = count;
    free(data);
    logf_("SSS: %u foliage transmission masks loaded", count);
}

static unsigned sss_mask_find(unsigned hash)
{
    unsigned lower = 0, upper = g_sssMaskCount;
    while (lower < upper) {
        unsigned middle = lower + (upper - lower) / 2;
        if (g_sssMasks[middle].hash < hash) lower = middle + 1;
        else upper = middle;
    }
    return lower < g_sssMaskCount && g_sssMasks[lower].hash == hash ? lower + 1 : 0;
}

static unsigned sss_mask_tile(unsigned hash, unsigned reversed)
{
    unsigned index = sss_mask_find(hash);
    if (index) return index;
    index = sss_mask_find(reversed);
    return index ? index | 32768u : 0;
}
