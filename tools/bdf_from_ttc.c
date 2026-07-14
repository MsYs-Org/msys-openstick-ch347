#define _FILE_OFFSET_BITS 64

#include <dlfcn.h>
#include <errno.h>
#include <iconv.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Offline TTC/OTF -> ISO10646 BDF generator for the minimal OpenStick image.
 * The board has the FreeType runtime but deliberately carries no development
 * headers.  Only ABI-stable public FreeType structures used by this tool are
 * declared here and functions are resolved with dlsym.  The generated BDF is
 * committed into the driver package; this program never runs at boot.
 */

typedef signed long FT_Fixed;
typedef signed long FT_Pos;
typedef signed long FT_Long;
typedef unsigned long FT_ULong;
typedef signed int FT_Int;
typedef unsigned int FT_UInt;
typedef signed short FT_Short;
typedef unsigned short FT_UShort;
typedef unsigned char FT_Byte;
typedef char FT_String;
typedef int FT_Error;
typedef uint32_t FT_Encoding;
typedef uint32_t FT_Glyph_Format;

typedef struct FT_LibraryRec_ *FT_Library;
typedef struct FT_FaceRec_ *FT_Face;
typedef struct FT_SizeRec_ *FT_Size;
typedef struct FT_CharMapRec_ *FT_CharMap;
typedef struct FT_GlyphSlotRec_ *FT_GlyphSlot;

typedef struct FT_Generic_ {
    void *data;
    void (*finalizer)(void *object);
} FT_Generic;

typedef struct FT_Vector_ {
    FT_Pos x;
    FT_Pos y;
} FT_Vector;

typedef struct FT_BBox_ {
    FT_Pos xMin;
    FT_Pos yMin;
    FT_Pos xMax;
    FT_Pos yMax;
} FT_BBox;

typedef struct FT_Bitmap_Size_ {
    FT_Short height;
    FT_Short width;
    FT_Pos size;
    FT_Pos x_ppem;
    FT_Pos y_ppem;
} FT_Bitmap_Size;

typedef struct FT_Glyph_Metrics_ {
    FT_Pos width;
    FT_Pos height;
    FT_Pos horiBearingX;
    FT_Pos horiBearingY;
    FT_Pos horiAdvance;
    FT_Pos vertBearingX;
    FT_Pos vertBearingY;
    FT_Pos vertAdvance;
} FT_Glyph_Metrics;

typedef struct FT_Bitmap_ {
    unsigned int rows;
    unsigned int width;
    int pitch;
    unsigned char *buffer;
    unsigned short num_grays;
    unsigned char pixel_mode;
    unsigned char palette_mode;
    void *palette;
} FT_Bitmap;

typedef struct FT_Outline_ {
    short n_contours;
    short n_points;
    FT_Vector *points;
    char *tags;
    short *contours;
    int flags;
} FT_Outline;

typedef struct FT_FaceRec_ {
    FT_Long num_faces;
    FT_Long face_index;
    FT_Long face_flags;
    FT_Long style_flags;
    FT_Long num_glyphs;
    FT_String *family_name;
    FT_String *style_name;
    FT_Int num_fixed_sizes;
    FT_Bitmap_Size *available_sizes;
    FT_Int num_charmaps;
    FT_CharMap *charmaps;
    FT_Generic generic;
    FT_BBox bbox;
    FT_UShort units_per_EM;
    FT_Short ascender;
    FT_Short descender;
    FT_Short height;
    FT_Short max_advance_width;
    FT_Short max_advance_height;
    FT_Short underline_position;
    FT_Short underline_thickness;
    FT_GlyphSlot glyph;
    FT_Size size;
    FT_CharMap charmap;
} FT_FaceRec;

typedef struct FT_GlyphSlotRec_ {
    FT_Library library;
    FT_Face face;
    FT_GlyphSlot next;
    FT_UInt glyph_index;
    FT_Generic generic;
    FT_Glyph_Metrics metrics;
    FT_Fixed linearHoriAdvance;
    FT_Fixed linearVertAdvance;
    FT_Vector advance;
    FT_Glyph_Format format;
    FT_Bitmap bitmap;
    FT_Int bitmap_left;
    FT_Int bitmap_top;
    FT_Outline outline;
} FT_GlyphSlotRec;

