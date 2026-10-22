#pragma once

// The taut path through a corridor of navmesh triangles (the "simple stupid
// funnel" of Recast/Detour): the shortest line from `start` to `end` that
// stays inside the portals - the edges each triangle shares with the next.
// Waypoints come out only where the path bends, at a portal's end, instead of
// one per triangle at each edge's middle (which walked a zigzag across every
// open floor). Header-only and free of the game, so it can be tested alone.

#include <vector>

namespace f3a::navmesh {

struct FunnelPoint { float x, y, z; };
// One portal, as seen walking through it: `left` and `right` of the way on.
struct FunnelPortal { FunnelPoint left, right; };

namespace funnel_detail {
// > 0 when c lies left of the line a -> b (counter-clockwise), < 0 right.
inline float Cross(const FunnelPoint& a, const FunnelPoint& b, const FunnelPoint& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
inline bool Same(const FunnelPoint& a, const FunnelPoint& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy < 1e-4f;
}
} // namespace funnel_detail

// `portals` are the inner edges in walking order (start and end are added
// here). Returns the bends and the end - not the start.
inline std::vector<FunnelPoint> PullString(const FunnelPoint& start, const FunnelPoint& end,
                                           const std::vector<FunnelPortal>& inner)
{
    using funnel_detail::Cross;
    using funnel_detail::Same;
    std::vector<FunnelPortal> portals;
    portals.reserve(inner.size() + 2);
    portals.push_back({ start, start });
    portals.insert(portals.end(), inner.begin(), inner.end());
    portals.push_back({ end, end });

    std::vector<FunnelPoint> out;
    // A bend at the point before it is none: the apex moves on along a fan of
    // triangles round one vertex without moving.
    auto bend = [&](const FunnelPoint& p) {
        if (!Same(out.empty() ? start : out.back(), p)) out.push_back(p);
    };
    FunnelPoint apex = start, left = start, right = start;
    size_t apexI = 0, leftI = 0, rightI = 0;
    for (size_t i = 1; i < portals.size(); ++i) {
        const FunnelPoint& pl = portals[i].left;
        const FunnelPoint& pr = portals[i].right;
        // The right side: tighten if the new point is no further out.
        if (Cross(apex, right, pr) >= 0.0f) {
            if (Same(apex, right) || Cross(apex, left, pr) < 0.0f) {
                right = pr; rightI = i;
            } else {
                // Crossed over the left side: the left point is a bend.
                bend(left);
                apex = left; apexI = leftI;
                left = right = apex; leftI = rightI = apexI;
                i = apexI;
                continue;
            }
        }
        // The left side, the same way round.
        if (Cross(apex, left, pl) <= 0.0f) {
            if (Same(apex, left) || Cross(apex, right, pl) > 0.0f) {
                left = pl; leftI = i;
            } else {
                bend(right);
                apex = right; apexI = rightI;
                left = right = apex; leftI = rightI = apexI;
                i = apexI;
                continue;
            }
        }
    }
    if (out.empty() || !Same(out.back(), end)) out.push_back(end);
    return out;
}

} // namespace f3a::navmesh
