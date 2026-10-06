#pragma once

// TacUI — the single public C++ entry point.
//
//     #include <tacui/tacui.hpp>
//
// That is the whole include surface. Implementation headers live outside
// include/ and are not part of the API; nothing under core/, rhi/d3d12/,
// platform/ or tools/ should ever be included from application code.
//
// For host languages that are not C++, the equivalent entry point is the
// C ABI in <tacui/tacui.h>.

// ---- values ---------------------------------------------------------------
#include "tacui/geometry.hpp"       // Vec2, Rect, Color

// ---- the retained tree ----------------------------------------------------
#include "tacui/vnode.hpp"          // VNode, VNodeArena    — the description
#include "tacui/element.hpp"        // Element, NodeRef, OverrideToken — the state
#include "tacui/ui.hpp"             // Ui                   — the rebuild loop

// ---- text -----------------------------------------------------------------
#include "tacui/text_system.hpp"    // TextSystem, ShapedLine
#include "tacui/glyph_atlas.hpp"    // GlyphAtlas

// ---- rendering ------------------------------------------------------------
#include "tacui/rhi.hpp"            // Device, SdfRect, GlyphQuad
#include "tacui/rhi_factory.hpp"    // backend entry points

// ---- authoring ------------------------------------------------------------
#include "tacui/builder.hpp"        // Builder, Scope, Node — the C++ base layer

// ---- hosting --------------------------------------------------------------
// Optional: the core is embeddable and an application that already owns a
// window and a renderer drives ui.update() / ui.paint() itself.
#include "tacui/host.hpp"           // host::run — a ready-made Win32 + D3D12 shell
