#include "tacui/builder.hpp"

#include <cstdio>

#include "tacui/ui.hpp"

namespace tac::ui {

Builder::Builder(BuildContext& ctx) : arena_(ctx.arena), ui_(&ctx.ui) {}

Ui& Builder::ui() const {
    return *ui_;
}

void Builder::behavior(Key key, NodeBehavior b) {
    if (ui_) ui_->registerBehavior(key, std::move(b));
}

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

Scope::~Scope() {
    if (owner_) owner_->pop();
}

Scope::Scope(Scope&& other) noexcept : owner_(other.owner_) {
    other.owner_ = nullptr;
}

Scope& Scope::operator=(Scope&& other) noexcept {
    if (this != &other) {
        if (owner_) owner_->pop();
        owner_       = other.owner_;
        other.owner_ = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

Node& Node::box(float x, float y, float w, float h) {
    if (node_) node_->bounds = Rect{ x, y, w, h };
    return *this;
}

Node& Node::box(Rect r) {
    if (node_) node_->bounds = r;
    return *this;
}

Node& Node::radius(float r) {
    if (node_) node_->cornerRadius = r;
    return *this;
}

Node& Node::color(Color c) {
    if (node_) node_->color = c;
    return *this;
}

Node& Node::key(Key k) {
    if (node_) node_->key = k;
    return *this;
}

Node& Node::size(float fontSize) {
    if (node_) node_->fontSize = fontSize;
    return *this;
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

void Builder::attach(VNode* node) {
    if (!node) return;

    if (open_.empty()) {
        if (root_) {
            std::fprintf(stderr, "[ui] Builder: more than one root node emitted\n");
            return;
        }
        root_ = node;
        return;
    }
    arena_.addChild(open_.back(), node);
}

void Builder::pop() {
    if (!open_.empty()) open_.pop_back();
}

Scope Builder::stack(Key key) {
    VNode* node = arena_.make(VType::Stack, key);
    attach(node);
    open_.push_back(node);
    return Scope(this);
}

Scope Builder::clip(Rect box, Key key) {
    VNode* node = arena_.make(VType::Stack, key);
    node->clipChildren = true;
    node->clip         = box;
    attach(node);
    open_.push_back(node);
    return Scope(this);
}

Node Builder::rect() {
    VNode* node = arena_.make(VType::Rect);
    attach(node);
    return Node(node);
}

Node Builder::text(const char* utf8) {
    VNode* node = arena_.make(VType::Text);
    attach(node);
    node->text = arena_.strdup(utf8 ? utf8 : "");
    return Node(node);
}

} // namespace tac::ui
