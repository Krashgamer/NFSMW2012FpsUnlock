#pragma once

// World-anchored readouts: speed camera and jump results, race position,
// the car-name popup.
//
// The in-world UI keeps its elements in a pool of 592-byte nodes (240 nodes
// at mgr+112). sub_791790 walks the pool once per tick and calls sub_7B53C0
// on every live node, which composes the node's placement: a matrix at
// +432..+495 with the world anchor at +480/+484/+488. The readout is drawn
// from that node every frame, so blending the node's per-tick fields smooths
// it. Nodes are found at that writer; 2D HUD nodes (screen anchored, z exactly
// 0) are left alone.
namespace readouts
{
bool InstallNodeHook();

// Registers the nodes the last tick updated and samples them (tick frames).
void Run(bool tickFrame);
void Apply(float alpha);
void Restore();
} // namespace readouts
