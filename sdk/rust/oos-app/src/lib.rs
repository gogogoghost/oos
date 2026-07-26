#![forbid(unsafe_op_in_unsafe_fn)]

pub const ABI_VERSION: u32 = 1;
pub const MAX_TEXTURE_SIZE: usize = 2_048;
pub const MAX_TEXTURE_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_VERTICES: usize = 65_535;
pub const MAX_INDICES: usize = 196_605;
pub const MAX_DRAW_COMMANDS: usize = 4_096;

pub const TEXTURE_LINEAR: u32 = 1 << 0;
pub const TEXTURE_REPLACE: u32 = 1 << 1;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct GfxVertex {
    pub position: [f32; 2],
    pub uv: [f32; 2],
    pub color: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct GfxDrawCommand {
    pub first_index: u32,
    pub index_count: u32,
    pub texture: u32,
    pub clip_min: [f32; 2],
    pub clip_max: [f32; 2],
}

#[link(wasm_import_module = "oos")]
extern "C" {
    fn oos_abi_version() -> u32;
    fn oos_surface_width() -> u32;
    fn oos_surface_height() -> u32;
    fn oos_wall_clock_minutes() -> u32;
    fn oos_log(level: u32, data: *const u8, len: u32);
    fn oos_gfx_texture_set(
        texture: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        flags: u32,
        rgba: *const u8,
        rgba_len: u32,
    ) -> i32;
    fn oos_gfx_texture_free(texture: u32) -> i32;
    fn oos_gfx_submit(
        vertices: *const GfxVertex,
        vertex_count: u32,
        indices: *const u16,
        index_count: u32,
        commands: *const GfxDrawCommand,
        command_count: u32,
        clear_rgba: u32,
    ) -> i32;
}

pub fn abi_version() -> u32 {
    unsafe { oos_abi_version() }
}

pub fn surface_size() -> [u32; 2] {
    unsafe { [oos_surface_width(), oos_surface_height()] }
}

pub fn wall_clock_minutes() -> u32 {
    unsafe { oos_wall_clock_minutes() }
}

pub fn log(level: u32, message: &str) {
    unsafe { oos_log(level, message.as_ptr(), message.len() as u32) }
}

pub fn texture_set(
    texture: u32,
    position: [u32; 2],
    size: [u32; 2],
    linear: bool,
    replace: bool,
    rgba: &[u8],
) -> Result<(), i32> {
    if size[0] as usize > MAX_TEXTURE_SIZE
        || size[1] as usize > MAX_TEXTURE_SIZE
        || rgba.len() > MAX_TEXTURE_BYTES
        || rgba.len() != size[0] as usize * size[1] as usize * 4
    {
        return Err(-1);
    }
    let result = unsafe {
        oos_gfx_texture_set(
            texture,
            position[0],
            position[1],
            size[0],
            size[1],
            (if linear { TEXTURE_LINEAR } else { 0 }) | (if replace { TEXTURE_REPLACE } else { 0 }),
            rgba.as_ptr(),
            rgba.len() as u32,
        )
    };
    (result == 0).then_some(()).ok_or(result)
}

pub fn texture_free(texture: u32) -> Result<(), i32> {
    let result = unsafe { oos_gfx_texture_free(texture) };
    (result == 0).then_some(()).ok_or(result)
}

pub fn submit(
    vertices: &[GfxVertex],
    indices: &[u16],
    commands: &[GfxDrawCommand],
    clear_rgba: [u8; 4],
) -> Result<(), i32> {
    if vertices.len() > MAX_VERTICES
        || indices.len() > MAX_INDICES
        || commands.len() > MAX_DRAW_COMMANDS
    {
        return Err(-1);
    }
    let clear = u32::from_le_bytes(clear_rgba);
    let result = unsafe {
        oos_gfx_submit(
            vertices.as_ptr(),
            vertices.len() as u32,
            indices.as_ptr(),
            indices.len() as u32,
            commands.as_ptr(),
            commands.len() as u32,
            clear,
        )
    };
    (result == 0).then_some(()).ok_or(result)
}

const _: () = assert!(core::mem::size_of::<GfxVertex>() == 20);
const _: () = assert!(core::mem::size_of::<GfxDrawCommand>() == 28);