typedef FT_Error (*ft_init_fn)(FT_Library *library);
typedef FT_Error (*ft_new_face_fn)(FT_Library, const char *, FT_Long, FT_Face *);
typedef FT_Error (*ft_done_face_fn)(FT_Face);
typedef FT_Error (*ft_done_library_fn)(FT_Library);
typedef FT_Error (*ft_select_charmap_fn)(FT_Face, FT_Encoding);
typedef FT_Error (*ft_set_pixel_sizes_fn)(FT_Face, FT_UInt, FT_UInt);
typedef FT_UInt (*ft_get_char_index_fn)(FT_Face, FT_ULong);
typedef FT_Error (*ft_load_char_fn)(FT_Face, FT_ULong, FT_Int);

typedef struct ft_api {
    void *handle;
    ft_init_fn init;
    ft_new_face_fn new_face;
    ft_done_face_fn done_face;
    ft_done_library_fn done_library;
    ft_select_charmap_fn select_charmap;
    ft_set_pixel_sizes_fn set_pixel_sizes;
    ft_get_char_index_fn get_char_index;
    ft_load_char_fn load_char;
} ft_api;

typedef struct codepoints {
    uint32_t *items;
    unsigned char *seen;
    size_t count;
    size_t capacity;
} codepoints;

typedef struct font_metrics {
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    int max_advance;
    uint64_t advance_sum;
    size_t glyph_count;
} font_metrics;

#define FT_ENCODING_UNICODE UINT32_C(0x756E6963)
#define FT_LOAD_RENDER (1L << 2)
#define FT_LOAD_MONOCHROME (1L << 12)
#define FT_LOAD_TARGET_MONO (2L << 16)
#define FT_PIXEL_MODE_MONO 1
#define FT_PIXEL_MODE_GRAY 2
#define MAX_CODEPOINTS 65536U

static int load_symbol(void *library, const char *name, void *target,
                       size_t target_size)
{
    void *symbol = dlsym(library, name);
    if (symbol == NULL || target_size != sizeof(symbol))
        return 0;
    memcpy(target, &symbol, sizeof(symbol));
    return 1;
}

static int ft_api_open(ft_api *api)
{
    memset(api, 0, sizeof(*api));
    api->handle = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
    if (api->handle == NULL) {
        fprintf(stderr, "bdf_from_ttc: cannot load libfreetype.so.6: %s\n",
                dlerror());
        return 0;
    }
#define LOAD_FT(field, name)                                                   \
    do {                                                                       \
        if (!load_symbol(api->handle, name, &api->field, sizeof(api->field))) \
            goto missing_symbol;                                               \
    } while (0)
    LOAD_FT(init, "FT_Init_FreeType");
    LOAD_FT(new_face, "FT_New_Face");
    LOAD_FT(done_face, "FT_Done_Face");
    LOAD_FT(done_library, "FT_Done_FreeType");
    LOAD_FT(select_charmap, "FT_Select_Charmap");
    LOAD_FT(set_pixel_sizes, "FT_Set_Pixel_Sizes");
    LOAD_FT(get_char_index, "FT_Get_Char_Index");
    LOAD_FT(load_char, "FT_Load_Char");
#undef LOAD_FT
    return 1;

missing_symbol:
    fprintf(stderr, "bdf_from_ttc: incompatible FreeType runtime\n");
    dlclose(api->handle);
    memset(api, 0, sizeof(*api));
    return 0;
}

static void ft_api_close(ft_api *api)
{
    if (api->handle != NULL)
        dlclose(api->handle);
    memset(api, 0, sizeof(*api));
}

static int codepoint_compare(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return a < b ? -1 : a > b;
}

static int add_codepoint(codepoints *set, uint32_t value)
{
    uint32_t *replacement;
    size_t capacity;

    if (value > UINT32_C(0x10FFFF) ||
        (value >= UINT32_C(0xD800) && value <= UINT32_C(0xDFFF)))
        return 1;
    if ((set->seen[value >> 3] & (1U << (value & 7U))) != 0)
        return 1;
    if (set->count >= MAX_CODEPOINTS) {
        fprintf(stderr, "bdf_from_ttc: codepoint limit exceeded\n");
        return 0;
    }
    if (set->count == set->capacity) {
        capacity = set->capacity == 0 ? 8192U : set->capacity * 2U;
        replacement = realloc(set->items, capacity * sizeof(*replacement));
        if (replacement == NULL)
            return 0;
        set->items = replacement;
        set->capacity = capacity;
    }
    set->seen[value >> 3] |= (unsigned char)(1U << (value & 7U));
    set->items[set->count++] = value;
    return 1;
}

static void sort_unique(codepoints *set)
{
    size_t source;
    size_t target = 0;

    qsort(set->items, set->count, sizeof(*set->items), codepoint_compare);
    for (source = 0; source < set->count; ++source) {
        if (target == 0 || set->items[source] != set->items[target - 1])
            set->items[target++] = set->items[source];
    }
    set->count = target;
}

