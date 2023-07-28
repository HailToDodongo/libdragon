#include "rdpq_paragraph.h"
#include "rdpq_text.h"
#include "rdpq_font.h"
#include "rdpq_font_internal.h"
#include "debug.h"
#include <stdlib.h>
#include <assert.h>

#define UNLIKELY(x) __builtin_expect(!!(x), 0)

void __rdpq_paragraph_builder_newline(int ch_newline);

static struct {
    rdpq_paragraph_t *layout;
    const rdpq_textparms_t *parms;
    const rdpq_font_t *font;
    uint8_t font_id;
    uint8_t style_id;
    float xscale, yscale; // FIXME: this should be a drawing context
    float x, y;
    int ch_line_start;
    bool skip_current_line;
    bool must_sort;
} builder;

static uint32_t utf8_decode(const char **str)
{
    const uint8_t *s = (const uint8_t*)*str;
    uint32_t c = *s++;
    if (c < 0x80) {
        *str = (const char*)s;
        return c;
    }
    if (c < 0xC0) {
        *str = (const char*)s;
        return 0xFFFD;
    }
    if (c < 0xE0) {
        c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
        *str = (const char*)s;
        return c;
    }
    if (c < 0xF0) {
        c = ((c & 0x0F) << 12); c |= ((*s++ & 0x3F) << 6); c |= (*s++ & 0x3F);
        *str = (const char*)s;
        return c;
    }
    if (c < 0xF8) {
        c = ((c & 0x07) << 18); c |= ((*s++ & 0x3F) << 12); c |= ((*s++ & 0x3F) << 6); c |= (*s++ & 0x3F);
        *str = (const char*)s;
        return c;
    }
    *str = (const char*)s;
    return 0xFFFD;
}

void rdpq_paragraph_builder_begin(const rdpq_textparms_t *parms, uint8_t initial_font_id, rdpq_paragraph_t *layout)
{
    assertf(initial_font_id > 0, "invalid usage of font ID 0 (reserved)");

    static const rdpq_textparms_t empty_parms = {0};
    memset(&builder, 0, sizeof(builder));
    builder.parms = parms ? parms : &empty_parms;
    if (!layout) {
        const int initial_chars = 256;
        layout = malloc(sizeof(rdpq_paragraph_t) + sizeof(rdpq_paragraph_char_t) * initial_chars);
        memset(layout, 0, sizeof(*layout));
        layout->capacity = initial_chars;
    }
    builder.layout = layout;
    builder.font_id = initial_font_id;
    builder.font = rdpq_text_get_font(initial_font_id);
    assertf(builder.font, "font %d not registered", initial_font_id);
    builder.xscale = 1.0f;
    builder.yscale = 1.0f;
    // start at center of pixel so that all rounds are to nearest
    builder.x = 0.5f + builder.parms->indent;
    builder.y = 0.5f + builder.parms->height ? builder.font->ascent : 0;
    builder.skip_current_line = builder.parms->height && builder.y - builder.font->descent >= builder.parms->height;
    builder.layout->nlines = 1;
}

void rdpq_paragraph_builder_font(uint8_t font_id)
{
    builder.must_sort |= builder.font_id > font_id;
    builder.font_id = font_id;
    builder.font = rdpq_text_get_font(font_id);
    builder.style_id = 0;
}

void rdpq_paragraph_builder_style(uint8_t style_id)
{
    builder.must_sort |= builder.style_id > style_id;
    builder.style_id = style_id;
}

