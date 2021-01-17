/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use api::{CompositeOperator, FilterPrimitive, FilterPrimitiveInput, FilterPrimitiveKind};
use api::{LineStyle, LineOrientation, ClipMode, MixBlendMode, ColorF, ColorSpace};
use api::units::*;
use crate::clip::{ClipDataStore, ClipItemKind, ClipStore, ClipNodeRange, ClipNodeFlags};
use crate::spatial_tree::SpatialNodeIndex;
use crate::filterdata::SFilterData;
use crate::frame_builder::FrameBuilderConfig;
use crate::frame_graph::PassId;
use crate::gpu_cache::{GpuCache, GpuCacheAddress, GpuCacheHandle};
use crate::gpu_types::{BorderInstance, ImageSource, UvRectKind};
use crate::internal_types::{CacheTextureId, FastHashMap, LayerIndex};
use crate::picture::{ResolvedSurfaceTexture, SurfaceInfo};
use crate::prim_store::{ClipData, PictureIndex};
use crate::prim_store::image::ImageCacheKey;
use crate::prim_store::gradient::{GRADIENT_FP_STOPS, GradientStopKey};
#[cfg(feature = "debugger")]
use crate::print_tree::{PrintTreePrinter};
use crate::resource_cache::ResourceCache;
use std::{usize, f32, i32, u32};
use crate::render_target::{RenderTargetIndex, RenderTargetKind};
use crate::render_task_graph::{RenderTaskId, RenderTaskGraphBuilder};
#[cfg(feature = "debugger")]
use crate::render_task_graph::RenderTaskGraph;
use crate::render_task_cache::{RenderTaskCacheKey, RenderTaskCacheKeyKind, RenderTaskParent};
use crate::visibility::PrimitiveVisibilityMask;
use smallvec::SmallVec;

const FLOATS_PER_RENDER_TASK_INFO: usize = 8;
pub const MAX_RENDER_TASK_SIZE: i32 = 16384;
pub const MAX_BLUR_STD_DEVIATION: f32 = 4.0;
pub const MIN_DOWNSCALING_RT_SIZE: i32 = 8;

fn render_task_sanity_check(size: &DeviceIntSize) {
    if size.width > MAX_RENDER_TASK_SIZE ||
        size.height > MAX_RENDER_TASK_SIZE {
        error!("Attempting to create a render task of size {}x{}", size.width, size.height);
        panic!();
    }
}

#[derive(Debug, Copy, Clone, PartialEq)]
#[repr(C)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct RenderTaskAddress(pub u16);

impl Into<RenderTaskAddress> for RenderTaskId {
    fn into(self) -> RenderTaskAddress {
        RenderTaskAddress(self.index as u16)
    }
}

/// A render task location that targets a persistent output buffer which
/// will be retained over multiple frames.
#[derive(Clone, Debug, Eq, PartialEq, Hash)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum StaticRenderTaskSurface {
    /// The output of the `RenderTask` will be persisted beyond this frame, and
    /// thus should be drawn into the `TextureCache`.
    TextureCache {
        /// Which texture in the texture cache should be drawn into.
        texture: CacheTextureId,
        /// The target layer in the above texture.
        layer: LayerIndex,
        /// What format this texture cache surface is
        target_kind: RenderTargetKind,
    },
    /// This render task will be drawn to a picture cache texture that is
    /// persisted between both frames and scenes, if the content remains valid.
    PictureCache {
        /// Describes either a WR texture or a native OS compositor target
        surface: ResolvedSurfaceTexture,
    },
}

/// Identifies the output buffer location for a given `RenderTask`.
#[derive(Clone, Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum RenderTaskLocation {
    /// A dynamic task that has not yet been allocated a texture and rect.
    Unallocated {
        /// Requested size of this render task
        size: DeviceIntSize,
    },
    /// The `RenderTask` should be drawn to a target provided by the atlas
    /// allocator. This is the most common case.
    Dynamic {
        /// Texture that this task was allocated to render on
        texture_id: CacheTextureId,
        /// Rectangle in the texture this task occupies
        rect: DeviceIntRect,
    },
    /// A task that is output to a persistent / retained target.
    Static {
        /// Target to draw to
        surface: StaticRenderTaskSurface,
        /// Rectangle in the texture this task occupies
        rect: DeviceIntRect,
    },
}

impl RenderTaskLocation {
    /// Returns true if this is a dynamic location.
    pub fn is_dynamic(&self) -> bool {
        match *self {
            RenderTaskLocation::Dynamic { .. } => true,
            _ => false,
        }
    }

    pub fn size(&self) -> DeviceIntSize {
        match self {
            RenderTaskLocation::Unallocated { size } => *size,
            RenderTaskLocation::Dynamic { rect, .. } => rect.size,
            RenderTaskLocation::Static { rect, .. } => rect.size,
        }
    }

