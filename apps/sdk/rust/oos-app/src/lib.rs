pub const ABI_VERSION: u32 = 2;
pub const MAX_TEXTURE_SIZE: usize = 2_048;
pub const MAX_TEXTURE_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_VERTICES: usize = 65_535;
pub const MAX_INDICES: usize = 196_605;
pub const MAX_DRAW_COMMANDS: usize = 4_096;
pub const MAX_GLES_BUFFER_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_GLES_SHADER_BYTES: usize = 64 * 1024;
pub const MAX_GLES_COMMAND_DATA_WORDS: usize = 16 * 1024;

pub mod bindings {
    wit_bindgen::generate!({
        world: "app",
        path: "../../wit",
        pub_export_macro: true,
    });
}

pub use bindings::exports::oos::platform::lifecycle::{Guest as App, KeyAction, KeyEvent};
pub use bindings::oos::platform::gles::{
    BlendEquation, BlendFactor, BufferUsage, Capabilities as GlesCapabilities,
    Command as GlesCommand, CommandOpcode, CompareFunction, CullFace, FrontFace, Primitive,
    ShaderStage, StencilOperation, UniformType, VertexType,
};
pub use bindings::oos::platform::graphics::{
    DrawCommand as GfxDrawCommand, TextureFlags, TextureFormat, Vertex as GfxVertex,
};
pub use bindings::oos::platform::types::ErrorCode;

use bindings::oos::platform::{gles, graphics, runtime, storage};

pub fn abi_version() -> u32 {
    runtime::abi_version()
}

pub fn surface_size() -> [u32; 2] {
    let size = graphics::surface_size();
    [size.width, size.height]
}

pub fn surface_format() -> TextureFormat {
    graphics::surface_format()
}

pub fn supported_texture_formats() -> u32 {
    graphics::supported_texture_formats()
}

pub fn wall_clock_minutes() -> u32 {
    runtime::wall_clock_minutes()
}

pub fn log(level: u32, message: &str) {
    let level = match level {
        0 => runtime::LogLevel::Debug,
        2 => runtime::LogLevel::Warn,
        3.. => runtime::LogLevel::Error,
        _ => runtime::LogLevel::Info,
    };
    runtime::log(level, message)
}

pub fn kv_get(key: &str) -> Result<Option<Vec<u8>>, ErrorCode> {
    storage::kv_get(key)
}

pub fn kv_set(key: &str, value: &[u8]) -> Result<(), ErrorCode> {
    storage::kv_set(key, value)
}

pub fn kv_delete(key: &str) -> Result<(), ErrorCode> {
    storage::kv_delete(key)
}

pub fn kv_clear() -> Result<(), ErrorCode> {
    storage::kv_clear()
}

pub use bindings::oos::platform::storage::{RowState as SqlRowState, ValueKind as SqlValueKind};

pub mod sqlite {
    use super::{storage, ErrorCode, SqlRowState, SqlValueKind};

    pub fn execute(database: &str, sql: &str) -> Result<u32, ErrorCode> {
        storage::database_execute(database, sql)
    }

    pub struct Statement {
        handle: u32,
    }

    impl Statement {
        pub fn prepare(database: &str, sql: &str) -> Result<Self, ErrorCode> {
            storage::database_prepare(database, sql).map(|handle| Self { handle })
        }

        pub fn step(&self) -> Result<SqlRowState, ErrorCode> {
            storage::statement_step(self.handle)
        }

        pub fn bind_null(&self, index: u32) -> Result<(), ErrorCode> {
            storage::statement_bind_null(self.handle, index)
        }

        pub fn bind_integer(&self, index: u32, value: i64) -> Result<(), ErrorCode> {
            storage::statement_bind_integer(self.handle, index, value)
        }

        pub fn bind_float(&self, index: u32, value: f64) -> Result<(), ErrorCode> {
            storage::statement_bind_float(self.handle, index, value)
        }

        pub fn bind_text(&self, index: u32, value: &str) -> Result<(), ErrorCode> {
            storage::statement_bind_text(self.handle, index, value)
        }

        pub fn bind_blob(&self, index: u32, value: &[u8]) -> Result<(), ErrorCode> {
            storage::statement_bind_blob(self.handle, index, value)
        }

        pub fn column_count(&self) -> Result<u32, ErrorCode> {
            storage::statement_column_count(self.handle)
        }

        pub fn column_kind(&self, column: u32) -> Result<SqlValueKind, ErrorCode> {
            storage::statement_column_kind(self.handle, column)
        }

        pub fn integer(&self, column: u32) -> Result<i64, ErrorCode> {
            storage::statement_column_integer(self.handle, column)
        }