static bool paragraph_wrap(int wrapchar, float *xcur, float *ycur)
{
    // Force a newline at wrapchar. If the newline doesn't fit vertically,
    // there's nothing more to do and we can return false.
    builder.x = *xcur;
    builder.y = *ycur;
    __rdpq_paragraph_builder_newline(wrapchar);
    if (builder.skip_current_line) {
        builder.layout->nchars = wrapchar;
        return false;
    }

    rdpq_paragraph_char_t *ch = &builder.layout->chars[wrapchar];
    rdpq_paragraph_char_t *end = &builder.layout->chars[builder.layout->nchars];

    // Calculate wrap translation
    float offx = builder.x - ch[0].x * 0.25f;
    float offy = builder.y - ch[0].y * 0.25f;

    // Translate all the characters between wrapchar and the end
    while (ch < end) {
        ch->x += offx * 4;
        ch->y += offy * 4;
        ++ch;
    }

    // Translate also the endpoint, so that it now points at the end of the
    // translated characters
    *xcur = *xcur + offx;
    *ycur = *ycur + offy;
    return true;
}

void rdpq_paragraph_builder_span(const char *utf8_text, int nbytes)
{
    // We're skipping the current line, so this span isn't useful
    if (builder.skip_current_line) return;

    const rdpq_font_t *fnt = builder.font;
    const rdpq_textparms_t *parms = builder.parms;
    const char *end = utf8_text + nbytes;
    float xcur = builder.x;
    float ycur = builder.y;
    int16_t next_index = -1;
    int last_space_index = -1;
    bool is_space = false;

    #define UTF8_DECODE_NEXT() ({ \
        uint32_t codepoint = *utf8_text > 0 ? *utf8_text++ : utf8_decode(&utf8_text); \
        is_space = (codepoint == ' '); \
        __rdpq_font_glyph(builder.font, codepoint); \
    })

    while (utf8_text < end || next_index >= 0) {
        int16_t index = next_index; next_index = -1;
        if (index < 0) index = UTF8_DECODE_NEXT();
        if (UNLIKELY(index < 0)) continue;

        float xadvance; int8_t xoff2; bool has_kerning; uint8_t sort_key;
        __rdpq_font_glyph_metrics(fnt, index, &xadvance, NULL, &xoff2, &has_kerning, &sort_key);

        if (!is_space) {
            builder.layout->chars[builder.layout->nchars++] = (rdpq_paragraph_char_t) {
                .font_id = builder.font_id,
                .style_id = builder.style_id,
                .sort_key = sort_key,
                .glyph = index,
                .x = xcur*4,
                .y = ycur*4,
            };
        } else {       
            last_space_index = builder.layout->nchars;
        }

        float last_pixel = xcur + xoff2 * builder.xscale;

        xcur += xadvance * builder.xscale;

        if (UNLIKELY(has_kerning && utf8_text < end)) {
            next_index = UTF8_DECODE_NEXT();
            if (next_index >= 0) {
                float kerning = __rdpq_font_kerning(fnt, index, next_index);
                xcur += kerning * builder.xscale;
            }
        }

        // Check if we are limited in width
        if (UNLIKELY(parms->width) && UNLIKELY(last_pixel > parms->width)) {
            // Check if we are allowed to wrap
            switch (parms->wrap) {
                case WRAP_CHAR:
                    if (!paragraph_wrap(builder.layout->nchars-1, &xcur, &ycur))
                        return;
                    break;
                case WRAP_WORD:
                    // Find the last space in the line
                    if (last_space_index >= 0) {
                        if (!paragraph_wrap(last_space_index, &xcur, &ycur))
                            return;
                        last_space_index = -1;
                        break;
                    }
                    builder.layout->nchars -= 1;
                    // fallthrough!
                case WRAP_ELLIPSES:
                case WRAP_NONE:
                    // The text doesn't fit on this line anymore.
                    // Skip the rest of the line, including the rest of this span,
                    // including the current character.
                    builder.skip_current_line = true;
                    return;
            }
        }
    }

    builder.x = xcur;
    builder.y = ycur;
}

