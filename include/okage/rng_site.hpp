#pragma once

namespace okage {
enum class RngSite : unsigned char { Unlabelled, UiGlyph, UiIcon, UiSelect };
inline thread_local RngSite current_rng_site = RngSite::Unlabelled;
struct RngSiteScope {
    explicit RngSiteScope(RngSite site) { current_rng_site = site; }
    ~RngSiteScope() { current_rng_site = RngSite::Unlabelled; }
};
} // namespace okage
