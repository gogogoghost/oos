use egui::{Context, FullOutput, TextureId};
use oos_app::{GfxDrawCommand, GfxVertex, TextureFlags, TextureFormat};

#[derive(Clone, Copy, Debug)]
pub enum Error {
    TextureIdOverflow,
    TextureUpload,
    VertexLimit,
    IndexOverflow,
    PaintCallbackUnsupported,
    FrameSubmission,
}

impl Error {
    pub const fn message(self) -> &'static str {
        match self {
            Self::TextureIdOverflow => "egui texture id overflow",
            Self::TextureUpload => "egui texture upload failed",
            Self::VertexLimit => "egui frame exceeds the OOS 16-bit vertex limit",
            Self::IndexOverflow => "egui index overflow",
            Self::PaintCallbackUnsupported => {
                "egui paint callback requires an explicit OOS GLES adapter"
            }
            Self::FrameSubmission => "egui frame submission failed",
        }
    }
}

/// Reusable egui renderer for the OOS indexed-triangle WIT interface.
///
/// The scratch vectors retain their capacity between frames. Texture bytes are
/// passed directly from Wasm linear memory to the host after egui's Color32
/// pixels have been flattened into the required canonical RGBA layout.
#[derive(Default)]
pub struct Renderer {
    rgba: Vec<u8>,
    vertices: Vec<GfxVertex>,
    indices: Vec<u16>,
    commands: Vec<GfxDrawCommand>,
}

impl Renderer {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn submit(
        &mut self,
        context: &Context,
        output: FullOutput,
        clear_rgba: [u8; 4],
    ) -> Result<(), Error> {
        let FullOutput {
            textures_delta,
            shapes,
            pixels_per_point,
            ..
        } = output;
        let result = (|| {
            for (texture_id, delta) in &textures_delta.set {
                let size = delta.image.size();
                let egui::ImageData::Color(image) = &delta.image;
                self.rgba.clear();
                self.rgba.reserve(image.pixels.len() * 4);
                for color in &image.pixels {
                    self.rgba.extend_from_slice(&color.to_array());
                }
                let position = delta.pos.unwrap_or([0, 0]);
                oos_app::texture_set(
                    texture_handle(*texture_id)?,
                    TextureFormat::Rgba8888,
                    [position[0] as u32, position[1] as u32],
                    [size[0] as u32, size[1] as u32],
                    size[0] as u32 * 4,
                    TextureFlags::LINEAR
                        | if delta.pos.is_none() {
                            TextureFlags::REPLACE
                        } else {
                            TextureFlags::empty()
                        },
                    &self.rgba,
                )
                .map_err(|_| Error::TextureUpload)?;
            }

            self.vertices.clear();
            self.indices.clear();
            self.commands.clear();
            let primitives = context.tessellate(shapes, pixels_per_point);
            for primitive in primitives {
                let egui::epaint::Primitive::Mesh(mesh) = primitive.primitive else {
                    return Err(Error::PaintCallbackUnsupported);
                };
                let vertex_base = self.vertices.len();
                let first_index = self.indices.len();
                if vertex_base + mesh.vertices.len() > u16::MAX as usize {
                    return Err(Error::VertexLimit);
                }
                self.vertices
                    .extend(mesh.vertices.iter().map(|vertex| GfxVertex {
                        position_x: vertex.pos.x,
                        position_y: vertex.pos.y,
                        uv_x: vertex.uv.x,
                        uv_y: vertex.uv.y,
                        red: vertex.color.r(),
                        green: vertex.color.g(),
                        blue: vertex.color.b(),
                        alpha: vertex.color.a(),
                    }));
                for index in &mesh.indices {
                    let adjusted = vertex_base + *index as usize;
                    self.indices
                        .push(u16::try_from(adjusted).map_err(|_| Error::IndexOverflow)?);
                }
                self.commands.push(GfxDrawCommand {
                    first_index: first_index as u32,
                    index_count: mesh.indices.len() as u32,
                    texture: texture_handle(mesh.texture_id)?,
                    clip_min_x: primitive.clip_rect.min.x,
                    clip_min_y: primitive.clip_rect.min.y,
                    clip_max_x: primitive.clip_rect.max.x,
                    clip_max_y: primitive.clip_rect.max.y,
                });
            }
            oos_app::submit(&self.vertices, &self.indices, &self.commands, clear_rgba)
                .map_err(|_| Error::FrameSubmission)
        })();

        let mut cleanup = Ok(());
        for texture_id in &textures_delta.free {
            let free = texture_handle(*texture_id)
                .and_then(|handle| oos_app::texture_free(handle).map_err(|_| Error::TextureUpload));
            if cleanup.is_ok() {
                cleanup = free;
            }
        }
        result.and(cleanup)
    }
}

