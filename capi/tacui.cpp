// TacUI C ABI implementation. Bridges the host-language view in tacui.h onto
// the C++ framework. Everything C++-specific stops here.

#include "tacui/tacui.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tacui/host.hpp"
#include "tacui/ui.hpp"

using namespace tac;

namespace {

void warn(const char* fmt, ...) {
    std::fprintf(stderr, "[capi] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

// Index 0 is permanently reserved so a zeroed tui_handle is always null.
constexpr uint32_t kNullIndex = 0;

// Mirrors ui::Element::generation: bumping it on free makes every outstanding
// handle to that slot resolve to null instead of a different node.
struct HandleEntry {
    uint32_t generation = 1;
    bool     alive      = false;
    bool     isLease    = false;
    ui::NodeRef       node;
    ui::OverrideToken token;
};

// The host enum and the TUI_EVENT_* constants must not drift apart.
int32_t toCApi(host::EventType t) {
    switch (t) {
    case host::EventType::Resize:     return TUI_EVENT_RESIZE;
    case host::EventType::Close:      return TUI_EVENT_CLOSE;
    case host::EventType::MouseMove:  return TUI_EVENT_MOUSE_MOVE;
    case host::EventType::MouseLeave: return TUI_EVENT_MOUSE_LEAVE;
    case host::EventType::MouseDown:  return TUI_EVENT_MOUSE_DOWN;
    case host::EventType::MouseUp:    return TUI_EVENT_MOUSE_UP;
    case host::EventType::KeyDown:    return TUI_EVENT_KEY_DOWN;
    }
    return -1;
}

} // namespace

struct tui_ui {
    ui::Ui   ui;
    tui_host host{};

    // Build plane — live only inside the build callback.
    ui::VNodeArena*         arena = nullptr;
    std::vector<ui::VNode*> stack;
    ui::VNode*              buildRoot = nullptr;

    // Handle table.
    std::vector<HandleEntry> handles;
    std::vector<uint32_t>    freeList;

    // Run state.
    bool        exitRequested    = false;
    bool        captureRequested = false;
    std::string capturePathUtf8;

    tui_handle allocHandle(bool isLease);
    HandleEntry* lookup(tui_handle h);
    void         freeHandle(uint32_t index);
};

// ---------------------------------------------------------------------------
// Handle table
// ---------------------------------------------------------------------------

tui_handle tui_ui::allocHandle(bool isLease) {
    uint32_t index;
    if (!freeList.empty()) {
        index = freeList.back();
        freeList.pop_back();
    } else {
        handles.emplace_back();
        index = static_cast<uint32_t>(handles.size()) - 1;
    }

    HandleEntry& e = handles[index];
    e.alive   = true;
    e.isLease = isLease;
    return tui_handle{ index, e.generation };
}

HandleEntry* tui_ui::lookup(tui_handle h) {
    if (h.index == kNullIndex || h.index >= handles.size()) return nullptr;
    HandleEntry& e = handles[h.index];
    if (!e.alive || e.generation != h.generation) return nullptr;
    return &e;
}

void tui_ui::freeHandle(uint32_t index) {
    if (index == kNullIndex || index >= handles.size()) return;
    HandleEntry& e = handles[index];
    if (!e.alive) return;

    e.token = ui::OverrideToken{};   // releases the lease, if any
    e.node  = ui::NodeRef{};
    e.alive = false;
    e.isLease = false;
    ++e.generation;                  // invalidates every outstanding handle
    freeList.push_back(index);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

extern "C" {

uint32_t tui_abi_version(void) {
    return TUI_ABI_VERSION;
}

tui_handle tui_handle_null(void) {
    return tui_handle{ kNullIndex, 0 };
}

int32_t tui_handle_valid(tui_handle h) {
    return (h.index != kNullIndex && h.generation != 0) ? 1 : 0;
}

tui_ui* tui_ui_create(void) {
    auto* ui = new (std::nothrow) tui_ui();
    if (!ui) return nullptr;
    ui->handles.emplace_back();   // slot 0 stays null forever
    return ui;
}

void tui_ui_destroy(tui_ui* ui) {
    delete ui;
}

void tui_set_host(tui_ui* ui, const tui_host* host) {
    if (!ui) return;
    ui->host = host ? *host : tui_host{};

    ui->ui.init([ui](ui::BuildContext& ctx) -> ui::VNode* {
        ui::VNodeArena& arena = ctx.arena;
        ui->arena     = &arena;
        ui->stack.clear();
        ui->buildRoot = nullptr;

        if (ui->host.build) {
            ui->host.build(ui->host.user);
        }

        ui->arena = nullptr;
        if (!ui->stack.empty()) {
            warn("build callback left %zu nodes unclosed", ui->stack.size());
            ui->stack.clear();
        }
        return ui->buildRoot;
    });
}

// ---------------------------------------------------------------------------
// Build plane
// ---------------------------------------------------------------------------

void tui_push(tui_ui* ui, uint32_t kind, uint64_t key) {
    if (!ui || !ui->arena) {
        warn("tui_push called outside a build callback");
        return;
    }

    ui::VType type = ui::VType::Stack;
    if (kind == TUI_NODE_RECT)      type = ui::VType::Rect;
    else if (kind == TUI_NODE_TEXT) type = ui::VType::Text;
    ui::VNode* v = ui->arena->make(type, key);
    if (!v) return;

    if (ui->stack.empty()) {
        if (ui->buildRoot) {
            warn("build callback emitted more than one root node");
            return;
        }
        ui->buildRoot = v;
    } else {
        ui->arena->addChild(ui->stack.back(), v);
    }
    ui->stack.push_back(v);
}

void tui_pop(tui_ui* ui) {
    if (!ui || ui->stack.empty()) {
        warn("tui_pop without a matching tui_push");
        return;
    }
    ui->stack.pop_back();
}

void tui_rect(tui_ui* ui, float x, float y, float w, float h,
              float corner_radius, uint32_t rgba8) {
    if (!ui || ui->stack.empty()) {
        warn("tui_rect outside a pushed node");
        return;
    }
    ui::VNode* v = ui->stack.back();
    v->bounds       = Rect{ x, y, w, h };
    v->cornerRadius = corner_radius;
    v->color        = Color::rgba8(static_cast<uint8_t>(rgba8 & 0xFFu),
                                   static_cast<uint8_t>((rgba8 >> 8) & 0xFFu),
                                   static_cast<uint8_t>((rgba8 >> 16) & 0xFFu),
                                   static_cast<uint8_t>((rgba8 >> 24) & 0xFFu));
}

void tui_text(tui_ui* ui, float x, float y, float w, float h,
              float font_size, uint32_t rgba8, const char* utf8) {
    if (!ui || !ui->arena || ui->stack.empty()) {
        warn("tui_text outside a pushed node");
        return;
    }
    ui::VNode* v = ui->stack.back();
    v->bounds   = Rect{ x, y, w, h };
    v->fontSize = font_size;
    v->color    = Color::rgba8(static_cast<uint8_t>(rgba8 & 0xFFu),
                               static_cast<uint8_t>((rgba8 >> 8) & 0xFFu),
                               static_cast<uint8_t>((rgba8 >> 16) & 0xFFu),
                               static_cast<uint8_t>((rgba8 >> 24) & 0xFFu));
    v->text     = ui->arena->strdup(utf8 ? utf8 : "");
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

int64_t tui_state_get(const tui_ui* ui, uint32_t slot, int64_t fallback) {
    if (!ui) return fallback;
    return ui->ui.state(slot, fallback);
}

void tui_state_set(tui_ui* ui, uint32_t slot, int64_t value) {
    if (!ui) return;
    ui->ui.setState(slot, value);
}

void tui_invalidate(tui_ui* ui) {
    if (!ui) return;
    ui->ui.invalidate();
}

// ---------------------------------------------------------------------------
// Direct write
// ---------------------------------------------------------------------------

tui_handle tui_node_find(tui_ui* ui, uint64_t key) {
    if (!ui) return tui_handle_null();
    ui::NodeRef node = ui->ui.find(key);
    if (!node.valid()) return tui_handle_null();

    tui_handle h  = ui->allocHandle(false);
    HandleEntry* e = ui->lookup(h);
    if (!e) return tui_handle_null();
    e->node = node;
    return h;
}

tui_handle tui_override_begin(tui_ui* ui, uint64_t key) {
    if (!ui) return tui_handle_null();

    ui::NodeRef node = ui->ui.find(key);
    if (!node.valid()) return tui_handle_null();

    ui::OverrideToken token = node.beginOverride();
    if (!token.valid()) return tui_handle_null();   // already has one

    tui_handle h   = ui->allocHandle(true);
    HandleEntry* e = ui->lookup(h);
    if (!e) return tui_handle_null();
    e->node  = node;
    e->token = std::move(token);
    return h;
}

void tui_override_translate(tui_ui* ui, tui_handle lease, float x, float y) {
    if (!ui) return;
    HandleEntry* e = ui->lookup(lease);
    if (e && e->isLease) e->token.setTranslate(x, y);
}

void tui_override_opacity(tui_ui* ui, tui_handle lease, float opacity) {
    if (!ui) return;
    HandleEntry* e = ui->lookup(lease);
    if (e && e->isLease) e->token.setOpacity(opacity);
}

void tui_override_release(tui_ui* ui, tui_handle lease) {
    if (!ui) return;
    HandleEntry* e = ui->lookup(lease);
    if (e && e->isLease) ui->freeHandle(lease.index);
}

// ---------------------------------------------------------------------------
// Node queries
// ---------------------------------------------------------------------------

int32_t tui_node_contains(tui_ui* ui, uint64_t key, float x, float y) {
    if (!ui) return 0;
    ui::NodeRef node = ui->ui.find(key);
    if (!node.valid()) return 0;
    return node.bounds().contains(x, y) ? 1 : 0;
}

void tui_node_bounds(tui_ui* ui, uint64_t key, float* out_xywh) {
    if (!ui || !out_xywh) return;
    ui::NodeRef node = ui->ui.find(key);
    if (!node.valid()) {
        out_xywh[0] = out_xywh[1] = out_xywh[2] = out_xywh[3] = 0.0f;
        return;
    }
    const Rect b = node.bounds();
    out_xywh[0] = b.x;
    out_xywh[1] = b.y;
    out_xywh[2] = b.w;
    out_xywh[3] = b.h;
}

int32_t tui_node_has_override(tui_ui* ui, uint64_t key) {
    if (!ui) return 0;
    ui::NodeRef node = ui->ui.find(key);
    return (node.valid() && node.hasLiveOverride()) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

uint32_t tui_stats_size(void) {
    return static_cast<uint32_t>(sizeof(tui_stats));
}

void tui_get_stats(const tui_ui* ui, tui_stats* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    if (!ui) return;

    const ui::Stats& s = ui->ui.stats();
    out->rebuilds                      = s.rebuilds;
    out->elements_created              = s.elementsCreated;
    out->elements_destroyed            = s.elementsDestroyed;
    out->elements_updated              = s.elementsUpdated;
    out->paints                        = s.paints;
    out->reconciles_with_live_override = s.reconcilesWithLiveOverride;
    out->live_overrides                = s.liveOverrides;
    out->text_shaped                   = s.textShaped;
    out->glyphs_rasterized             = s.glyphsRasterized;
    out->glyph_cache_hits              = s.glyphCacheHits;
    out->glyph_quads                   = s.glyphQuads;
    out->atlas_uploads                 = s.atlasUploads;
}

namespace {

void dumpRec(const ui::Element& e, int depth, tui_dump_fn fn, void* user) {
    char buf[320];

    const char* kind = "Stack";
    if (e.type == ui::VType::Rect)      kind = "Rect";
    else if (e.type == ui::VType::Text) kind = "Text";

    int n = std::snprintf(buf, sizeof(buf), "%s key=%llu gen=%u",
                          kind,
                          static_cast<unsigned long long>(e.key),
                          e.generation);

    if (e.render) {
        const Rect b = e.render->bounds;
        n += std::snprintf(buf + n, sizeof(buf) - n,
                           " base=(%.0f,%.0f,%.0f,%.0f) r=%.0f",
                           b.x, b.y, b.w, b.h, e.render->cornerRadius);
        if (e.type == ui::VType::Text) {
            n += std::snprintf(buf + n, sizeof(buf) - n,
                               " size=%.0f glyphs=%zu text=\"%s\"",
                               e.render->fontSize,
                               e.render->placed.size(),
                               e.render->text.c_str());
        }
        if (e.render->overrideActive) {
            std::snprintf(buf + n, sizeof(buf) - n,
                          " override{op=%.2f t=(%.0f,%.0f)}",
                          e.render->overrideOpacity,
                          e.render->overrideTranslate.x,
                          e.render->overrideTranslate.y);
        }
    }

    fn(user, depth, buf);
    for (const auto& c : e.children) {
        dumpRec(*c, depth + 1, fn, user);
    }
}

} // namespace

void tui_dump_tree(tui_ui* ui, tui_dump_fn fn, void* user) {
    if (!ui || !fn) return;
    const ui::ElementPtr& root = ui->ui.root();
    if (root) dumpRec(*root, 0, fn, user);
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void tui_request_exit(tui_ui* ui) {
    if (ui) ui->exitRequested = true;
}

void tui_capture_next_frame(tui_ui* ui, const char* path_utf8) {
    if (!ui) return;
    const char* path = path_utf8;
    ui->capturePathUtf8 = (path && *path) ? path : "";
    ui->captureRequested = !ui->capturePathUtf8.empty();
}

int32_t tui_run(tui_ui* ui, const char* title_utf8, uint32_t width, uint32_t height) {
    if (!ui) return -1;

    host::Options opts;
    opts.title  = title_utf8 ? title_utf8 : "TacUI";
    opts.width  = width;
    opts.height = height;
    opts.clear  = Color::rgba8(24, 26, 32);

    // The C ABI's hooks map onto the host's: the host owns the window, the
    // device and the loop, and re-checks our flags right after the callback
    // returns so tui_request_exit / tui_capture_next_frame work from inside it.
    auto onFrame = [ui](host::Control& ctl, double elapsed) {
        if (ui->host.frame) {
            ui->host.frame(ui->host.user, elapsed);
        }
        if (ui->exitRequested) {
            ctl.requestExit();
        }
        if (ui->captureRequested) {
            ctl.captureNextFrame(ui->capturePathUtf8.c_str());
            ui->captureRequested = false;
        }
    };

    auto onEvent = [ui](const host::Event& ev) {
        if (!ui->host.event) return;
        const int32_t type = toCApi(ev.type);
        if (type < 0) return;
        ui->host.event(ui->host.user, type, ev.x, ev.y, ev.key);
    };

    const int rc = host::run(ui->ui, opts, onFrame, onEvent);
    if (rc != 0) {
        warn("host::run failed (%d)", rc);
        return -2;
    }
    return 0;
}

} // extern "C"
