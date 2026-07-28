pub const ABI_VERSION: u32 = 1;
pub const MAX_TEXTURE_SIZE: usize = 2_048;
pub const MAX_TEXTURE_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_VERTICES: usize = 65_535;
pub const MAX_INDICES: usize = 196_605;
pub const MAX_DRAW_COMMANDS: usize = 4_096;

pub mod bindings {
    wit_bindgen::generate!({
        world: "app",
        path: "../../../wit",
        pub_export_macro: true,
    });
}

pub use bindings::exports::oos::platform::lifecycle::{Guest as App, KeyAction, KeyEvent};
pub use bindings::oos::platform::graphics::{
    DrawCommand as GfxDrawCommand, TextureFlags, Vertex as GfxVertex,
};
pub use bindings::oos::platform::types::ErrorCode;

use bindings::oos::platform::{graphics, runtime};

pub fn abi_version() -> u32 {
    runtime::abi_version()
}

pub fn surface_size() -> [u32; 2] {
    let size = graphics::surface_size();
    [size.width, size.height]
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

pub fn texture_set(
    texture: u32,
    position: [u32; 2],
    size: [u32; 2],
    linear: bool,
    replace: bool,
    rgba: &[u8],
) -> Result<(), ErrorCode> {
    if size[0] as usize > MAX_TEXTURE_SIZE
        || size[1] as usize > MAX_TEXTURE_SIZE
        || rgba.len() > MAX_TEXTURE_BYTES
        || rgba.len() != size[0] as usize * size[1] as usize * 4
    {
        return Err(ErrorCode::InvalidArgument);
    }
    let mut options = TextureFlags::empty();
    if linear {
        options |= TextureFlags::LINEAR;
    }
    if replace {
        options |= TextureFlags::REPLACE;
    }
    graphics::texture_set(
        texture,
        graphics::Point {
            x: position[0],
            y: position[1],
        },
        graphics::Size {
            width: size[0],
            height: size[1],
        },
        options,
        rgba,
    )
}

pub fn texture_free(texture: u32) -> Result<(), ErrorCode> {
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