/// Host texture that can be displayed by egui as `TextureId::User`.
///
/// This is intended for canvas-style producers such as LVGL ports, emulators,
/// video decoders, and software game engines. RGB565 data remains RGB565 all
/// the way to the host upload; no RGBA expansion is performed by this adapter.
pub struct CanvasTexture {
    id: TextureId,
    handle: u32,
    format: TextureFormat,
    options: TextureFlags,
    live: bool,
}

impl CanvasTexture {
    pub fn create(
        user_id: u32,
        format: TextureFormat,
        size: [u32; 2],
        row_stride: u32,
        options: TextureFlags,
        pixels: &[u8],
    ) -> Result<Self, Error> {
        let id = TextureId::User(user_id as u64);
        let handle = texture_handle(id)?;
        let sampler_options = options & !TextureFlags::REPLACE;
        oos_app::texture_set(
            handle,
            format,
            [0, 0],
            size,
            row_stride,
            sampler_options | TextureFlags::REPLACE,
            pixels,
        )
        .map_err(|_| Error::TextureUpload)?;
        Ok(Self {
            id,
            handle,
            format,
            options: sampler_options,
            live: true,
        })
    }

    pub const fn id(&self) -> TextureId {
        self.id
    }

    pub const fn format(&self) -> TextureFormat {
        self.format
    }

    pub fn update(
        &self,
        position: [u32; 2],
        size: [u32; 2],
        row_stride: u32,
        pixels: &[u8],
    ) -> Result<(), Error> {
        oos_app::texture_set(
            self.handle,
            self.format,
            position,
            size,
            row_stride,
            self.options,
            pixels,
        )
        .map_err(|_| Error::TextureUpload)
    }

    pub fn replace(
        &mut self,
        format: TextureFormat,
        size: [u32; 2],
        row_stride: u32,
        options: TextureFlags,
        pixels: &[u8],
    ) -> Result<(), Error> {
        let sampler_options = options & !TextureFlags::REPLACE;
        oos_app::texture_set(
            self.handle,
            format,
            [0, 0],
            size,
            row_stride,
            sampler_options | TextureFlags::REPLACE,
            pixels,
        )
        .map_err(|_| Error::TextureUpload)?;
        self.format = format;
        self.options = sampler_options;
        Ok(())
    }

    pub fn free(mut self) -> Result<(), Error> {
        self.live = false;
        oos_app::texture_free(self.handle).map_err(|_| Error::TextureUpload)
    }
}

impl Drop for CanvasTexture {
    fn drop(&mut self) {
        if self.live {
            let _ = oos_app::texture_free(self.handle);
        }
    }
}

pub fn texture_handle(texture: TextureId) -> Result<u32, Error> {
    match texture {
        TextureId::Managed(id) => id
            .checked_add(1)
            .and_then(|id| u32::try_from(id).ok())
            .filter(|id| id & 0x8000_0000 == 0)
            .ok_or(Error::TextureIdOverflow),
        TextureId::User(id) => u32::try_from(id)
            .ok()
            .filter(|id| id & 0x8000_0000 == 0)
            .map(|id| id | 0x8000_0000)
            .ok_or(Error::TextureIdOverflow),
    }
}