void __rdpq_paragraph_builder_newline(int ch_newline)
{
    float line_height = 0.0f;
    
    line_height += builder.font->ascent - builder.font->descent + builder.font->line_gap;
    line_height += builder.parms->line_spacing;
     
    builder.y += line_height * builder.yscale;
    builder.x = 0.5f;
    builder.skip_current_line = builder.parms->height && builder.y - builder.font->descent >= builder.parms->height;
    builder.layout->nlines += 1;

    int ix0 = builder.ch_line_start;
    int ix1 = ch_newline;

    // If there's at least one character on this line
    if (ix0 != ix1) {
        rdpq_paragraph_char_t* ch0 = &builder.layout->chars[ix0];
        rdpq_paragraph_char_t* ch1 = &builder.layout->chars[ix1-1];

        const rdpq_font_t *fnt0 = rdpq_text_get_font(ch0->font_id); assert(fnt0);
        const rdpq_font_t *fnt1 = rdpq_text_get_font(ch1->font_id); assert(fnt1);

        // Extract X of first pixel of first char, and last pixel of last char
        // This is a slightly more accurate centering than just using the glyph position
        int8_t off_x0, off_x1;
        __rdpq_font_glyph_metrics(fnt0, ch0->glyph, NULL, &off_x0, NULL,    NULL, NULL);
        __rdpq_font_glyph_metrics(fnt1, ch1->glyph, NULL, NULL,    &off_x1, NULL, NULL);

        // Compute absolute x0/x1 in the paragraph
        float x0 = ch0->x * 0.25f + off_x0 * builder.xscale;
        float x1 = ch1->x * 0.25f + off_x1 * builder.xscale;

        // Do right/center alignment of the row (and adjust extents)
        if (UNLIKELY(builder.parms->width && builder.parms->align)) {
            float offset = builder.parms->width - (x1 - x0);
            if (builder.parms->align == ALIGN_CENTER) offset *= 0.5f;

            int16_t offset_fx = offset * 4;
            for (rdpq_paragraph_char_t *ch = ch0; ch <= ch1; ++ch)
                ch->x += offset_fx;
            x0 += offset;
            x1 += offset;
        }

        // Update bounding box
        if (builder.layout->bbox[0] > x0) builder.layout->bbox[0] = x0;
        if (builder.layout->bbox[2] < x1) builder.layout->bbox[2] = x1;
    }

    builder.ch_line_start = ch_newline;
}

void rdpq_paragraph_builder_newline()
{
    __rdpq_paragraph_builder_newline(builder.layout->nchars);
}


static int char_compare(const void *a, const void *b)
{
    const rdpq_paragraph_char_t *ca = a, *cb = b;
    return (ca->fsg & 0xFFFFFF00) - (cb->fsg & 0xFFFFFF00);
}

void insertion_sort_char_array(rdpq_paragraph_char_t *chars, int nchars)
{
    for (int i = 1; i < nchars; ++i) {
        rdpq_paragraph_char_t tmp = chars[i];
        int j = i;
        while (j > 0 && char_compare(chars + j - 1, &tmp) > 0) {
            chars[j] = chars[j - 1];
            --j;
        }
        chars[j] = tmp;
    }
}

void __rdpq_paragraph_builder_optimize(void)
{
    builder.must_sort = true;
}

rdpq_paragraph_t* rdpq_paragraph_builder_end(void)
{
    // Update bounding box (vertically)
    float y0 = builder.layout->chars[0].y * 0.25f - builder.font->ascent;
    float y1 = builder.layout->chars[builder.layout->nchars-1].y * 0.25f - builder.font->descent + builder.font->line_gap + 1;

    if (UNLIKELY(builder.parms->height && builder.parms->valign)) {
        float offset = builder.parms->height - (y1 - y0);
        if (builder.parms->valign == VALIGN_CENTER) offset *= 0.5f;

        builder.layout->y0 = offset;
        y0 += offset;
        y1 += offset;
    }

    builder.layout->bbox[1] = y0;
    builder.layout->bbox[3] = y1;

    // Sort the chars by font/style/glyph
    if (builder.must_sort || 1) {
        if (builder.layout->nchars < 48) {
            insertion_sort_char_array(builder.layout->chars, builder.layout->nchars);
        } else {
            qsort(builder.layout->chars, builder.layout->nchars, sizeof(rdpq_paragraph_char_t),
                char_compare);
        }
    }

    // Make sure there is always a terminator.
    assertf(builder.layout->nchars < builder.layout->capacity,
        "paragraph too long (%d/%d chars)", builder.layout->nchars, builder.layout->capacity);
    builder.layout->chars[builder.layout->nchars].fsg = 0;

    return builder.layout;
}

