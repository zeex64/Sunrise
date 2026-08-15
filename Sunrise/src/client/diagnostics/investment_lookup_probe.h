#pragma once

#include "../executable/image.h"

namespace sunrise::client::diagnostics {

/** Captures small code windows around item-registry vtable calls when explicitly requested. */
void capture_investment_lookup_candidates(
    const executable::ExecutableImage& image) noexcept;

} // namespace sunrise::client::diagnostics