        pub fn float(&self, column: u32) -> Result<f64, ErrorCode> {
            storage::statement_column_float(self.handle, column)
        }

        pub fn text(&self, column: u32) -> Result<String, ErrorCode> {
            storage::statement_column_text(self.handle, column)
        }

        pub fn blob(&self, column: u32) -> Result<Vec<u8>, ErrorCode> {
            storage::statement_column_blob(self.handle, column)
        }

        pub fn finish(mut self) -> Result<(), ErrorCode> {
            let handle = core::mem::take(&mut self.handle);
            storage::statement_finish(handle)
        }
    }

    impl Drop for Statement {
        fn drop(&mut self) {
            if self.handle != 0 {
                let _ = storage::statement_finish(self.handle);
            }
        }
    }
}

pub fn texture_set(
    texture: u32,
    format: TextureFormat,
    position: [u32; 2],
    size: [u32; 2],
    row_stride: u32,
    options: TextureFlags,
    pixels: &[u8],
) -> Result<(), ErrorCode> {
    let bytes_per_pixel = match format {
        TextureFormat::A8 => 1usize,
        TextureFormat::Rgb565 | TextureFormat::Rgba4444 => 2,
        TextureFormat::Rgba8888 => 4,
    };
    let row_bytes = (size[0] as usize).checked_mul(bytes_per_pixel);
    let required_bytes = row_bytes.and_then(|row_bytes| {
        if row_stride as usize >= row_bytes && size[1] > 0 {
            (row_stride as usize)
                .checked_mul(size[1] as usize - 1)
                .and_then(|prefix| prefix.checked_add(row_bytes))
        } else {
            None
        }
    });
    if texture == 0
        || size[0] == 0
        || size[1] == 0
        || size[0] as usize > MAX_TEXTURE_SIZE
        || size[1] as usize > MAX_TEXTURE_SIZE
        || pixels.len() > MAX_TEXTURE_BYTES
        || required_bytes != Some(pixels.len())
    {
        return Err(ErrorCode::InvalidArgument);
    }
    graphics::texture_set(
        texture,
        format,
        graphics::Point {
            x: position[0],
            y: position[1],
        },
        graphics::Size {
            width: size[0],
            height: size[1],
        },
        row_stride,
        options,
        pixels,
    )
}

pub mod gles2 {
    use super::*;

    pub const CLEAR_COLOR: u32 = 1 << 0;
    pub const CLEAR_DEPTH: u32 = 1 << 1;
    pub const CLEAR_STENCIL: u32 = 1 << 2;

    pub fn command(opcode: CommandOpcode, args: [u32; 8]) -> GlesCommand {
        GlesCommand {
            opcode,
            a0: args[0],
            a1: args[1],
            a2: args[2],
            a3: args[3],
            a4: args[4],
            a5: args[5],
            a6: args[6],
            a7: args[7],
        }
    }

    pub fn begin_frame(clear_mask: u32, clear_rgba: [u8; 4], depth: f32) -> GlesCommand {
        command(
            CommandOpcode::BeginFrame,
            [
                clear_mask,
                u32::from_le_bytes(clear_rgba),
                depth.to_bits(),
                0,
                0,
                0,
                0,
                0,
            ],
        )
    }

    pub fn end_frame() -> GlesCommand {
        command(CommandOpcode::EndFrame, [0; 8])
    }

    pub fn viewport(x: i32, y: i32, width: u32, height: u32) -> GlesCommand {
        command(
            CommandOpcode::Viewport,
            [x as u32, y as u32, width, height, 0, 0, 0, 0],
        )
    }

    pub fn scissor(enabled: bool, x: i32, y: i32, width: u32, height: u32) -> GlesCommand {
        command(
            CommandOpcode::Scissor,
            [enabled as u32, x as u32, y as u32, width, height, 0, 0, 0],
        )
    }

    pub fn blend(
        enabled: bool,
        source_rgb: BlendFactor,
        destination_rgb: BlendFactor,
        source_alpha: BlendFactor,
        destination_alpha: BlendFactor,
        rgb_equation: BlendEquation,
        alpha_equation: BlendEquation,
        constant_rgba: [u8; 4],
    ) -> GlesCommand {
        command(
            CommandOpcode::Blend,
            [
                enabled as u32,
                source_rgb as u32,
                destination_rgb as u32,
                source_alpha as u32,
                destination_alpha as u32,
                rgb_equation as u32,
                alpha_equation as u32,
                u32::from_le_bytes(constant_rgba),
            ],
        )
    }

