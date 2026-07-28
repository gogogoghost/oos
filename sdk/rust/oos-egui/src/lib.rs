use egui::{Context, FullOutput, TextureId};
use oos_app::{GfxDrawCommand, GfxVertex};

#[derive(Clone, Copy, Debug)]
pub enum Error {
    TextureIdOverflow,
    TextureUpload,
    VertexLimit,
    IndexOverflow,
    FrameSubmission,
}

impl Error {
    pub const fn message(self) -> &'static str {
        match self {
            Self::TextureIdOverflow => "egui texture id overflow",
            Self::TextureUpload => "egui texture upload failed",
            Self::VertexLimit => "egui frame exceeds the OOS 16-bit vertex limit",
            Self::IndexOverflow => "egui index overflow",
            Self::FrameSubmission => "egui frame submission failed",
        }
    }
}

pub fn submit(context: &Context, output: FullOutput, clear_rgba: [u8; 4]) -> Result<(), Error> {
    for (texture_id, delta) in &output.textures_delta.set {
        let size = delta.image.size();
        let egui::ImageData::Color(image) = &delta.image;
        let rgba: Vec<u8> = image
            .pixels
            .iter()
            .flat_map(|color| color.to_array())
            .collect();
        let position = delta.pos.unwrap_or([0, 0]);
        oos_app::texture_set(
            texture_handle(*texture_id)?,
            [position[0] as u32, position[1] as u32],
            [size[0] as u32, size[1] as u32],
            true,
            delta.pos.is_none(),
            &rgba,
        )
        .map_err(|_| Error::TextureUpload)?;
    }

    let primitives = context.tessellate(output.shapes, output.pixels_per_point);
    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    let mut commands = Vec::new();
    for primitive in primitives {
        let egui::epaint::Primitive::Mesh(mesh) = primitive.primitive else {
            continue;
        };
        let vertex_base = vertices.len();
        let first_index = indices.len();
        if vertex_base + mesh.vertices.len() > u16::MAX as usize {
            return Err(Error::VertexLimit);
        }
        vertices.extend(mesh.vertices.iter().map(|vertex| GfxVertex {
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
            indices.push(u16::try_from(adjusted).map_err(|_| Error::IndexOverflow)?);
        }
        commands.push(GfxDrawCommand {
            first_index: first_index as u32,
            index_count: mesh.indices.len() as u32,
            texture: texture_handle(mesh.texture_id)?,
            clip_min_x: primitive.clip_rect.min.x,
            clip_min_y: primitive.clip_rect.min.y,
            clip_max_x: primitive.clip_rect.max.x,
            clip_max_y: primitive.clip_rect.max.y,
        });
    }
    oos_app::submit(&vertices, &indices, &commands, clear_rgba)
        .map_err(|_| Error::FrameSubmission)?;
    for texture_id in &output.textures_delta.free {
        let _ = oos_app::texture_free(texture_handle(*texture_id)?);
    }
    Ok(())
}

fn texture_handle(texture: TextureId) -> Result<u32, Error> {
    match texture {
        TextureId::Managed(id) => u32::try_from(id + 1).map_err(|_| Error::TextureIdOverflow),
        TextureId::User(id) => {
            let id = u32::try_from(id).map_err(|_| Error::TextureIdOverflow)?;
            Ok(id | 0x8000_0000)
        }
    }
}