static uint8_t must_hex_digit(uint8_t ch, bool *error)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    *error = true;
    return 0;
}

rdpq_paragraph_t* __rdpq_paragraph_build(const rdpq_textparms_t *parms, uint8_t initial_font_id, const char *utf8_text, int nbytes, rdpq_paragraph_t *layout, bool optimize)
{
    rdpq_paragraph_builder_begin(parms, initial_font_id, layout);

    const char *buf = utf8_text;
    const char *end = utf8_text + nbytes;
    const char *span = buf;

    while (buf < end) {
        if (UNLIKELY(buf[0] == '$')) {
            rdpq_paragraph_builder_span(span, buf - span);
            if (buf[1] == '$') {
                // next span will include the escaped char
                buf += 2; span = buf - 1;                
            } else {
                bool error = false;
                uint8_t font_id = must_hex_digit(buf[1], &error) << 4 | must_hex_digit(buf[2], &error);
                assertf(!error, "invalid font id: %c%c at position %d", buf[1], buf[2], buf-utf8_text);
                rdpq_paragraph_builder_font(font_id);
                span = buf + 3;
                buf = span;
            }
            continue;
        } else if (UNLIKELY(buf[0] == '^')) {
            rdpq_paragraph_builder_span(span, buf - span);
            if (buf[1] == '^') {
                // next span will include the escaped char
                buf += 2; span = buf - 1;                
            } else {
                bool error = false;
                rdpq_paragraph_builder_span(span, buf - span);
                uint8_t style_id = must_hex_digit(buf[1], &error) << 4 | must_hex_digit(buf[2], &error);
                assertf(!error, "invalid style id: %c%c at position %d", buf[1], buf[2], buf-utf8_text);
                rdpq_paragraph_builder_style(style_id);
                span = buf + 3;
                buf = span;
            }
            continue;
        } else if (UNLIKELY(buf[0] == '\n')) {
            rdpq_paragraph_builder_span(span, buf - span);
            rdpq_paragraph_builder_newline();
            span = buf + 1;
            buf = span;
            continue;
        } else {
            ++buf;
        }
    }

    if (buf != span)
        rdpq_paragraph_builder_span(span, buf - span);
    if (optimize)
        __rdpq_paragraph_builder_optimize();
    return rdpq_paragraph_builder_end();
}

rdpq_paragraph_t* rdpq_paragraph_build(const rdpq_textparms_t *parms, uint8_t initial_font_id, const char *utf8_text, int nbytes)
{
    return __rdpq_paragraph_build(parms, initial_font_id, utf8_text, nbytes, NULL, true);
}

void rdpq_paragraph_render(const rdpq_paragraph_t *layout, float x0, float y0)
{
    const rdpq_paragraph_char_t *ch = layout->chars;

    x0 += layout->x0;
    y0 += layout->y0;
    while (ch->font_id != 0) {
        const rdpq_font_t *fnt = rdpq_text_get_font(ch->font_id);
        int n = rdpq_font_render_paragraph(fnt, ch, x0, y0);
        ch += n;
        assert(ch <= layout->chars + layout->nchars);
    }
}

void rdpq_paragraph_free(rdpq_paragraph_t *layout)
{
    #ifndef NDEBUG
    memset(layout, 0, sizeof(*layout));
    #endif
    free(layout);
}