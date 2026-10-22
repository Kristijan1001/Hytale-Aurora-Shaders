#define EMISSION_UNIT (HOOK_UNIT_BASE + 1)
#define EMISSION_VARIANTS 16
typedef struct { unsigned hash, tint, color, average; } EmissionRecord;
static EmissionRecord *g_emissions;
static int g_emissionCount;
static GLuint g_emissionTexture;

static void emissions_load(void)
{
    free(g_emissions);
    g_emissions = NULL;
    g_emissionCount = 0;
    char *text = read_file("emitters.txt", NULL);
    if (!text) return;
    int capacity = 1;
    for (char *character = text; *character; character++) if (*character == '\n') capacity++;
    g_emissions = malloc((size_t)capacity * sizeof(*g_emissions));
    if (!g_emissions) { free(text); return; }
    char *context = NULL;
    for (char *line = strtok_s(text, "\n", &context); line; line = strtok_s(NULL, "\n", &context)) {
        EmissionRecord record;
        if (sscanf(line, "%x %x %x %x", &record.hash, &record.tint, &record.color, &record.average) == 4)
            g_emissions[g_emissionCount++] = record;
    }
    free(text);
    logf_("emissives: %d native texture/tint records", g_emissionCount);
}

static void emissions_tile(unsigned hash, unsigned reverse, unsigned char *map, int width, int height,
                           int page, int tileX, int tileY)
{
    if (!map) return;
    int start = -1;
    for (int pass = 0; pass < 2 && start < 0; pass++) {
        unsigned key = pass ? reverse : hash;
        int low = 0, high = g_emissionCount;
        while (low < high) {
            int middle = low + (high - low) / 2;
            if (g_emissions[middle].hash < key) low = middle + 1; else high = middle;
        }
        if (low < g_emissionCount && g_emissions[low].hash == key) start = low;
    }
    if (start < 0) return;
    unsigned key = g_emissions[start].hash;
    int variant = 0;
    for (int index = start; index < g_emissionCount && g_emissions[index].hash == key && variant < EMISSION_VARIANTS; index++, variant++) {
        EmissionRecord *record = &g_emissions[index];
        size_t offset = (((size_t)(page * EMISSION_VARIANTS * 2 + variant * 2) * height + tileY) * width + tileX) * 4;
        size_t tintOffset = offset + (size_t)width * height * 4;
        unsigned red = (record->average >> 16) & 255u, green = (record->average >> 8) & 255u, blue = record->average & 255u;
        unsigned maximum = red > green ? red : green;
        if (blue > maximum) maximum = blue;
        if (!maximum) maximum = 1;
        map[offset] = (unsigned char)(((record->color >> 16) & 255u) * red / maximum);
        map[offset + 1] = (unsigned char)(((record->color >> 8) & 255u) * green / maximum);
        map[offset + 2] = (unsigned char)((record->color & 255u) * blue / maximum);
        map[offset + 3] = 255;
        map[tintOffset] = (unsigned char)(record->tint >> 16);
        map[tintOffset + 1] = (unsigned char)(record->tint >> 8);
        map[tintOffset + 2] = (unsigned char)record->tint;
        map[tintOffset + 3] = record->tint == 0xffffffffu ? 0 : 255;
    }
}
