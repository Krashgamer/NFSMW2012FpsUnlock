#pragma once

// The camera / view.
//
// Every frame the renderer copies two view descriptors out of the render
// manager into the camera object (sub_8D39F0 / sub_78F210 at 0x46F95B), and
// several passes read the manager's master block directly. Blending that
// master block - the first 1024 bytes, which hold both descriptors - is what
// puts the world, the camera, lights, smoke and debris on the same
// interpolated view as the cars. Blending the copies instead (or as well)
// leaves those passes on the tick-latched view, or blends twice.
namespace view
{
void Apply(float alpha);
void Restore();
} // namespace view