static int add_gb2312(codepoints *set)
{
    iconv_t converter;
    unsigned int lead;
    unsigned int trail;

    converter = iconv_open("UTF-32BE", "GB2312");
    if (converter == (iconv_t)-1) {
        fprintf(stderr, "bdf_from_ttc: GB2312 iconv unavailable: %s\n",
                strerror(errno));
        return 0;
    }
    for (lead = 0xA1; lead <= 0xF7; ++lead) {
        for (trail = 0xA1; trail <= 0xFE; ++trail) {
            char input_storage[2] = {(char)lead, (char)trail};
            unsigned char output_storage[4] = {0, 0, 0, 0};
            char *input = input_storage;
            char *output = (char *)output_storage;
            size_t input_left = sizeof(input_storage);
            size_t output_left = sizeof(output_storage);
            uint32_t value;

            (void)iconv(converter, NULL, NULL, NULL, NULL);
            errno = 0;
            if (iconv(converter, &input, &input_left, &output, &output_left) ==
                    (size_t)-1 ||
                input_left != 0 || output_left != 0)
                continue;
            value = ((uint32_t)output_storage[0] << 24) |
                    ((uint32_t)output_storage[1] << 16) |
                    ((uint32_t)output_storage[2] << 8) |
                    output_storage[3];
            if (!add_codepoint(set, value)) {
                iconv_close(converter);
                return 0;
            }
        }
    }
    iconv_close(converter);
    return 1;
}

static int decode_utf8_file(codepoints *set, const char *path)
{
    FILE *stream = fopen(path, "rb");
    int byte;
    uint32_t value = 0;
    uint32_t minimum = 0;
    unsigned int remaining = 0;

    if (stream == NULL) {
        fprintf(stderr, "bdf_from_ttc: cannot read %s: %s\n",
                path, strerror(errno));
        return 0;
    }
    while ((byte = fgetc(stream)) != EOF) {
        unsigned int current = (unsigned int)(unsigned char)byte;
        if (remaining == 0) {
            if (current < 0x80) {
                if (current >= 0x20 && !add_codepoint(set, current))
                    goto failed;
            } else if ((current & 0xE0) == 0xC0) {
                value = current & 0x1F;
                minimum = 0x80;
                remaining = 1;
            } else if ((current & 0xF0) == 0xE0) {
                value = current & 0x0F;
                minimum = 0x800;
                remaining = 2;
            } else if ((current & 0xF8) == 0xF0) {
                value = current & 0x07;
                minimum = 0x10000;
                remaining = 3;
            }
        } else if ((current & 0xC0) == 0x80) {
            value = (value << 6) | (current & 0x3F);
            if (--remaining == 0 && value >= minimum &&
                !add_codepoint(set, value))
                goto failed;
        } else {
            remaining = 0;
            value = 0;
        }
    }
    if (ferror(stream)) {
        fprintf(stderr, "bdf_from_ttc: cannot scan %s\n", path);
        goto failed;
    }
    fclose(stream);
    return 1;

failed:
    fclose(stream);
    return 0;
}

static int rounded_26_6(FT_Pos value)
{
    if (value >= 0)
        return (int)((value + 32) / 64);
    return -(int)((-value + 32) / 64);
}

static int glyph_advance(const FT_GlyphSlot slot)
{
    int advance = rounded_26_6(slot->advance.x);
    if (advance <= 0)
        advance = rounded_26_6(slot->metrics.horiAdvance);
    return advance > 0 ? advance : 1;
}

static int load_bitmap(const ft_api *api, FT_Face face, uint32_t codepoint)
{
    FT_Int flags = (FT_Int)(FT_LOAD_RENDER | FT_LOAD_MONOCHROME |
                            FT_LOAD_TARGET_MONO);
    FT_GlyphSlot slot;

    if (api->get_char_index(face, codepoint) == 0)
        return 0;
    if (api->load_char(face, codepoint, flags) != 0)
        return 0;
    slot = face->glyph;
    if (slot == NULL ||
        (slot->bitmap.pixel_mode != FT_PIXEL_MODE_MONO &&
         slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY))
        return 0;
    return 1;
}

static void include_metrics(font_metrics *metrics, const FT_GlyphSlot slot)
{
    int left = slot->bitmap_left;
    int bottom = slot->bitmap_top - (int)slot->bitmap.rows;
    int right = left + (int)slot->bitmap.width;
    int top = slot->bitmap_top;
    int advance = glyph_advance(slot);

    if (metrics->glyph_count == 0 || left < metrics->min_x)
        metrics->min_x = left;
    if (metrics->glyph_count == 0 || bottom < metrics->min_y)
        metrics->min_y = bottom;
    if (metrics->glyph_count == 0 || right > metrics->max_x)
        metrics->max_x = right;
    if (metrics->glyph_count == 0 || top > metrics->max_y)
        metrics->max_y = top;
    if (advance > metrics->max_advance)
        metrics->max_advance = advance;
    metrics->advance_sum += (unsigned int)advance;
    ++metrics->glyph_count;
}

