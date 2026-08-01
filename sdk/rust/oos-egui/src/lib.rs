use std::sync::Arc;

use egui::{
    Context, Event, FontData, FontDefinitions, FontFamily, FullOutput, Key, OrderedViewportIdMap,
    PlatformOutput, Pos2, RawInput, Rect, TextureFilter, TextureId, TextureOptions,
    TextureWrapMode, Vec2, ViewportOutput,
};
use oos_app::{
    FontRole, GfxDrawCommand, GfxVertex, KeyAction, KeyEvent, TextureFlags, TextureFormat,
};

#[derive(Clone, Copy, Debug)]
pub enum Error {
    FontUnavailable,
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
            Self::FontUnavailable => "OOS system UI font is unavailable",
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

/// Install the OOS-provided UI font without retaining another copy in the app.
/// The WIT-owned vector is moved directly into egui's font database.
pub fn install_system_fonts(context: &Context) -> Result<(), Error> {
    let bytes =
        oos_app::font_assets::load(FontRole::UiProportional).map_err(|_| Error::FontUnavailable)?;
    let name = "oos-ui".to_owned();
    let mut definitions = FontDefinitions::empty();
    definitions
        .font_data
        .insert(name.clone(), Arc::new(FontData::from_owned(bytes)));
    definitions
        .families
        .insert(FontFamily::Proportional, vec![name.clone()]);
    definitions
        .families
        .insert(FontFamily::Monospace, vec![name]);
    context.set_fonts(definitions);
    Ok(())
}

/// Keypad-focused egui input integration for OOS native applications.
#[derive(Default)]
pub struct Input {
    events: Vec<Event>,
}

impl Input {
    pub fn new() -> Self {
        Self::default()
    }

    /// Queue one OOS key event. Returns false for keys that egui cannot name.
    pub fn push_key(&mut self, event: KeyEvent) -> bool {
        let Some((key, text)) = key_mapping(event.code) else {
            return false;
        };
        let (pressed, repeat) = match event.action {
            KeyAction::Released => (false, false),
            KeyAction::Pressed => (true, false),
            KeyAction::Repeated => (true, true),
        };
        self.events.push(Event::Key {
            key,
            physical_key: Some(key),
            pressed,
            repeat,
            modifiers: egui::Modifiers::NONE,
        });
        if pressed {
            if let Some(text) = text {
                self.events.push(Event::Text(text.to_owned()));
            }
        }
        true
    }