    pub fn to_source_rect(&self) -> (DeviceIntRect, LayerIndex) {
        match *self {
            RenderTaskLocation::Unallocated { .. } => panic!("Expected position to be set for the task!"),
            RenderTaskLocation::Dynamic { rect, .. } => (rect, 0),
            RenderTaskLocation::Static { surface: StaticRenderTaskSurface::PictureCache { .. }, .. } => {
                panic!("bug: picture cache tasks should never be a source!");
            }
            RenderTaskLocation::Static { rect, surface: StaticRenderTaskSurface::TextureCache { layer, .. } } => {
                (rect, layer)
            }
        }
    }
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct CacheMaskTask {
    pub actual_rect: DeviceRect,
    pub root_spatial_node_index: SpatialNodeIndex,
    pub clip_node_range: ClipNodeRange,
    pub device_pixel_scale: DevicePixelScale,
    pub clear_to_one: bool,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct ClipRegionTask {
    pub local_pos: LayoutPoint,
    pub device_pixel_scale: DevicePixelScale,
    pub clip_data: ClipData,
    pub clear_to_one: bool,
}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct PictureTask {
    pub pic_index: PictureIndex,
    pub can_merge: bool,
    pub content_origin: DevicePoint,
    pub uv_rect_handle: GpuCacheHandle,
    pub surface_spatial_node_index: SpatialNodeIndex,
    pub uv_rect_kind: UvRectKind,
    pub device_pixel_scale: DevicePixelScale,
    /// A bitfield that describes which dirty regions should be included
    /// in batches built for this picture task.
    pub vis_mask: PrimitiveVisibilityMask,
    pub scissor_rect: Option<DeviceIntRect>,
    pub valid_rect: Option<DeviceIntRect>,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct BlurTask {
    pub blur_std_deviation: f32,
    pub target_kind: RenderTargetKind,
    pub uv_rect_handle: GpuCacheHandle,
    pub blur_region: DeviceIntSize,
    pub uv_rect_kind: UvRectKind,
}

impl BlurTask {
    #[cfg(feature = "debugger")]
    fn print_with<T: PrintTreePrinter>(&self, pt: &mut T) {
        pt.add_item(format!("std deviation: {}", self.blur_std_deviation));
        pt.add_item(format!("target: {:?}", self.target_kind));
    }

    // In order to do the blur down-scaling passes without introducing errors, we need the
    // source of each down-scale pass to be a multuple of two. If need be, this inflates
    // the source size so that each down-scale pass will sample correctly.
    pub fn adjusted_blur_source_size(original_size: DeviceSize, mut std_dev: DeviceSize) -> DeviceSize {
        let mut adjusted_size = original_size;
        let mut scale_factor = 1.0;
        while std_dev.width > MAX_BLUR_STD_DEVIATION && std_dev.height > MAX_BLUR_STD_DEVIATION {
            if adjusted_size.width < MIN_DOWNSCALING_RT_SIZE as f32 ||
               adjusted_size.height < MIN_DOWNSCALING_RT_SIZE as f32 {
                break;
            }
            std_dev = std_dev * 0.5;
            scale_factor *= 2.0;
            adjusted_size = (original_size.to_f32() / scale_factor).ceil();
        }

        adjusted_size * scale_factor
    }
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct ScalingTask {
    pub target_kind: RenderTargetKind,
    pub image: Option<ImageCacheKey>,
    pub uv_rect_kind: UvRectKind,
    pub padding: DeviceIntSideOffsets,
}

// Where the source data for a blit task can be found.
#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum BlitSource {
    Image {
        key: ImageCacheKey,
    },
    RenderTask {
        task_id: RenderTaskId,
    },
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct BorderTask {
    pub instances: Vec<BorderInstance>,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct BlitTask {
    pub source: BlitSource,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct GradientTask {
    pub stops: [GradientStopKey; GRADIENT_FP_STOPS],
    pub orientation: LineOrientation,
    pub start_point: f32,
    pub end_point: f32,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct LineDecorationTask {
    pub wavy_line_thickness: f32,
    pub style: LineStyle,
    pub orientation: LineOrientation,
    pub local_size: LayoutSize,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum SvgFilterInfo {
    Blend(MixBlendMode),
    Flood(ColorF),
    LinearToSrgb,
    SrgbToLinear,
    Opacity(f32),
    ColorMatrix(Box<[f32; 20]>),
    DropShadow(ColorF),
    Offset(DeviceVector2D),
    ComponentTransfer(SFilterData),
    Composite(CompositeOperator),
    // TODO: This is used as a hack to ensure that a blur task's input is always in the blur's previous pass.
    Identity,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct SvgFilterTask {
    pub info: SvgFilterInfo,
    pub extra_gpu_cache_handle: Option<GpuCacheHandle>,
    pub uv_rect_handle: GpuCacheHandle,
    pub uv_rect_kind: UvRectKind,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct RenderTaskData {
    pub data: [f32; FLOATS_PER_RENDER_TASK_INFO],
}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum RenderTaskKind {
    Picture(PictureTask),
    CacheMask(CacheMaskTask),
    ClipRegion(ClipRegionTask),
    VerticalBlur(BlurTask),
    HorizontalBlur(BlurTask),
    Readback,
    Scaling(ScalingTask),
    Blit(BlitTask),
    Border(BorderTask),
    LineDecoration(LineDecorationTask),
    Gradient(GradientTask),
    SvgFilter(SvgFilterTask),
    #[cfg(test)]
    Test(RenderTargetKind),
}

impl RenderTaskKind {
    pub fn as_str(&self) -> &'static str {
        match *self {
            RenderTaskKind::Picture(..) => "Picture",
            RenderTaskKind::CacheMask(..) => "CacheMask",
            RenderTaskKind::ClipRegion(..) => "ClipRegion",
            RenderTaskKind::VerticalBlur(..) => "VerticalBlur",
            RenderTaskKind::HorizontalBlur(..) => "HorizontalBlur",
            RenderTaskKind::Readback => "Readback",
            RenderTaskKind::Scaling(..) => "Scaling",
            RenderTaskKind::Blit(..) => "Blit",
            RenderTaskKind::Border(..) => "Border",
            RenderTaskKind::LineDecoration(..) => "LineDecoration",
            RenderTaskKind::Gradient(..) => "Gradient",
            RenderTaskKind::SvgFilter(..) => "SvgFilter",
            #[cfg(test)]
            RenderTaskKind::Test(..) => "Test",
        }
    }

    pub fn target_kind(&self) -> RenderTargetKind {
        match *self {
            RenderTaskKind::LineDecoration(..) |
            RenderTaskKind::Readback |
            RenderTaskKind::Border(..) |
            RenderTaskKind::Gradient(..) |
            RenderTaskKind::Picture(..) |
            RenderTaskKind::Blit(..) |
            RenderTaskKind::SvgFilter(..) => {
                RenderTargetKind::Color
            }

            RenderTaskKind::ClipRegion(..) |
            RenderTaskKind::CacheMask(..) => {
                RenderTargetKind::Alpha
            }

            RenderTaskKind::VerticalBlur(ref task_info) |
            RenderTaskKind::HorizontalBlur(ref task_info) => {
                task_info.target_kind
            }

            RenderTaskKind::Scaling(ref task_info) => {
                task_info.target_kind
            }

            #[cfg(test)]
            RenderTaskKind::Test(kind) => kind,
        }
    }

    pub fn new_picture(
        size: DeviceIntSize,
        unclipped_size: DeviceSize,
        pic_index: PictureIndex,
        content_origin: DevicePoint,
        uv_rect_kind: UvRectKind,
        surface_spatial_node_index: SpatialNodeIndex,
        device_pixel_scale: DevicePixelScale,
        vis_mask: PrimitiveVisibilityMask,
        scissor_rect: Option<DeviceIntRect>,
        valid_rect: Option<DeviceIntRect>,
    ) -> Self {
        render_task_sanity_check(&size);

        let can_merge = size.width as f32 >= unclipped_size.width &&
                        size.height as f32 >= unclipped_size.height;

        RenderTaskKind::Picture(PictureTask {
            pic_index,
            content_origin,
            can_merge,
            uv_rect_handle: GpuCacheHandle::new(),
            uv_rect_kind,
            surface_spatial_node_index,
            device_pixel_scale,
            vis_mask,
            scissor_rect,
            valid_rect,
        })
    }

    pub fn new_gradient(
        stops: [GradientStopKey; GRADIENT_FP_STOPS],
        orientation: LineOrientation,
        start_point: f32,
        end_point: f32,
    ) -> Self {
        RenderTaskKind::Gradient(GradientTask {
            stops,
            orientation,
            start_point,
            end_point,
        })
    }

    pub fn new_readback() -> Self {
        RenderTaskKind::Readback
    }

    pub fn new_line_decoration(
        style: LineStyle,
        orientation: LineOrientation,
        wavy_line_thickness: f32,
        local_size: LayoutSize,
    ) -> Self {
        RenderTaskKind::LineDecoration(LineDecorationTask {
            style,
            orientation,
            wavy_line_thickness,
            local_size,
        })
    }

    pub fn new_border_segment(
        instances: Vec<BorderInstance>,
    ) -> Self {
        RenderTaskKind::Border(BorderTask {
            instances,
        })
    }

    pub fn new_rounded_rect_mask(
        local_pos: LayoutPoint,
        clip_data: ClipData,
        device_pixel_scale: DevicePixelScale,
        fb_config: &FrameBuilderConfig,
    ) -> Self {
        RenderTaskKind::ClipRegion(ClipRegionTask {
            local_pos,
            device_pixel_scale,
            clip_data,
            clear_to_one: fb_config.gpu_supports_fast_clears,
        })
    }

    pub fn new_mask(
        outer_rect: DeviceRect,
        clip_node_range: ClipNodeRange,
        root_spatial_node_index: SpatialNodeIndex,
        clip_store: &mut ClipStore,
        gpu_cache: &mut GpuCache,
        resource_cache: &mut ResourceCache,
        rg_builder: &mut RenderTaskGraphBuilder,
        clip_data_store: &mut ClipDataStore,
        device_pixel_scale: DevicePixelScale,
        fb_config: &FrameBuilderConfig,
        surfaces: &[SurfaceInfo],
    ) -> RenderTaskId {
        // Step through the clip sources that make up this mask. If we find
        // any box-shadow clip sources, request that image from the render
        // task cache. This allows the blurred box-shadow rect to be cached
        // in the texture cache across frames.
        // TODO(gw): Consider moving this logic outside this function, especially
        //           as we add more clip sources that depend on render tasks.
        // TODO(gw): If this ever shows up in a profile, we could pre-calculate
        //           whether a ClipSources contains any box-shadows and skip
        //           this iteration for the majority of cases.
        let task_size = outer_rect.size.to_i32();

        // If we have a potentially tiled clip mask, clear the mask area first. Otherwise,
        // the first (primary) clip mask will overwrite all the clip mask pixels with
        // blending disabled to set to the initial value.

        let clip_task_id = rg_builder.add().init(
            RenderTask::new_dynamic(
                task_size,
                RenderTaskKind::CacheMask(CacheMaskTask {
                    actual_rect: outer_rect,
                    clip_node_range,
                    root_spatial_node_index,
                    device_pixel_scale,
                    clear_to_one: fb_config.gpu_supports_fast_clears,
                }),
            )
        );

        for i in 0 .. clip_node_range.count {
            let clip_instance = clip_store.get_instance_from_range(&clip_node_range, i);
            let clip_node = &mut clip_data_store[clip_instance.handle];
            match clip_node.item.kind {
                ClipItemKind::BoxShadow { ref mut source } => {
                    let (cache_size, cache_key) = source.cache_key
                        .as_ref()
                        .expect("bug: no cache key set")
                        .clone();
                    let blur_radius_dp = cache_key.blur_radius_dp as f32;
                    let device_pixel_scale = DevicePixelScale::new(cache_key.device_pixel_scale.to_f32_px());

                    // Request a cacheable render task with a blurred, minimal
                    // sized box-shadow rect.
                    source.cache_handle = Some(resource_cache.request_render_task(
                        RenderTaskCacheKey {
                            size: cache_size,
                            kind: RenderTaskCacheKeyKind::BoxShadow(cache_key),
                        },
                        gpu_cache,
                        rg_builder,
                        None,
                        false,
                        RenderTaskParent::RenderTask(clip_task_id),
                        surfaces,
                        |rg_builder| {
                            let clip_data = ClipData::rounded_rect(
                                source.minimal_shadow_rect.size,
                                &source.shadow_radius,
                                ClipMode::Clip,
                            );

                            // Draw the rounded rect.
                            let mask_task_id = rg_builder.add().init(RenderTask::new_dynamic(
                                cache_size,
                                RenderTaskKind::new_rounded_rect_mask(
                                    source.minimal_shadow_rect.origin,
                                    clip_data,
                                    device_pixel_scale,
                                    fb_config,
                                ),
                            ));

                            // Blur it
                            RenderTask::new_blur(
                                DeviceSize::new(blur_radius_dp, blur_radius_dp),
                                mask_task_id,
                                rg_builder,
                                RenderTargetKind::Alpha,
                                None,
                                cache_size,
                            )
                        }
                    ));
                }
                ClipItemKind::Rectangle { mode: ClipMode::Clip, .. } => {
                    if !clip_instance.flags.contains(ClipNodeFlags::SAME_COORD_SYSTEM) {
                        // This is conservative - it's only the case that we actually need
                        // a clear here if we end up adding this mask via add_tiled_clip_mask,
                        // but for simplicity we will just clear if any of these are encountered,
                        // since they are rare.
                        let clip_task = rg_builder.get_task_mut(clip_task_id);
                        match clip_task.kind {
                            RenderTaskKind::CacheMask(ref mut task) => {
                                task.clear_to_one = true;
                            }
                            _ => {
                                unreachable!();
                            }
                        }
                    }
                }
                ClipItemKind::Rectangle { mode: ClipMode::ClipOut, .. } |
                ClipItemKind::RoundedRectangle { .. } |
                ClipItemKind::Image { .. } => {}
            }
        }

        clip_task_id
    }

    // Write (up to) 8 floats of data specific to the type
    // of render task that is provided to the GPU shaders
    // via a vertex texture.
    pub fn write_task_data(
        &self,
        target_rect: DeviceIntRect,
        target_index: RenderTargetIndex,
    ) -> RenderTaskData {
        // NOTE: The ordering and layout of these structures are
        //       required to match both the GPU structures declared
        //       in prim_shared.glsl, and also the uses in submit_batch()
        //       in renderer.rs.
        // TODO(gw): Maybe there's a way to make this stuff a bit
        //           more type-safe. Although, it will always need
        //           to be kept in sync with the GLSL code anyway.

        let data = match self {
            RenderTaskKind::Picture(ref task) => {
                // Note: has to match `PICTURE_TYPE_*` in shaders
                [
                    task.device_pixel_scale.0,
                    task.content_origin.x,
                    task.content_origin.y,
                ]
            }
            RenderTaskKind::CacheMask(ref task) => {
                [
                    task.device_pixel_scale.0,
                    task.actual_rect.origin.x,
                    task.actual_rect.origin.y,
                ]
            }
            RenderTaskKind::ClipRegion(ref task) => {
                [
                    task.device_pixel_scale.0,
                    0.0,
                    0.0,
                ]
            }
            RenderTaskKind::VerticalBlur(ref task) |
            RenderTaskKind::HorizontalBlur(ref task) => {
                [
                    task.blur_std_deviation,
                    task.blur_region.width as f32,
                    task.blur_region.height as f32,
                ]
            }
            RenderTaskKind::Readback |
            RenderTaskKind::Scaling(..) |
            RenderTaskKind::Border(..) |
            RenderTaskKind::LineDecoration(..) |
            RenderTaskKind::Gradient(..) |
            RenderTaskKind::Blit(..) => {
                [0.0; 3]
            }


            RenderTaskKind::SvgFilter(ref task) => {
                match task.info {
                    SvgFilterInfo::Opacity(opacity) => [opacity, 0.0, 0.0],
                    SvgFilterInfo::Offset(offset) => [offset.x, offset.y, 0.0],
                    _ => [0.0; 3]
                }
            }

            #[cfg(test)]
            RenderTaskKind::Test(..) => {
                [0.0; 3]
            }
        };

        RenderTaskData {
            data: [
                target_rect.origin.x as f32,
                target_rect.origin.y as f32,
                target_rect.size.width as f32,
                target_rect.size.height as f32,
                target_index.0 as f32,
                data[0],
                data[1],
                data[2],
            ]
        }
    }

    pub fn write_gpu_blocks(
        &mut self,
        target_rect: DeviceIntRect,
        target_index: RenderTargetIndex,
        gpu_cache: &mut GpuCache,
    ) {
        profile_scope!("write_gpu_blocks");

        let (cache_handle, uv_rect_kind) = match self {
            RenderTaskKind::HorizontalBlur(ref mut info) |
            RenderTaskKind::VerticalBlur(ref mut info) => {
                (&mut info.uv_rect_handle, info.uv_rect_kind)
            }
            RenderTaskKind::Picture(ref mut info) => {
                (&mut info.uv_rect_handle, info.uv_rect_kind)
            }
            RenderTaskKind::SvgFilter(ref mut info) => {
                (&mut info.uv_rect_handle, info.uv_rect_kind)
            }
            RenderTaskKind::Readback |
            RenderTaskKind::Scaling(..) |
            RenderTaskKind::Blit(..) |
            RenderTaskKind::ClipRegion(..) |
            RenderTaskKind::Border(..) |
            RenderTaskKind::CacheMask(..) |
            RenderTaskKind::Gradient(..) |
            RenderTaskKind::LineDecoration(..) => {
                return;
            }
            #[cfg(test)]
            RenderTaskKind::Test(..) => {
                return;
            }
        };

        if let Some(mut request) = gpu_cache.request(cache_handle) {
            let p0 = target_rect.min().to_f32();
            let p1 = target_rect.max().to_f32();
            let image_source = ImageSource {
                p0,
                p1,
                texture_layer: target_index.0 as f32,
                user_data: [0.0; 3],
                uv_rect_kind,
            };
            image_source.write_gpu_blocks(&mut request);
        }

        if let RenderTaskKind::SvgFilter(ref mut filter_task) = self {
            match filter_task.info {
                SvgFilterInfo::ColorMatrix(ref matrix) => {
                    let handle = filter_task.extra_gpu_cache_handle.get_or_insert_with(GpuCacheHandle::new);
                    if let Some(mut request) = gpu_cache.request(handle) {
                        for i in 0..5 {
                            request.push([matrix[i*4], matrix[i*4+1], matrix[i*4+2], matrix[i*4+3]]);
                        }
                    }
                }
                SvgFilterInfo::DropShadow(color) |
                SvgFilterInfo::Flood(color) => {
                    let handle = filter_task.extra_gpu_cache_handle.get_or_insert_with(GpuCacheHandle::new);
                    if let Some(mut request) = gpu_cache.request(handle) {
                        request.push(color.to_array());
                    }
                }
                SvgFilterInfo::ComponentTransfer(ref data) => {
                    let handle = filter_task.extra_gpu_cache_handle.get_or_insert_with(GpuCacheHandle::new);
                    if let Some(request) = gpu_cache.request(handle) {
                        data.update(request);
                    }
                }
                SvgFilterInfo::Composite(ref operator) => {
                    if let CompositeOperator::Arithmetic(k_vals) = operator {
                        let handle = filter_task.extra_gpu_cache_handle.get_or_insert_with(GpuCacheHandle::new);
                        if let Some(mut request) = gpu_cache.request(handle) {
                            request.push(*k_vals);
                        }
                    }
                }
                _ => {},
            }
        }
    }
}

/// In order to avoid duplicating the down-scaling and blur passes when a picture has several blurs,
/// we use a local (primitive-level) cache of the render tasks generated for a single shadowed primitive
/// in a single frame.
pub type BlurTaskCache = FastHashMap<BlurTaskKey, RenderTaskId>;

/// Since we only use it within a single primitive, the key only needs to contain the down-scaling level
/// and the blur std deviation.
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum BlurTaskKey {
    DownScale(u32),
    Blur { downscale_level: u32, stddev_x: u32, stddev_y: u32 },
}

impl BlurTaskKey {
    fn downscale_and_blur(downscale_level: u32, blur_stddev: DeviceSize) -> Self {
        // Quantise the std deviations and store it as integers to work around
        // Eq and Hash's f32 allergy.
        // The blur radius is rounded before RenderTask::new_blur so we don't need
        // a lot of precision.
        const QUANTIZATION_FACTOR: f32 = 1024.0;
        let stddev_x = (blur_stddev.width * QUANTIZATION_FACTOR) as u32;
        let stddev_y = (blur_stddev.height * QUANTIZATION_FACTOR) as u32;
        BlurTaskKey::Blur { downscale_level, stddev_x, stddev_y }
    }
}

// The majority of render tasks have 0, 1 or 2 dependencies, except for pictures that
// typically have dozens to hundreds of dependencies. SmallVec with 2 inline elements
// avoids many tiny heap allocations in pages with a lot of text shadows and other
// types of render tasks.
pub type TaskDependencies = SmallVec<[RenderTaskId;2]>;

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct RenderTask {
    pub location: RenderTaskLocation,
    pub children: TaskDependencies,
    pub kind: RenderTaskKind,

    // TODO(gw): These fields and perhaps others can become private once the
    //           frame_graph / render_task source files are unified / cleaned up.
    pub free_after: PassId,
    pub render_on: PassId,
}

impl RenderTask {
    pub fn new(
        location: RenderTaskLocation,
        kind: RenderTaskKind,
    ) -> Self {
        render_task_sanity_check(&location.size());

        RenderTask {
            location,
            children: TaskDependencies::new(),
            kind,
            free_after: PassId::MAX,
            render_on: PassId::MIN,
        }
    }

    pub fn new_dynamic(
        size: DeviceIntSize,
        kind: RenderTaskKind,
    ) -> Self {
        RenderTask::new(
            RenderTaskLocation::Unallocated { size },
            kind,
        )
    }

    #[cfg(test)]
    pub fn new_test(
        location: RenderTaskLocation,
        target: RenderTargetKind,
    ) -> Self {
        RenderTask {
            location,
            children: TaskDependencies::new(),
            kind: RenderTaskKind::Test(target),
            free_after: PassId::MAX,
            render_on: PassId::MIN,
        }
    }

    pub fn new_blit(
        size: DeviceIntSize,
        source: BlitSource,
        rg_builder: &mut RenderTaskGraphBuilder,
    ) -> RenderTaskId {
        // If this blit uses a render task as a source,
        // ensure it's added as a child task. This will
        // ensure it gets allocated in the correct pass
        // and made available as an input when this task
        // executes.
        let child_id = match source {
            BlitSource::RenderTask { task_id } => Some(task_id),
            BlitSource::Image { .. } => None,
        };

        let blit_task_id = rg_builder.add().init(RenderTask::new_dynamic(
            size,
            RenderTaskKind::Blit(BlitTask {
                source,
            }),
        ));

        if let Some(child_id) = child_id {
            rg_builder.add_dependency(blit_task_id, child_id);
        }

        blit_task_id
    }

    // Construct a render task to apply a blur to a primitive.
    // The render task chain that is constructed looks like:
    //
    //    PrimitiveCacheTask: Draw the primitives.
    //           ^
    //           |
    //    DownscalingTask(s): Each downscaling task reduces the size of render target to
    //           ^            half. Also reduce the std deviation to half until the std
    //           |            deviation less than 4.0.
    //           |
    //           |
    //    VerticalBlurTask: Apply the separable vertical blur to the primitive.
    //           ^
    //           |
    //    HorizontalBlurTask: Apply the separable horizontal blur to the vertical blur.
    //           |
    //           +---- This is stored as the input task to the primitive shader.
    //
    pub fn new_blur(
        blur_std_deviation: DeviceSize,
        src_task_id: RenderTaskId,
        rg_builder: &mut RenderTaskGraphBuilder,
        target_kind: RenderTargetKind,
        mut blur_cache: Option<&mut BlurTaskCache>,
        blur_region: DeviceIntSize,
    ) -> RenderTaskId {
        // Adjust large std deviation value.
        let mut adjusted_blur_std_deviation = blur_std_deviation;
        let (blur_target_size, uv_rect_kind) = {
            let src_task = rg_builder.get_task(src_task_id);
            (src_task.location.size(), src_task.uv_rect_kind())
        };
        let mut adjusted_blur_target_size = blur_target_size;
        let mut downscaling_src_task_id = src_task_id;
        let mut scale_factor = 1.0;
        let mut n_downscales = 1;
        while adjusted_blur_std_deviation.width > MAX_BLUR_STD_DEVIATION &&
              adjusted_blur_std_deviation.height > MAX_BLUR_STD_DEVIATION {
            if adjusted_blur_target_size.width < MIN_DOWNSCALING_RT_SIZE ||
               adjusted_blur_target_size.height < MIN_DOWNSCALING_RT_SIZE {
                break;
            }
            adjusted_blur_std_deviation = adjusted_blur_std_deviation * 0.5;
            scale_factor *= 2.0;
            adjusted_blur_target_size = (blur_target_size.to_f32() / scale_factor).to_i32();

            let cached_task = match blur_cache {
                Some(ref mut cache) => cache.get(&BlurTaskKey::DownScale(n_downscales)).cloned(),
                None => None,
            };

            downscaling_src_task_id = cached_task.unwrap_or_else(|| {
                RenderTask::new_scaling(
                    downscaling_src_task_id,
                    rg_builder,
                    target_kind,
                    adjusted_blur_target_size,
                )
            });

            if let Some(ref mut cache) = blur_cache {
                cache.insert(BlurTaskKey::DownScale(n_downscales), downscaling_src_task_id);
            }

            n_downscales += 1;
        }


        let blur_key = BlurTaskKey::downscale_and_blur(n_downscales, adjusted_blur_std_deviation);

        let cached_task = match blur_cache {
            Some(ref mut cache) => cache.get(&blur_key).cloned(),
            None => None,
        };

        let blur_region = blur_region / (scale_factor as i32);

        let blur_task_id = cached_task.unwrap_or_else(|| {
            let blur_task_v = rg_builder.add().init(RenderTask::new_dynamic(
                adjusted_blur_target_size,
                RenderTaskKind::VerticalBlur(BlurTask {
                    blur_std_deviation: adjusted_blur_std_deviation.height,
                    target_kind,
                    uv_rect_handle: GpuCacheHandle::new(),
                    blur_region,
                    uv_rect_kind,
                }),
            ));
            rg_builder.add_dependency(blur_task_v, downscaling_src_task_id);

            let task_id = rg_builder.add().init(RenderTask::new_dynamic(
                adjusted_blur_target_size,
                RenderTaskKind::HorizontalBlur(BlurTask {
                    blur_std_deviation: adjusted_blur_std_deviation.width,
                    target_kind,
                    uv_rect_handle: GpuCacheHandle::new(),
                    blur_region,
                    uv_rect_kind,
                }),
            ));
            rg_builder.add_dependency(task_id, blur_task_v);

            task_id
        });

        if let Some(ref mut cache) = blur_cache {
            cache.insert(blur_key, blur_task_id);
        }

        blur_task_id
    }

    pub fn new_scaling(
        src_task_id: RenderTaskId,
        rg_builder: &mut RenderTaskGraphBuilder,
        target_kind: RenderTargetKind,
        size: DeviceIntSize,
    ) -> RenderTaskId {
        Self::new_scaling_with_padding(
            BlitSource::RenderTask { task_id: src_task_id },
            rg_builder,
            target_kind,
            size,
            DeviceIntSideOffsets::zero(),
        )
    }

    pub fn new_scaling_with_padding(
        source: BlitSource,
        rg_builder: &mut RenderTaskGraphBuilder,
        target_kind: RenderTargetKind,
        padded_size: DeviceIntSize,
        padding: DeviceIntSideOffsets,
    ) -> RenderTaskId {
        let (uv_rect_kind, child, image) = match source {
            BlitSource::RenderTask { task_id } => (rg_builder.get_task(task_id).uv_rect_kind(), Some(task_id), None),
            BlitSource::Image { key } => (UvRectKind::Rect, None, Some(key)),
        };

        let task_id = rg_builder.add().init(
            RenderTask::new_dynamic(
                padded_size,
                RenderTaskKind::Scaling(ScalingTask {
                    target_kind,
                    image,
                    uv_rect_kind,
                    padding,
                }),
            )
        );

        if let Some(child_id) = child {
            rg_builder.add_dependency(task_id, child_id);
        }

        task_id
    }

    pub fn new_svg_filter(
        filter_primitives: &[FilterPrimitive],
        filter_datas: &[SFilterData],
        rg_builder: &mut RenderTaskGraphBuilder,
        content_size: DeviceIntSize,
        uv_rect_kind: UvRectKind,
        original_task_id: RenderTaskId,
        device_pixel_scale: DevicePixelScale,
    ) -> RenderTaskId {

        if filter_primitives.is_empty() {
            return original_task_id;
        }

        // Resolves the input to a filter primitive
        let get_task_input = |
            input: &FilterPrimitiveInput,
            filter_primitives: &[FilterPrimitive],
            rg_builder: &mut RenderTaskGraphBuilder,
            cur_index: usize,
            outputs: &[RenderTaskId],
            original: RenderTaskId,
            color_space: ColorSpace,
        | {
            // TODO(cbrewster): Not sure we can assume that the original input is sRGB.
            let (mut task_id, input_color_space) = match input.to_index(cur_index) {
                Some(index) => (outputs[index], filter_primitives[index].color_space),
                None => (original, ColorSpace::Srgb),
            };

            match (input_color_space, color_space) {
                (ColorSpace::Srgb, ColorSpace::LinearRgb) => {
                    task_id = RenderTask::new_svg_filter_primitive(
                        smallvec![task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::SrgbToLinear,
                        rg_builder,
                    );
                },
                (ColorSpace::LinearRgb, ColorSpace::Srgb) => {
                    task_id = RenderTask::new_svg_filter_primitive(
                        smallvec![task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::LinearToSrgb,
                        rg_builder,
                    );
                },
                _ => {},
            }

            task_id
        };

        let mut outputs = vec![];
        let mut cur_filter_data = 0;
        for (cur_index, primitive) in filter_primitives.iter().enumerate() {
            let render_task_id = match primitive.kind {
                FilterPrimitiveKind::Identity(ref identity) => {
                    // Identity does not create a task, it provides its input's render task
                    get_task_input(
                        &identity.input,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    )
                }
                FilterPrimitiveKind::Blend(ref blend) => {
                    let input_1_task_id = get_task_input(
                        &blend.input1,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );
                    let input_2_task_id = get_task_input(
                        &blend.input2,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );

                    RenderTask::new_svg_filter_primitive(
                        smallvec![input_1_task_id, input_2_task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::Blend(blend.mode),
                        rg_builder,
                    )
                },
                FilterPrimitiveKind::Flood(ref flood) => {
                    RenderTask::new_svg_filter_primitive(
                        smallvec![],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::Flood(flood.color),
                        rg_builder,
                    )
                }
                FilterPrimitiveKind::Blur(ref blur) => {
                    let width_std_deviation = blur.width * device_pixel_scale.0;
                    let height_std_deviation = blur.height * device_pixel_scale.0;
                    let input_task_id = get_task_input(
                        &blur.input,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );

                    RenderTask::new_blur(
                        DeviceSize::new(width_std_deviation, height_std_deviation),
                        // TODO: This is a hack to ensure that a blur task's input is always
                        // in the blur's previous pass.
                        RenderTask::new_svg_filter_primitive(
                            smallvec![input_task_id],
                            content_size,
                            uv_rect_kind,
                            SvgFilterInfo::Identity,
                            rg_builder,
                        ),
                        rg_builder,
                        RenderTargetKind::Color,
                        None,
                        content_size,
                    )
                }
                FilterPrimitiveKind::Opacity(ref opacity) => {
                    let input_task_id = get_task_input(
                        &opacity.input,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );

                    RenderTask::new_svg_filter_primitive(
                        smallvec![input_task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::Opacity(opacity.opacity),
                        rg_builder,
                    )
                }
                FilterPrimitiveKind::ColorMatrix(ref color_matrix) => {
                    let input_task_id = get_task_input(
                        &color_matrix.input,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );

                    RenderTask::new_svg_filter_primitive(
                        smallvec![input_task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::ColorMatrix(Box::new(color_matrix.matrix)),
                        rg_builder,
                    )
                }
                FilterPrimitiveKind::DropShadow(ref drop_shadow) => {
                    let input_task_id = get_task_input(
                        &drop_shadow.input,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );

                    let blur_std_deviation = drop_shadow.shadow.blur_radius * device_pixel_scale.0;
                    let offset = drop_shadow.shadow.offset * LayoutToWorldScale::new(1.0) * device_pixel_scale;

                    let offset_task_id = RenderTask::new_svg_filter_primitive(
                        smallvec![input_task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::Offset(offset),
                        rg_builder,
                    );

                    let blur_task_id = RenderTask::new_blur(
                        DeviceSize::new(blur_std_deviation, blur_std_deviation),
                        offset_task_id,
                        rg_builder,
                        RenderTargetKind::Color,
                        None,
                        content_size,
                    );

                    RenderTask::new_svg_filter_primitive(
                        smallvec![input_task_id, blur_task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::DropShadow(drop_shadow.shadow.color),
                        rg_builder,
                    )
                }
                FilterPrimitiveKind::ComponentTransfer(ref component_transfer) => {
                    let input_task_id = get_task_input(
                        &component_transfer.input,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );

                    let filter_data = &filter_datas[cur_filter_data];
                    cur_filter_data += 1;
                    if filter_data.is_identity() {
                        input_task_id
                    } else {
                        RenderTask::new_svg_filter_primitive(
                            smallvec![input_task_id],
                            content_size,
                            uv_rect_kind,
                            SvgFilterInfo::ComponentTransfer(filter_data.clone()),
                            rg_builder,
                        )
                    }
                }
                FilterPrimitiveKind::Offset(ref info) => {
                    let input_task_id = get_task_input(
                        &info.input,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );

                    let offset = info.offset * LayoutToWorldScale::new(1.0) * device_pixel_scale;
                    RenderTask::new_svg_filter_primitive(
                        smallvec![input_task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::Offset(offset),
                        rg_builder,
                    )
                }
                FilterPrimitiveKind::Composite(info) => {
                    let input_1_task_id = get_task_input(
                        &info.input1,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );
                    let input_2_task_id = get_task_input(
                        &info.input2,
                        filter_primitives,
                        rg_builder,
                        cur_index,
                        &outputs,
                        original_task_id,
                        primitive.color_space
                    );

                    RenderTask::new_svg_filter_primitive(
                        smallvec![input_1_task_id, input_2_task_id],
                        content_size,
                        uv_rect_kind,
                        SvgFilterInfo::Composite(info.operator),
                        rg_builder,
                    )
                }
            };
            outputs.push(render_task_id);
        }

        // The output of a filter is the output of the last primitive in the chain.
        let mut render_task_id = *outputs.last().unwrap();

        // Convert to sRGB if needed
        if filter_primitives.last().unwrap().color_space == ColorSpace::LinearRgb {
            render_task_id = RenderTask::new_svg_filter_primitive(
                smallvec![render_task_id],
                content_size,
                uv_rect_kind,
                SvgFilterInfo::LinearToSrgb,
                rg_builder,
            );
        }

        render_task_id
    }

    pub fn new_svg_filter_primitive(
        tasks: TaskDependencies,
        target_size: DeviceIntSize,
        uv_rect_kind: UvRectKind,
        info: SvgFilterInfo,
        rg_builder: &mut RenderTaskGraphBuilder,
    ) -> RenderTaskId {
        let task_id = rg_builder.add().init(RenderTask::new_dynamic(
            target_size,
            RenderTaskKind::SvgFilter(SvgFilterTask {
                extra_gpu_cache_handle: None,
                uv_rect_handle: GpuCacheHandle::new(),
                uv_rect_kind,
                info,
            }),
        ));

        for child_id in tasks {
            rg_builder.add_dependency(task_id, child_id);
        }

        task_id
    }

    pub fn uv_rect_kind(&self) -> UvRectKind {
        match self.kind {
            RenderTaskKind::CacheMask(..) |
            RenderTaskKind::Readback => {
                unreachable!("bug: unexpected render task");
            }

            RenderTaskKind::Picture(ref task) => {
                task.uv_rect_kind
            }

            RenderTaskKind::VerticalBlur(ref task) |
            RenderTaskKind::HorizontalBlur(ref task) => {
                task.uv_rect_kind
            }

            RenderTaskKind::Scaling(ref task) => {
                task.uv_rect_kind
            }

            RenderTaskKind::SvgFilter(ref task) => {
                task.uv_rect_kind
            }

            RenderTaskKind::ClipRegion(..) |
            RenderTaskKind::Border(..) |
            RenderTaskKind::Gradient(..) |
            RenderTaskKind::LineDecoration(..) |
            RenderTaskKind::Blit(..) => {
                UvRectKind::Rect
            }

            #[cfg(test)]
            RenderTaskKind::Test(..) => {
                unreachable!("Unexpected render task");
            }
        }
    }

    pub fn get_texture_address(&self, gpu_cache: &GpuCache) -> GpuCacheAddress {
        match self.kind {
            RenderTaskKind::Picture(ref info) => {
                gpu_cache.get_address(&info.uv_rect_handle)
            }
            RenderTaskKind::VerticalBlur(ref info) |
            RenderTaskKind::HorizontalBlur(ref info) => {
                gpu_cache.get_address(&info.uv_rect_handle)
            }
            RenderTaskKind::SvgFilter(ref info) => {
                gpu_cache.get_address(&info.uv_rect_handle)
            }
            RenderTaskKind::ClipRegion(..) |
            RenderTaskKind::Readback |
            RenderTaskKind::Scaling(..) |
            RenderTaskKind::Blit(..) |
            RenderTaskKind::Border(..) |
            RenderTaskKind::CacheMask(..) |
            RenderTaskKind::Gradient(..) |
            RenderTaskKind::LineDecoration(..) => {
                panic!("texture handle not supported for this task kind");
            }
            #[cfg(test)]
            RenderTaskKind::Test(..) => {
                panic!("RenderTask tests aren't expected to exercise this code");
            }
        }
    }

    pub fn get_dynamic_size(&self) -> DeviceIntSize {
        self.location.size()
    }

    pub fn get_target_texture(&self) -> CacheTextureId {
        match self.location {
            RenderTaskLocation::Dynamic { texture_id, .. } => {
                assert_ne!(texture_id, CacheTextureId::INVALID);
                texture_id
            }
            RenderTaskLocation::Unallocated { .. } |
            RenderTaskLocation::Static { .. } => {
                unreachable!();
            }
        }
    }

    pub fn get_target_rect(&self) -> (DeviceIntRect, RenderTargetIndex) {
        match self.location {
            // Previously, we only added render tasks after the entire
            // primitive chain was determined visible. This meant that
            // we could assert any render task in the list was also
            // allocated (assigned to passes). Now, we add render
            // tasks earlier, and the picture they belong to may be
            // culled out later, so we can't assert that the task
            // has been allocated.
            // Render tasks that are created but not assigned to
            // passes consume a row in the render task texture, but
            // don't allocate any space in render targets nor
            // draw any pixels.
            // TODO(gw): Consider some kind of tag or other method
            //           to mark a task as unused explicitly. This
            //           would allow us to restore this debug check.
            RenderTaskLocation::Dynamic { rect, .. } => {
                (rect, RenderTargetIndex(0))
            }
            RenderTaskLocation::Unallocated { .. } => {
                panic!("bug: get_target_rect called before allocating");
            }
            RenderTaskLocation::Static { rect, surface: StaticRenderTaskSurface::PictureCache { ref surface, .. } } => {
                let layer = match surface {
                    ResolvedSurfaceTexture::TextureCache { layer, .. } => *layer,
                    ResolvedSurfaceTexture::Native { .. } => 0,
                };

                (
                    rect,
                    RenderTargetIndex(layer as usize),
                )
            }
            RenderTaskLocation::Static { rect, surface: StaticRenderTaskSurface::TextureCache { layer, .. } } => {
                (rect, RenderTargetIndex(layer as usize))
            }
        }
    }

    pub fn target_kind(&self) -> RenderTargetKind {
        self.kind.target_kind()
    }

    #[cfg(feature = "debugger")]
    pub fn print_with<T: PrintTreePrinter>(&self, pt: &mut T, tree: &RenderTaskGraph) -> bool {
        match self.kind {
            RenderTaskKind::Picture(ref task) => {
                pt.new_level(format!("Picture of {:?}", task.pic_index));
            }
            RenderTaskKind::CacheMask(ref task) => {
                pt.new_level(format!("CacheMask with {} clips", task.clip_node_range.count));
                pt.add_item(format!("rect: {:?}", task.actual_rect));
            }
            RenderTaskKind::LineDecoration(..) => {
                pt.new_level("LineDecoration".to_owned());
            }
            RenderTaskKind::ClipRegion(..) => {
                pt.new_level("ClipRegion".to_owned());
            }
            RenderTaskKind::VerticalBlur(ref task) => {
                pt.new_level("VerticalBlur".to_owned());
                task.print_with(pt);
            }
            RenderTaskKind::HorizontalBlur(ref task) => {
                pt.new_level("HorizontalBlur".to_owned());
                task.print_with(pt);
            }
            RenderTaskKind::Readback => {
                pt.new_level("Readback".to_owned());
            }
            RenderTaskKind::Scaling(ref kind) => {
                pt.new_level("Scaling".to_owned());
                pt.add_item(format!("kind: {:?}", kind));
            }
            RenderTaskKind::Border(..) => {
                pt.new_level("Border".to_owned());
            }
            RenderTaskKind::Blit(ref task) => {
                pt.new_level("Blit".to_owned());
                pt.add_item(format!("source: {:?}", task.source));
            }
            RenderTaskKind::Gradient(..) => {
                pt.new_level("Gradient".to_owned());
            }
            RenderTaskKind::SvgFilter(ref task) => {
                pt.new_level("SvgFilter".to_owned());
                pt.add_item(format!("primitive: {:?}", task.info));
            }
            #[cfg(test)]
            RenderTaskKind::Test(..) => {
                pt.new_level("Test".to_owned());
            }
        }

        pt.add_item(format!("dimensions: {:?}", self.location.size()));

        for &child_id in &self.children {
            if tree[child_id].print_with(pt, tree) {
                pt.add_item(format!("self: {:?}", child_id))
            }
        }

        pt.end_level();
        true
    }
}