    pub fn depth(enabled: bool, write: bool, function: CompareFunction) -> GlesCommand {
        command(
            CommandOpcode::Depth,
            [enabled as u32, write as u32, function as u32, 0, 0, 0, 0, 0],
        )
    }

    pub fn color_mask(red: bool, green: bool, blue: bool, alpha: bool) -> GlesCommand {
        command(
            CommandOpcode::ColorMask,
            [
                red as u32,
                green as u32,
                blue as u32,
                alpha as u32,
                0,
                0,
                0,
                0,
            ],
        )
    }

    pub fn stencil(enabled: bool, front_write_mask: u32, back_write_mask: u32) -> GlesCommand {
        command(
            CommandOpcode::Stencil,
            [
                enabled as u32,
                front_write_mask,
                back_write_mask,
                0,
                0,
                0,
                0,
                0,
            ],
        )
    }

    pub fn stencil_function(
        face: CullFace,
        function: CompareFunction,
        reference: i32,
        mask: u32,
    ) -> GlesCommand {
        command(
            CommandOpcode::StencilFunction,
            [
                face as u32,
                function as u32,
                reference as u32,
                mask,
                0,
                0,
                0,
                0,
            ],
        )
    }

    pub fn stencil_operation(
        face: CullFace,
        stencil_fail: StencilOperation,
        depth_fail: StencilOperation,
        pass: StencilOperation,
    ) -> GlesCommand {
        command(
            CommandOpcode::StencilOperation,
            [
                face as u32,
                stencil_fail as u32,
                depth_fail as u32,
                pass as u32,
                0,
                0,
                0,
                0,
            ],
        )
    }

    pub fn raster(
        line_width: f32,
        polygon_offset: Option<[f32; 2]>,
        dither: bool,
        depth_range: [f32; 2],
    ) -> GlesCommand {
        let [factor, units] = polygon_offset.unwrap_or([0.0, 0.0]);
        command(
            CommandOpcode::Raster,
            [
                line_width.to_bits(),
                polygon_offset.is_some() as u32,
                factor.to_bits(),
                units.to_bits(),
                dither as u32,
                depth_range[0].to_bits(),
                depth_range[1].to_bits(),
                0,
            ],
        )
    }

    pub fn cull(enabled: bool, face: CullFace, front: FrontFace) -> GlesCommand {
        command(
            CommandOpcode::Cull,
            [enabled as u32, face as u32, front as u32, 0, 0, 0, 0, 0],
        )
    }

    pub fn use_program(program: u32) -> GlesCommand {
        command(CommandOpcode::UseProgram, [program, 0, 0, 0, 0, 0, 0, 0])
    }

    pub fn bind_texture(unit: u32, texture: u32) -> GlesCommand {
        command(
            CommandOpcode::BindTexture,
            [unit, texture, 0, 0, 0, 0, 0, 0],
        )
    }

    pub fn bind_vertex_buffer(buffer: u32) -> GlesCommand {
        command(
            CommandOpcode::BindVertexBuffer,
            [buffer, 0, 0, 0, 0, 0, 0, 0],
        )
    }

    pub fn bind_index_buffer(buffer: u32) -> GlesCommand {
        command(
            CommandOpcode::BindIndexBuffer,
            [buffer, 0, 0, 0, 0, 0, 0, 0],
        )
    }

    pub fn vertex_attribute(
        location: u32,
        components: u32,
        element_type: VertexType,
        normalized: bool,
        stride: u32,
        offset: u32,
        enabled: bool,
    ) -> GlesCommand {
        command(
            CommandOpcode::VertexAttribute,
            [
                location,
                components,
                element_type as u32,
                normalized as u32,
                stride,
                offset,
                enabled as u32,
                0,
            ],
        )
    }

    pub fn uniform(
        location: i32,
        value_type: UniformType,
        count: u32,
        word_offset: u32,
    ) -> GlesCommand {
        command(
            CommandOpcode::Uniform,
            [
                location as u32,
                value_type as u32,
                count,
                word_offset,
                0,
                0,
                0,
                0,
            ],
        )
    }

    pub fn draw_arrays(primitive: Primitive, first: u32, count: u32) -> GlesCommand {
        command(
            CommandOpcode::DrawArrays,
            [primitive as u32, first, count, 0, 0, 0, 0, 0],
        )
    }

    pub fn draw_elements(
        primitive: Primitive,
        count: u32,
        index_type: VertexType,
        offset: u32,
    ) -> GlesCommand {
        command(
            CommandOpcode::DrawElements,
            [
                primitive as u32,
                count,
                index_type as u32,
                offset,
                0,
                0,
                0,
                0,
            ],
        )
    }

