// The renderer's text: .ttf fonts, the glyph atlas they are drawn into, the built-in 8x8 font, and
// gfx::text / text_width.
#define STB_TRUETYPE_IMPLEMENTATION
#include "gfx_internal.h"

#include "font8x8_basic.h"

namespace gfx {
namespace internal {

uint32_t next_codepoint(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

// Finds a letter in the font's atlas, drawing it in the first time it is needed.
const Glyph* glyph_for(Font& f, uint32_t cp) {
    auto found = f.glyphs.find(cp);
    if (found != f.glyphs.end()) return &found->second;
    Glyph g = {};
    int advance = 0, bearing = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointHMetrics(&f.info, (int)cp, &advance, &bearing);
    stbtt_GetCodepointBitmapBox(&f.info, (int)cp, f.scale, f.scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    g.advance = advance * f.scale;
    if (w > 0 && h > 0 && !f.full) {
        if (f.penX + w + 1 > ATLAS_SIZE) {
            f.penX = 1;
            f.penY += f.rowH + 1;
            f.rowH = 0;
        }
        if (f.penY + h + 1 > ATLAS_SIZE) {
            f.full = true;
            SDL_Log("gfx: the font atlas is full, so some letters will be missing");
        } else {
            stbtt_MakeCodepointBitmap(&f.info, f.alpha.data() + (size_t)f.penY * ATLAS_SIZE + f.penX, w, h, ATLAS_SIZE, f.scale, f.scale, (int)cp);
            g.u0 = (float)f.penX / ATLAS_SIZE; g.v0 = (float)f.penY / ATLAS_SIZE;
            g.u1 = (float)(f.penX + w) / ATLAS_SIZE; g.v1 = (float)(f.penY + h) / ATLAS_SIZE;
            g.x0 = (float)x0; g.y0 = (float)y0; g.w = (float)w; g.h = (float)h;
            f.penX += w + 1;
            f.rowH = SDL_max(f.rowH, h);
            f.dirty = true;
        }
    }
    return &f.glyphs.emplace(cp, g).first->second;
}

void upload_fonts() {
    fonts.each([](Handle, Font& f) {
        Texture* atlas = textures.get(f.atlas);
        if (!f.dirty || !atlas) return;
        std::vector<uint8_t> rgba((size_t)ATLAS_SIZE * ATLAS_SIZE * 4, 255);
        for (size_t i = 0; i < f.alpha.size(); i++) rgba[i * 4 + 3] = f.alpha[i];
        upload_pixels(*atlas, rgba.data());
        f.dirty = false;
    });
}

void free_font(Handle h) {
    Font* f = fonts.get(h);
    if (!f) return;
    free_texture(f->atlas);
    fonts.remove(h);
}

}  // namespace internal

using namespace internal;

static Handle open_font(const std::string& path, float pixelHeight, bool engine);

Handle load_font(const std::string& name, float pixelHeight) {
    return open_font(game_path(name), pixelHeight, false);
}

Handle load_engine_font(const std::string& path, float pixelHeight) {
    return open_font(path, pixelHeight, true);
}

static Handle open_font(const std::string& path, float pixelHeight, bool engine) {
    pixelHeight = SDL_clamp(pixelHeight, 8.0f, 200.0f);
    std::string key = path;
    Handle found = 0;
    fonts.each([&](Handle h, Font& f) {
        if (f.key == key && f.px == pixelHeight) found = h;
    });
    if (found) return found;
    size_t size = 0;
    void* data = path.empty() ? nullptr : SDL_LoadFile(path.c_str(), &size);
    if (!data) return 0;
    Handle h = fonts.add(Font());
    Font* f = fonts.get(h);
    f->file.assign((unsigned char*)data, (unsigned char*)data + size);
    SDL_free(data);
    if (!stbtt_InitFont(&f->info, f->file.data(), stbtt_GetFontOffsetForIndex(f->file.data(), 0))) {
        fonts.remove(h);
        return 0;
    }
    f->px = pixelHeight;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, pixelHeight);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
    f->ascent = ascent * f->scale;
    f->alpha.assign((size_t)ATLAS_SIZE * ATLAS_SIZE, 0);
    std::vector<uint8_t> blank((size_t)ATLAS_SIZE * ATLAS_SIZE * 4, 0);
    f->atlas = make_texture(ATLAS_SIZE, ATLAS_SIZE, blank.data(), TEX_NOMIP | TEX_CLAMP | TEX_DATA, engine ? OWNER_ENGINE : OWNER_FILE);
    f->engine = engine;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (engine && stbtt_GetCodepointBox(&f->info, 'H', &x0, &y0, &x1, &y1) && y1 > y0)
        f->boost = 0.875f * pixelHeight / ((y1 - y0) * f->scale), f->lift = f->ascent - y1 * f->scale;
    f->key = key;
    f->mtime = file_time(path);
    return h;
}

// Text is drawn with the font chosen by gfx.font, or with a built-in 8x8 pixel font that needs no file.
void text(const std::string& s, float x, float y, float size, const float rgba[4]) {
    // Logged as one box for the whole text, not one per letter.
    bool logging = uiLogging;
    uiLogging = false;
    if (logging && s.find_first_not_of(" \n") != std::string::npos) {
        float lines = 1;
        for (char ch : s) lines += ch == '\n';
        uiBoxes.push_back({x, y, text_width(s, size), size * (1 + (lines - 1) * 1.25f), false, s});
    }
    float color[4], penX = x;
    srgb_to_linear4(rgba, color);
    Font* f = fonts.get(ds.font);
    if (f) {
        float k = size / f->px * f->boost;
        uint32_t previous = 0;
        for (size_t i = 0; i < s.size();) {
            uint32_t cp = next_codepoint(s, i);
            if (cp == '\n') { penX = x; y += size * 1.25f; previous = 0; continue; }
            const Glyph* g = glyph_for(*f, cp);
            if (previous) penX += stbtt_GetCodepointKernAdvance(&f->info, (int)previous, (int)cp) * f->scale * k;
            if (g->w > 0) quad_2d(f->atlas, penX + g->x0 * k, y + (f->ascent - f->lift + g->y0) * k, g->w * k, g->h * k, color, g->u0, g->v0, g->u1, g->v1);
            penX += g->advance * k;
            previous = cp;
        }
    } else {
        float cell = size / 8.0f;
        for (size_t i = 0; i < s.size();) {
            uint32_t ch = next_codepoint(s, i);
            if (ch == '\n') { penX = x; y += size * 1.25f; continue; }
            if (ch >= 128) ch = '?';
            for (int row = 0; row < 8; row++) {
                unsigned char bits = (unsigned char)font8x8_basic[ch][row];
                int col = 0;
                while (col < 8) {
                    if (!(bits & (1 << col))) { col++; continue; }
                    int start = col;
                    while (col < 8 && (bits & (1 << col))) col++;
                    quad_2d(whiteTex, penX + start * cell, y + row * cell, (col - start) * cell, cell, color, 0, 0, 1, 1);
                }
            }
            penX += size;
        }
    }
    uiLogging = logging;
}

float text_width(const std::string& s, float size) {
    float widest = 0, current = 0;
    Font* f = fonts.get(ds.font);
    uint32_t previous = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next_codepoint(s, i);
        if (cp == '\n') { current = 0; previous = 0; continue; }
        if (f) {
            if (previous) current += stbtt_GetCodepointKernAdvance(&f->info, (int)previous, (int)cp) * f->scale * size / f->px * f->boost;
            current += glyph_for(*f, cp)->advance * size / f->px * f->boost;
        } else {
            current += size;
        }
        previous = cp;
        if (current > widest) widest = current;
    }
    return widest;
}

}  // namespace gfx