    /// Build input using logical points while preserving the physical OOS
    /// surface size and texture limit.
    pub fn take(&mut self, monotonic_time_us: u64) -> RawInput {
        let [width, height] = oos_app::surface_size();
        let native_scale = oos_app::pixels_per_point();
        let native_scale = if native_scale.is_finite() && native_scale > 0.0 {
            native_scale
        } else {
            1.0
        };
        let mut input = RawInput {
            screen_rect: Some(Rect::from_min_size(
                Pos2::ZERO,
                Vec2::new(width as f32 / native_scale, height as f32 / native_scale),
            )),
            max_texture_side: Some(oos_app::MAX_TEXTURE_SIZE),
            time: Some(monotonic_time_us as f64 / 1_000_000.0),
            events: std::mem::take(&mut self.events),
            ..Default::default()
        };
        if let Some(viewport) = input.viewports.get_mut(&input.viewport_id) {
            viewport.native_pixels_per_point = Some(native_scale);
            viewport.inner_rect = input.screen_rect;
            viewport.focused = Some(true);
        }
        input
    }
}

fn key_mapping(code: u32) -> Option<(Key, Option<&'static str>)> {
    Some(match code {
        2 => (Key::Num1, Some("1")),
        3 => (Key::Num2, Some("2")),
        4 => (Key::Num3, Some("3")),
        5 => (Key::Num4, Some("4")),
        6 => (Key::Num5, Some("5")),
        7 => (Key::Num6, Some("6")),
        8 => (Key::Num7, Some("7")),
        9 => (Key::Num8, Some("8")),
        10 => (Key::Num9, Some("9")),
        11 => (Key::Num0, Some("0")),
        103 => (Key::ArrowUp, None),
        105 => (Key::ArrowLeft, None),
        106 => (Key::ArrowRight, None),
        108 => (Key::ArrowDown, None),
        158 => (Key::Escape, None),
        352 => (Key::Enter, None),
        _ => return None,
    })
}

/// Non-painting data that a complete egui application integration must handle.
/// OOS keeps this separate from the renderer because clipboard, URL, IME,
/// accessibility, and viewport policy belong to the application/SystemUI layer.
pub struct BackendOutput {
    pub platform_output: PlatformOutput,
    pub viewport_output: OrderedViewportIdMap<ViewportOutput>,
}

/// Reusable egui renderer for the OOS indexed-triangle WIT interface.
/// Scratch vectors retain their capacity between frames. Color32 is a canonical
/// four-byte RGBA value, so texture pixels cross WIT without a Guest-side copy.
#[derive(Default)]
pub struct Renderer {
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
    ) -> Result<BackendOutput, Error> {
        let FullOutput {
            platform_output,
            textures_delta,
            shapes,
            pixels_per_point,
            viewport_output,
        } = output;
        let result = (|| {
            for (texture_id, delta) in &textures_delta.set {
                let size = delta.image.size();
                let egui::ImageData::Color(image) = &delta.image;
                let position = delta.pos.unwrap_or([0, 0]);
                oos_app::texture_set(
                    texture_handle(*texture_id)?,
                    TextureFormat::Rgba8888,
                    [position[0] as u32, position[1] as u32],
                    [size[0] as u32, size[1] as u32],
                    size[0] as u32 * 4,
                    texture_flags(delta.options, delta.pos.is_none()),
                    bytemuck::cast_slice(image.pixels.as_slice()),
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
                        position_x: vertex.pos.x * pixels_per_point,
                        position_y: vertex.pos.y * pixels_per_point,
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
                    clip_min_x: primitive.clip_rect.min.x * pixels_per_point,
                    clip_min_y: primitive.clip_rect.min.y * pixels_per_point,
                    clip_max_x: primitive.clip_rect.max.x * pixels_per_point,
                    clip_max_y: primitive.clip_rect.max.y * pixels_per_point,
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
        result.and(cleanup).map(|()| BackendOutput {
            platform_output,
            viewport_output,
        })
    }
}

fn texture_flags(options: TextureOptions, replace: bool) -> TextureFlags {
    let mut flags = TextureFlags::empty();
    if options.minification == TextureFilter::Linear {
        flags |= TextureFlags::LINEAR_MINIFICATION;
    }
    if options.magnification == TextureFilter::Linear {
        flags |= TextureFlags::LINEAR_MAGNIFICATION;
    }
    match options.wrap_mode {
        TextureWrapMode::ClampToEdge => {}
        TextureWrapMode::Repeat => flags |= TextureFlags::REPEAT_X | TextureFlags::REPEAT_Y,
        TextureWrapMode::MirroredRepeat => {
            flags |= TextureFlags::MIRRORED_REPEAT_X | TextureFlags::MIRRORED_REPEAT_Y;
        }
    }
    if let Some(filter) = options.mipmap_mode {
        flags |= TextureFlags::MIPMAPS;
        if filter == TextureFilter::Linear {
            flags |= TextureFlags::LINEAR_MIPMAPS;
        }
    }
    if replace {
        flags |= TextureFlags::REPLACE;
    }
    flags
}

/// Host texture that can be displayed by egui as `TextureId::User`.
/// RGB565 data remains RGB565 all the way to the host upload.
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn texture_options_preserve_sampler_state() {
        let flags = texture_flags(TextureOptions::LINEAR_MIRRORED_REPEAT, true);
        assert!(flags.contains(TextureFlags::LINEAR_MINIFICATION));
        assert!(flags.contains(TextureFlags::LINEAR_MAGNIFICATION));
        assert!(flags.contains(TextureFlags::MIRRORED_REPEAT_X));
        assert!(flags.contains(TextureFlags::MIRRORED_REPEAT_Y));
        assert!(flags.contains(TextureFlags::REPLACE));
    }

    #[test]
    fn key_actions_include_release_and_repeat() {
        let mut input = Input::new();
        assert!(input.push_key(KeyEvent {
            code: 2,
            action: KeyAction::Repeated,
            monotonic_time_us: 1,
        }));
        assert!(matches!(
            input.events.first(),
            Some(Event::Key {
                pressed: true,
                repeat: true,
                ..
            })
        ));
        assert!(input.push_key(KeyEvent {
            code: 2,
            action: KeyAction::Released,
            monotonic_time_us: 2,
        }));
        assert!(matches!(
            input.events.last(),
            Some(Event::Key {
                pressed: false,
                repeat: false,
                ..
            })
        ));
    }
}
