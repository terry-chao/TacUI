#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "tacui/geometry.hpp"

namespace tac::ui {

// The immutable description half of architecture.md §2. A VNode tree is
// rebuilt from scratch whenever state changes, lives in an arena, and is
// discarded wholesale on the next rebuild — nothing here is retained.
//
// This is the tree a host language produces (over the C ABI in slice F).

enum class VType : uint16_t {
    Stack,   // grouping node; lays out nothing in M0 (real layout is M2)
    Rect,    // a rounded rectangle
    Text,    // a single line of text, drawn from its box's left/baseline
};

// Identity hint for reconcile. 0 means "no key" — match by position instead.
using Key = uint64_t;
constexpr Key kNoKey = 0;

struct VNode {
    VType type = VType::Rect;
    Key   key  = kNoKey;

    // Rect props. Base values only; direct-write overrides live on the
    // RenderObject, never here (architecture.md §14.3).
    Rect  bounds;
    float cornerRadius = 0.0f;
    Color color;

    // Text props. `text` is owned by the arena, so it stays valid until the
    // next rebuild. The baseline sits at bounds.y + ascent(fontSize).
    const char* text     = nullptr;
    float       fontSize = 0.0f;

    VNode*   parent     = nullptr;
    VNode**  children   = nullptr;
    uint32_t childCount = 0;

    // Arena bookkeeping; not part of the description.
    uint32_t childCapacity = 0;
};

// Bump allocator for VNodes. `reset()` is O(1) and keeps the underlying
// blocks, so a steady-state rebuild allocates nothing from the heap — which
// is the C++-specific cost architecture.md §13.3 calls out.
class VNodeArena {
public:
    void reset();

    VNode* make(VType type, Key key = kNoKey);
    void   addChild(VNode* parent, VNode* child);

    // Copies a NUL-terminated string into the arena, so a VNode never holds a
    // pointer into caller-owned memory.
    const char* strdup(const char* s);

    // Diagnostics.
    size_t bytesAllocated() const { return allocated_; }
    size_t blockCount() const { return blocks_.size(); }

private:
    void* alloc(size_t bytes, size_t align);

    static constexpr size_t kBlockSize = 256 * 1024;

    std::vector<std::unique_ptr<uint8_t[]>> blocks_;
    size_t offset_    = 0;
    size_t allocated_ = 0;
};

} // namespace tac::ui
