#pragma once

#include <memory>

#include "tacui/rhi.hpp"

namespace tac::rhi {

// Backend entry points. One per supported RHI; M0 only ships D3D12.
// Returns nullptr if the backend could not be initialised.

std::unique_ptr<Device> createD3D12Device(const SwapchainDesc& desc);

} // namespace tac::rhi
