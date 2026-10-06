#include "tacui/vnode.hpp"

#include <cassert>
#include <cstring>

namespace tac::ui {

void VNodeArena::reset() {
    // Blocks are kept; only the bump pointer rewinds.
    if (blocks_.size() > 1) {
        blocks_.resize(1);
    }
    offset_    = 0;
    allocated_ = 0;
}

void* VNodeArena::alloc(size_t bytes, size_t align) {
    if (bytes > kBlockSize) {
        assert(false && "VNodeArena allocation larger than a block");
        return nullptr;
    }
    if (blocks_.empty()) {
        blocks_.push_back(std::make_unique<uint8_t[]>(kBlockSize));
    }

    size_t aligned = (offset_ + align - 1) & ~(align - 1);
    if (aligned + bytes > kBlockSize) {
        blocks_.push_back(std::make_unique<uint8_t[]>(kBlockSize));
        offset_  = 0;
        aligned  = 0;
    }

    void* p = blocks_.back().get() + aligned;
    offset_    = aligned + bytes;
    allocated_ += bytes;
    return p;
}

VNode* VNodeArena::make(VType type, Key key) {
    auto* v = new (alloc(sizeof(VNode), alignof(VNode))) VNode();
    v->type = type;
    v->key  = key;
    return v;
}

const char* VNodeArena::strdup(const char* s) {
    if (!s) return nullptr;
    const size_t len = std::strlen(s) + 1;
    auto* copy = static_cast<char*>(alloc(len, 1));
    if (!copy) return nullptr;
    std::memcpy(copy, s, len);
    return copy;
}

void VNodeArena::addChild(VNode* parent, VNode* child) {
    assert(parent && child);
    if (child->parent) return;

    if (parent->childCount == parent->childCapacity) {
        const uint32_t newCapacity = parent->childCapacity ? parent->childCapacity * 2 : 4;
        auto** grown = static_cast<VNode**>(alloc(sizeof(VNode*) * newCapacity, alignof(VNode*)));
        if (!grown) return;
        if (parent->childCount) {
            std::memcpy(grown, parent->children, sizeof(VNode*) * parent->childCount);
        }
        parent->children      = grown;
        parent->childCapacity = newCapacity;
    }

    child->parent = parent;
    parent->children[parent->childCount++] = child;
}

} // namespace tac::ui
