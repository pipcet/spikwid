# swgl

Software OpenGL implementation for WebRender

## Overview
This is a relatively simple single threaded software rasterizer designed
for use by WebRender. It will shade one quad at a time using a 4xf32 vector
with one vertex per lane. It rasterizes quads usings spans and shades that
span 4 pixels at a time.

## Building
clang-cl is required to build on Windows. This can be done by installing
the llvm binaries from https://releases.llvm.org/ and adding the installation
to the path with something like `set PATH=%PATH%;C:\Program Files\LLVM\bin`.
Then `set CC=clang-cl` and `set CXX=clang-cl`. That should be sufficient
for `cc-rs` to use `clang-cl` instead of `cl`.

## Extensions
SWGL contains a number of OpenGL and GLSL extensions designed to both ease
integration with WebRender and to help accelerate span rasterization.

GLSL extension intrinsics are generally prefixed with `swgl_` to distinguish
them from other items in the GLSL namespace.

Inside GLSL, the `SWGL` preprocessor token is defined so that usage of SWGL
extensions may be conditionally compiled.

```
void swgl_clipMask(sampler2D mask, vec2 offset, vec2 bb_origin, vec2 bb_size);
```

When called from the the vertex shader, this specifies a clip mask texture to
be used to mask the currently drawn primitive while blending is enabled. This
mask will only apply to the current primitive.

The mask must be an R8 texture that will be interpreted as alpha weighting
applied to the source pixel prior to the blend stage. It is sampled 1:1 with
nearest filtering without any applied transform. The given offset specifies
the positioning of the clip mask relative to the framebuffer's viewport.

The supplied bounding box constrains sampling of the clip mask to only fall
within the given rectangle, specified relative to the clip mask offset.
Anything falling outside this rectangle will be clipped entirely. If the
rectangle is empty, then the clip mask will be ignored.

```
void swgl_antiAlias(int edgeMask);
```

When called from the vertex shader, this enables anti-aliasing for the
currently drawn primitive while blending is enabled. This setting will only
apply to the current primitive. Anti-aliasing will be applied only to the
edges corresponding to bits supplied in the mask. For simple use-cases,
the edge mask can be set to all 1 bits to enable AA for the entire quad.

The order of the bits in the edge mask must match the winding order in which
the vertices are output in the vertex shader if processed as a quad, so that
the edge ends on that vertex. The easiest way to understand this ordering
is that for a rectangle (x0,y0,x1,y1) then the edge Nth edge bit corresponds
to the edge where Nth coordinate in the rectangle is constant.

```
swgl_commitTextureLinearRGBA8(sampler, vec2 uv, vec4 uv_bounds, float layer);
swgl_commitTextureLinearR8(sampler, vec2 uv, vec4 uv_bounds, float layer);

swgl_commitTextureLinearColorRGBA8(sampler, vec2 uv, vec4 uv_bounds, vec4|float color, float layer);
swgl_commitTextureLinearColorR8(sampler, vec2 uv, vec4 uv_bounds, vec4|float color, float layer);

swgl_commitTextureLinearChunkRGBA8(sampler, vec2 uv, int layerOffset);
swgl_commitTextureLinearChunkColorRGBA8(sampler, vec2 uv, vec4 color, int layerOffset);
```

Samples and commits an entire span of texture starting at the given uv and
within the supplied uv bounds from the given layer. The color variations
also accept a supplied color that modulates the result.

The RGBA8 versions may only be used to commit within swgl_drawSpanRGBA8, and
the R8 versions may only be used to commit within swgl_drawSpanR8.

The chunk variations only commit a single chunk rather than an entire span. The
uv coordinates must be clamped beforehand and then scaled appropriately with
swgl_linearQuantize. The layer offset must be resolved to a linear offset with
with swgl_textureLayerOffset.

SWGL tries to use an anti-aliasing method that is reasonably close to WR's
signed-distance field approximation. WR would normally try to discern the
2D local-space coordinates of a given destination pixel relative to the
2D local-space bounding rectangle of a primitive. It then uses the screen-
space derivative to try to determine the how many local-space units equate
to a distance of around one screen-space pixel. A distance approximation
of coverage is then used based on the distance in local-space from the
the current pixel's center, roughly at half-intensity at pixel center
and ranging to zero or full intensity within a radius of half a pixel
away from the center. To account for AAing going outside the normal geometry
boundaries of the primitive, WR has to extrude the primitive by a local-space
estimate to allow some AA to happen within the extruded region.

SWGL can ultimately do this approximation more simply and get around the
extrusion limitations by just ensuring spans encompass any pixel that is
partially covered when computing span boundaries. Further, since SWGL already
knows the slope of an edge and the coordinate of the span relative to the span
boundaries, finding the partial coverage of a given span becomes easy to do
without requiring any extra interpolants to track against local-space bounds.
Essentially, SWGL just performs anti-aliasing on the actual geometry bounds,
but when the pixels on a span's edge are determined to be partially covered
during span rasterization, it uses the same distance field method as WR on
those span boundary pixels to estimate the coverage based on edge slope.
