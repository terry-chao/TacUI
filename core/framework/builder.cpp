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
        node_        = other.node_;
        other.node_  = nullptr;
    }
    return *this;
}

Scope& Scope::width(float w)  { if (node_) node_->width  = w; return *this; }
Scope& Scope::height(float h) { if (node_) node_->height = h; return *this; }
Scope& Scope::flex(float f)   { if (node_) node_->flex   = f; return *this; }

Scope& Scope::margin(EdgeInsets m) { if (node_) node_->margin = m; return *this; }
Scope& Scope::margin(float all)    { return margin(EdgeInsets::all(all)); }

Scope& Scope::align(float x, float y) {
    if (node_) { node_->alignX = x; node_->alignY = y; }
    return *this;
}

Scope& Scope::padding(EdgeInsets p) {
    if (node_) node_->padding = p;
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

Node& Node::width(float w)  { if (node_) node_->width  = w; return *this; }
Node& Node::height(float h) { if (node_) node_->height = h; return *this; }
Node& Node::flex(float f)   { if (node_) node_->flex   = f; return *this; }

Node& Node::margin(EdgeInsets m) { if (node_) node_->margin = m; return *this; }
Node& Node::margin(float all)    { return margin(EdgeInsets::all(all)); }

Node& Node::align(float x, float y) {
    if (node_) { node_->alignX = x; node_->alignY = y; }
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
    return Scope(this, node);
}

Scope Builder::stack(Rect box, Key key) {
    VNode* node = arena_.make(VType::Stack, key);
    node->bounds = box;
    attach(node);
    open_.push_back(node);
    return Scope(this, node);
}

Scope Builder::row(Rect box, const StackProps& props, Key key) {
    VNode* node = arena_.make(VType::Stack, key);
    node->layout = StackLayout::Row;
    node->bounds = box;
    node->gap    = props.gap;
    node->padding = props.padding;
    attach(node);
    open_.push_back(node);
    return Scope(this, node);
}

Scope Builder::column(Rect box, const StackProps& props, Key key) {
    VNode* node = arena_.make(VType::Stack, key);
    node->layout = StackLayout::Column;
    node->bounds = box;
    node->gap    = props.gap;
    node->padding = props.padding;
    attach(node);
    open_.push_back(node);
    return Scope(this, node);
}

Scope Builder::clip(Rect box, Key key) {
    VNode* node = arena_.make(VType::Stack, key);
    node->clipChildren = true;
    node->clip         = box;
    node->bounds       = box;
    attach(node);
    open_.push_back(node);
    return Scope(this, node);
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