    pub fn get_capabilities() -> GlesCapabilities {
        gles::get_capabilities()
    }

    pub fn buffer_set(
        buffer: u32,
        size: u32,
        usage: BufferUsage,
        initial_data: &[u8],
    ) -> Result<(), ErrorCode> {
        if buffer == 0
            || size == 0
            || size as usize > MAX_GLES_BUFFER_BYTES
            || (!initial_data.is_empty() && initial_data.len() != size as usize)
        {
            return Err(ErrorCode::InvalidArgument);
        }
        gles::buffer_set(buffer, size, usage, initial_data)
    }

    pub fn buffer_write(buffer: u32, offset: u32, data: &[u8]) -> Result<(), ErrorCode> {
        if buffer == 0
            || data.is_empty()
            || offset as usize > MAX_GLES_BUFFER_BYTES
            || data.len() > MAX_GLES_BUFFER_BYTES - offset as usize
        {
            return Err(ErrorCode::InvalidArgument);
        }
        gles::buffer_write(buffer, offset, data)
    }

    pub fn buffer_free(buffer: u32) -> Result<(), ErrorCode> {
        if buffer == 0 {
            return Err(ErrorCode::InvalidArgument);
        }
        gles::buffer_free(buffer)
    }

    pub fn shader_set(shader: u32, stage: ShaderStage, source: &str) -> Result<(), ErrorCode> {
        if shader == 0 || source.is_empty() || source.len() > MAX_GLES_SHADER_BYTES {
            return Err(ErrorCode::InvalidArgument);
        }
        gles::shader_set(shader, stage, source)
    }

    pub fn shader_free(shader: u32) -> Result<(), ErrorCode> {
        if shader == 0 {
            return Err(ErrorCode::InvalidArgument);
        }
        gles::shader_free(shader)
    }

    pub fn program_set(
        program: u32,
        vertex_shader: u32,
        fragment_shader: u32,
    ) -> Result<(), ErrorCode> {
        if program == 0 || vertex_shader == 0 || fragment_shader == 0 {
            return Err(ErrorCode::InvalidArgument);
        }
        gles::program_set(program, vertex_shader, fragment_shader)
    }

    pub fn program_free(program: u32) -> Result<(), ErrorCode> {
        if program == 0 {
            return Err(ErrorCode::InvalidArgument);
        }
        gles::program_free(program)
    }

    pub fn attribute_location(program: u32, name: &str) -> i32 {
        if program == 0 || name.is_empty() || name.len() > 255 {
            return -1;
        }
        gles::attribute_location(program, name)
    }

    pub fn uniform_location(program: u32, name: &str) -> i32 {
        if program == 0 || name.is_empty() || name.len() > 255 {
            return -1;
        }
        gles::uniform_location(program, name)
    }

    pub fn submit(commands: &[GlesCommand], data: &[u32]) -> Result<(), ErrorCode> {
        if commands.len() < 2 {
            return Err(ErrorCode::InvalidArgument);
        }
        if commands.len() > MAX_DRAW_COMMANDS || data.len() > MAX_GLES_COMMAND_DATA_WORDS {
            return Err(ErrorCode::LimitExceeded);
        }
        gles::submit(commands, data)
    }

    pub fn push_f32(data: &mut Vec<u32>, values: &[f32]) -> u32 {
        let offset = data.len() as u32;
        data.reserve(values.len());
        for value in values {
            data.push(value.to_bits());
        }
        offset
    }

    pub fn push_i32(data: &mut Vec<u32>, values: &[i32]) -> u32 {
        let offset = data.len() as u32;
        data.reserve(values.len());
        data.extend(values.iter().map(|value| *value as u32));
        offset
    }
}

pub fn texture_free(texture: u32) -> Result<(), ErrorCode> {
    if texture == 0 {
        return Err(ErrorCode::InvalidArgument);
    }
    graphics::texture_free(texture)
}

pub fn submit(
    vertices: &[GfxVertex],
    indices: &[u16],
    commands: &[GfxDrawCommand],
    clear_rgba: [u8; 4],
) -> Result<(), ErrorCode> {
    if vertices.len() > MAX_VERTICES
        || indices.len() > MAX_INDICES
        || commands.len() > MAX_DRAW_COMMANDS
    {
        return Err(ErrorCode::LimitExceeded);
    }
    graphics::submit(vertices, indices, commands, u32::from_le_bytes(clear_rgba))
}

const _: () = assert!(core::mem::size_of::<GfxVertex>() == 20);
const _: () = assert!(core::mem::size_of::<GfxDrawCommand>() == 28);
const _: () = assert!(core::mem::size_of::<GlesCommand>() == 36);