static unsigned int bitmap_bit(const FT_Bitmap *bitmap,
                               unsigned int x, unsigned int y)
{
    int pitch = bitmap->pitch;
    const unsigned char *row;

    if (pitch >= 0)
        row = bitmap->buffer + (size_t)y * (size_t)pitch;
    else
        row = bitmap->buffer +
              (size_t)(bitmap->rows - 1U - y) * (size_t)(-pitch);
    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
        return (row[x >> 3] >> (7U - (x & 7U))) & 1U;
    return row[x] >= 96U;
}

static int write_bitmap_rows(FILE *output, const FT_Bitmap *bitmap)
{
    unsigned int y;
    unsigned int x;
    unsigned int byte;
    unsigned int bit_count;

    for (y = 0; y < bitmap->rows; ++y) {
        byte = 0;
        bit_count = 0;
        for (x = 0; x < bitmap->width; ++x) {
            byte = (byte << 1) | bitmap_bit(bitmap, x, y);
            if (++bit_count == 8) {
                if (fprintf(output, "%02X", byte) < 0)
                    return 0;
                byte = 0;
                bit_count = 0;
            }
        }
        if (bit_count != 0 &&
            fprintf(output, "%02X", byte << (8U - bit_count)) < 0)
            return 0;
        if (fputc('\n', output) == EOF)
            return 0;
    }
    return 1;
}

static int write_bdf(const ft_api *api, FT_Face face, const codepoints *set,
                     unsigned int pixel_size, const char *xlfd_weight,
                     const char *property_weight, const char *output_path)
{
    font_metrics metrics;
    FILE *output = NULL;
    size_t index;
    int average_width;
    int width;
    int height;
    int status = 0;

    memset(&metrics, 0, sizeof(metrics));
    for (index = 0; index < set->count; ++index) {
        if (load_bitmap(api, face, set->items[index]))
            include_metrics(&metrics, face->glyph);
    }
    if (metrics.glyph_count == 0) {
        fprintf(stderr, "bdf_from_ttc: no requested glyph can be rendered\n");
        return 0;
    }
    width = metrics.max_x - metrics.min_x;
    height = metrics.max_y - metrics.min_y;
    average_width = (int)((metrics.advance_sum * 10U +
                           metrics.glyph_count / 2U) /
                          metrics.glyph_count);

    output = fopen(output_path, "wx");
    if (output == NULL) {
        fprintf(stderr, "bdf_from_ttc: cannot create %s: %s\n",
                output_path, strerror(errno));
        return 0;
    }
    if (fprintf(output,
                "STARTFONT 2.1\n"
                "COMMENT Generated offline from Noto Sans CJK SC for MSYS\n"
                "FONT -msys-msyscjk-%s-r-normal--%u-%u-75-75-p-%d-iso10646-1\n"
                "SIZE %u 75 75\n"
                "FONTBOUNDINGBOX %d %d %d %d\n"
                "STARTPROPERTIES 17\n"
                "FOUNDRY \"MSYS\"\n"
                "FAMILY_NAME \"msyscjk\"\n"
                "WEIGHT_NAME \"%s\"\n"
                "SLANT \"R\"\n"
                "SETWIDTH_NAME \"Normal\"\n"
                "ADD_STYLE_NAME \"\"\n"
                "PIXEL_SIZE %u\n"
                "POINT_SIZE %u\n"
                "RESOLUTION_X 75\n"
                "RESOLUTION_Y 75\n"
                "SPACING \"P\"\n"
                "AVERAGE_WIDTH %d\n"
                "CHARSET_REGISTRY \"ISO10646\"\n"
                "CHARSET_ENCODING \"1\"\n"
                "FONT_ASCENT %d\n"
                "FONT_DESCENT %d\n"
                "DEFAULT_CHAR 9633\n"
                "ENDPROPERTIES\n"
                "CHARS %zu\n",
                xlfd_weight, pixel_size, pixel_size * 10U, average_width,
                pixel_size, width, height, metrics.min_x, metrics.min_y,
                property_weight,
                pixel_size, pixel_size * 10U, average_width,
                metrics.max_y, -metrics.min_y, metrics.glyph_count) < 0)
        goto done;

    for (index = 0; index < set->count; ++index) {
        FT_GlyphSlot slot;
        int advance;
        int scalable_width;
        int bottom;

        if (!load_bitmap(api, face, set->items[index]))
            continue;
        slot = face->glyph;
        advance = glyph_advance(slot);
        scalable_width = (advance * 1000 + (int)pixel_size / 2) /
                         (int)pixel_size;
        bottom = slot->bitmap_top - (int)slot->bitmap.rows;
        if (fprintf(output,
                    "STARTCHAR uni%04" PRIX32 "\n"
                    "ENCODING %" PRIu32 "\n"
                    "SWIDTH %d 0\n"
                    "DWIDTH %d 0\n"
                    "BBX %u %u %d %d\n"
                    "BITMAP\n",
                    set->items[index], set->items[index], scalable_width,
                    advance, slot->bitmap.width, slot->bitmap.rows,
                    slot->bitmap_left, bottom) < 0 ||
            !write_bitmap_rows(output, &slot->bitmap) ||
            fprintf(output, "ENDCHAR\n") < 0)
            goto done;
    }
    if (fprintf(output, "ENDFONT\n") < 0 || fflush(output) != 0)
        goto done;
    status = 1;

done:
    if (fclose(output) != 0)
        status = 0;
    if (!status)
        remove(output_path);
    else
        printf("bdf_from_ttc: glyphs=%zu pixels=%u output=%s\n",
               metrics.glyph_count, pixel_size, output_path);
    return status;
}

static int parse_unsigned(const char *text, unsigned long maximum,
                          unsigned long *result)
{
    char *end = NULL;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > maximum)
        return 0;
    *result = value;
    return 1;
}

int main(int argc, char **argv)
{
    ft_api api;
    FT_Library library = NULL;
    FT_Face face = NULL;
    codepoints set;
    unsigned long face_index;
    unsigned long pixel_size;
    int argument;
    int result = 1;
    uint32_t ascii;

    const char *xlfd_weight;
    const char *property_weight;

    if (argc < 6 || !parse_unsigned(argv[2], 4095, &face_index) ||
        !parse_unsigned(argv[3], 64, &pixel_size) || pixel_size < 8) {
        fprintf(stderr,
                "usage: %s FONT.ttc FACE_INDEX PIXEL_SIZE WEIGHT OUTPUT.bdf [UTF8_FILE ...]\n",
                argv[0]);
        return 2;
    }
    if (strcmp(argv[4], "medium") == 0 || strcmp(argv[4], "regular") == 0) {
        xlfd_weight = "medium";
        property_weight = "Medium";
    } else if (strcmp(argv[4], "bold") == 0) {
        xlfd_weight = "bold";
        property_weight = "Bold";
    } else {
        fprintf(stderr, "bdf_from_ttc: WEIGHT must be medium or bold\n");
        return 2;
    }
    memset(&set, 0, sizeof(set));
    set.seen = calloc((UINT32_C(0x110000) + 7U) / 8U, 1U);
    if (set.seen == NULL)
        goto done;
    for (ascii = 0x20; ascii <= 0x7E; ++ascii) {
        if (!add_codepoint(&set, ascii))
            goto done;
    }
    if (!add_codepoint(&set, 0x00A0) || !add_codepoint(&set, 0x2026) ||
        !add_codepoint(&set, 0x25A1) || !add_codepoint(&set, 0xFFFD) ||
        !add_gb2312(&set))
        goto done;
    for (argument = 6; argument < argc; ++argument) {
        if (!decode_utf8_file(&set, argv[argument]))
            goto done;
    }
    sort_unique(&set);

    if (!ft_api_open(&api))
        goto done;
    if (api.init(&library) != 0 ||
        api.new_face(library, argv[1], (FT_Long)face_index, &face) != 0) {
        fprintf(stderr, "bdf_from_ttc: cannot open requested font face\n");
        goto close_ft;
    }
    if (api.select_charmap(face, FT_ENCODING_UNICODE) != 0 ||
        api.set_pixel_sizes(face, 0, (FT_UInt)pixel_size) != 0) {
        fprintf(stderr, "bdf_from_ttc: cannot select Unicode %lupx face\n",
                pixel_size);
        goto close_ft;
    }
    if (!write_bdf(&api, face, &set, (unsigned int)pixel_size,
                   xlfd_weight, property_weight, argv[5]))
        goto close_ft;
    result = 0;

close_ft:
    if (face != NULL)
        api.done_face(face);
    if (library != NULL)
        api.done_library(library);
    ft_api_close(&api);
done:
    free(set.items);
    free(set.seen);
    return result;
}
