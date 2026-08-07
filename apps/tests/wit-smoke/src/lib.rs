use oos_app::bindings::oos::platform::{
    assets, audio, bluetooth, camera, codec, device, graphics, ip, modem, power, runtime,
    system_services, vibrator, wifi,
};
use oos_app::{
    gles2, App as AppLifecycle, ErrorCode, FontRole, KeyEvent, ShaderStage, TextureFormat,
    VertexType,
};

struct App;

fn unavailable<T>(result: Result<T, ErrorCode>) {
    assert!(matches!(result, Err(ErrorCode::Unavailable)));
}

fn permission_denied<T>(result: Result<T, ErrorCode>) {
    assert!(matches!(result, Err(ErrorCode::PermissionDenied)));
}

fn graphics_smoke() {
    assert_eq!(oos_app::pixels_per_point(), 1.0);
    assert_ne!(
        oos_app::supported_texture_formats() & (1 << TextureFormat::Rgb565 as u32),
        0
    );
    let rgb565 = [0x00, 0xf8, 0, 0, 0xaa, 0xbb, 0xe0, 0x07, 0x1f, 0x00];
    oos_app::texture_set(
        99,
        TextureFormat::Rgb565,
        [0, 0],
        [2, 2],
        6,
        oos_app::TextureFlags::REPLACE
            | oos_app::TextureFlags::LINEAR_MINIFICATION
            | oos_app::TextureFlags::LINEAR_MAGNIFICATION,
        &rgb565,
    )
    .unwrap();
    oos_app::texture_free(99).unwrap();

    let capabilities = gles2::get_capabilities();
    assert!(capabilities.depth_bits >= 16);
    assert!(capabilities.stencil_bits >= 8);

    let vertex_shader =
        "attribute vec2 aPosition; void main() { gl_Position = vec4(aPosition, 0.0, 1.0); }";
    let fragment_shader =
        "precision mediump float; void main() { gl_FragColor = vec4(0.1, 0.8, 0.2, 1.0); }";
    gles2::shader_set(1, ShaderStage::Vertex, vertex_shader).unwrap();
    gles2::shader_set(2, ShaderStage::Fragment, fragment_shader).unwrap();
    gles2::program_set(3, 1, 2).unwrap();
    let position = gles2::attribute_location(3, "aPosition");
    assert!(position >= 0);

    let vertices = [-0.8f32, -0.8, 0.8, -0.8, 0.0, 0.8];
    let mut vertex_bytes = Vec::with_capacity(vertices.len() * 4);
    for value in vertices {
        vertex_bytes.extend_from_slice(&value.to_bits().to_le_bytes());
    }
    gles2::buffer_set(
        4,
        vertex_bytes.len() as u32,
        oos_app::BufferUsage::StaticDraw,
        &vertex_bytes,
    )
    .unwrap();
    let commands = [
        gles2::begin_frame(gles2::CLEAR_COLOR | gles2::CLEAR_DEPTH, [4, 6, 8, 255], 1.0),
        gles2::depth(true, true, oos_app::CompareFunction::Less),
        gles2::use_program(3),
        gles2::bind_vertex_buffer(4),
        gles2::vertex_attribute(position as u32, 2, VertexType::Float32, false, 8, 0, true),
        gles2::draw_arrays(oos_app::Primitive::Triangles, 0, 3),
        gles2::end_frame(),
    ];
    gles2::submit(&commands, &[]).unwrap();
    gles2::buffer_free(4).unwrap();
    gles2::program_free(3).unwrap();
    gles2::shader_free(2).unwrap();
    gles2::shader_free(1).unwrap();
}

fn mocked() {
    let asset = assets::open("test.dat").unwrap();
    assert_eq!(asset.size, 10);
    assert_eq!(assets::read(asset.handle, 5, 3).unwrap(), b"ass");
    assets::close(asset.handle).unwrap();
    let formats = audio::supported_formats();
    assert!(formats.iter().any(|format| format.mime_type == "audio/wav"));
    assert!(formats
        .iter()
        .any(|format| format.mime_type == "audio/mpeg"));
    assert!(formats
        .iter()
        .any(|format| format.mime_type == "audio/flac"));
    let pcm_capabilities = audio::get_pcm_capabilities();
    assert_eq!(pcm_capabilities.minimum_sample_rate, 8_000);
    assert_eq!(pcm_capabilities.maximum_sample_rate, 48_000);
    assert_eq!(pcm_capabilities.supported_channel_mask, 0x3);
    assert!(pcm_capabilities.minimum_capacity_frames <= 512);
    assert!(pcm_capabilities.maximum_capacity_frames >= 512);
    let pcm = audio::pcm_open(16_000, 1, 512, audio::Usage::Media).unwrap();
    assert_eq!(pcm.audio_stream.sample_rate, 16_000);
    assert_eq!(audio::pcm_write(pcm.handle, &[0_i16; 64]).unwrap(), 64);
    audio::pcm_set_volume(pcm.handle, 0.5).unwrap();
    audio::pcm_pause(pcm.handle).unwrap();
    assert!(audio::pcm_status(pcm.handle).unwrap().paused);
    audio::pcm_flush(pcm.handle).unwrap();
    audio::pcm_resume(pcm.handle).unwrap();
    audio::pcm_close(pcm.handle).unwrap();
    let player = audio::player_open_asset("test.dat", audio::Usage::Media).unwrap();
    audio::player_set_volume(player, 0.75).unwrap();
    audio::player_set_looping(player, false).unwrap();
    audio::player_play(player).unwrap();
    let _ = audio::player_status(player).unwrap();
    audio::player_pause(player).unwrap();
    audio::player_close(player).unwrap();
    let midi = [
        b'M', b'T', b'h', b'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, b'M', b'T', b'r', b'k', 0, 0, 0, 15,
        0, 0xc0, 0, 0, 0x90, 60, 100, 96, 0x80, 60, 64, 0, 0xff, 0x2f, 0,
    ];
    let source_limits = audio::get_source_limits();
    assert!(source_limits.maximum_sources >= 1);
    assert!(source_limits.maximum_source_bytes >= midi.len() as u64);
    let source = audio::source_create(&midi, "application/octet-stream", "effect.bin").unwrap();
    let dynamic_player = audio::player_open_source(source, audio::Usage::Media).unwrap();
    audio::source_close(source).unwrap();
    audio::player_play(dynamic_player).unwrap();
    let dynamic_status = audio::player_status(dynamic_player).unwrap();
    assert!(!matches!(
        dynamic_status.failure,
        audio::MediaFailure::UnsupportedFormat
    ));
    audio::player_close(dynamic_player).unwrap();
    let tone = audio::play_tone(440.0, 10, 0.1, audio::Usage::Media).unwrap();
    assert_eq!(tone.sample_rate, 48_000);
    assert!(audio::record_wav("recording.wav", 10)
        .unwrap()
        .path
        .contains("local"));
    assert_eq!(audio::last_error(), "mock service ready");

    let cameras = camera::enumerate().unwrap();
    assert_eq!(cameras[0].id, "mock-camera-0");
    camera::set_torch(&cameras[0].id, true).unwrap();
    let photo = camera::capture_jpeg(&cameras[0].id, "photo.jpg", 320, 240, false, 1).unwrap();
    assert_eq!((photo.width, photo.height), (320, 240));

    let battery = power::query_battery().unwrap();
    assert_eq!(battery.capacity_percent, 82);
    assert!(power::wait_for_battery_event(1).unwrap().is_none());
    power::set_interactive(true).unwrap();
    power::acquire_wake_lock("smoke").unwrap();
    power::release_wake_lock("smoke").unwrap();
    power::enable_auto_suspend().unwrap();
    power::disable_auto_suspend().unwrap();
    power::schedule_rtc_wake(1).unwrap();
    power::clear_rtc_wake().unwrap();
    power::suspend(1).unwrap();
    assert!(matches!(
        power::query_flip_state().unwrap(),
        power::FlipState::Open
    ));

    vibrator::vibrate(1).unwrap();
    vibrator::stop().unwrap();
    assert!(vibrator::supports_amplitude_control());
    vibrator::set_amplitude(64).unwrap();

    let status = wifi::get_status().unwrap();
    assert_eq!(status.ssid, "OOS Mock Network");
    assert_eq!(wifi::scan(1).unwrap().len(), 3);
    assert_eq!(wifi::list_networks().unwrap().len(), 1);
    assert_eq!(wifi::connect("ssid", wifi::Security::Open, "").unwrap(), 2);
    wifi::disconnect().unwrap();
    wifi::reconnect().unwrap();
    wifi::forget(1).unwrap();
    wifi::save_configuration().unwrap();

    let configuration = ip::get_status().unwrap();
    assert_eq!(configuration.interface_name, "wlan0");
    ip::use_dhcp(1).unwrap();
    ip::use_static(&configuration).unwrap();

    bluetooth::enable(1).unwrap();
    let devices = bluetooth::classic_scan(1).unwrap();
    assert_eq!(devices[0].name, "OOS Mock Headset");
    assert_eq!(bluetooth::le_scan(1).unwrap().len(), 1);
    let address = &devices[0].address;
    bluetooth::pair(address, bluetooth::Transport::Auto).unwrap();
    bluetooth::profile_connect(address, bluetooth::Profile::Hid).unwrap();
    bluetooth::profile_disconnect(address, bluetooth::Profile::Hid).unwrap();
    bluetooth::profile_connection_cycle(address, bluetooth::Profile::Hid, 1).unwrap();
    bluetooth::le_connection_cycle(address, 1, 1).unwrap();
    bluetooth::cancel_pairing(address).unwrap();
    bluetooth::unpair(address).unwrap();
    bluetooth::disable(1).unwrap();

    let snapshot = modem::query_snapshot(1).unwrap();
    assert_eq!(snapshot.baseband_version, "OOS-MOCK-1.0");
    assert_eq!(snapshot.network_operator.short_name, "OOS");
    assert_eq!(modem::set_radio_power(true, 1).unwrap().error, 0);

    let codec = codec::test_h264_round_trip(16, 16, 2, 1).unwrap();
    assert_eq!(codec.decoded_frames, 2);
    assert_eq!(codec.encoder_name, "mock.h264.encoder");
}

fn permission_filtered() {
    assert_eq!(
        device::get_access(device::Feature::Wifi),
        device::AccessState::Denied
    );
    audio::play_tone(440.0, 1, 0.1, audio::Usage::Media).unwrap();
    permission_denied(audio::record_wav("recording.wav", 1));

    permission_denied(camera::enumerate());
    permission_denied(camera::set_torch("0", false));
    assert_eq!(camera::last_error(), "permission denied");

    assert_eq!(power::query_battery().unwrap().capacity_percent, 82);
    permission_denied(power::set_interactive(true));
    permission_denied(power::acquire_wake_lock("smoke"));
    permission_denied(power::query_flip_state());

    vibrator::vibrate(1).unwrap();
    vibrator::stop().unwrap();
    codec::test_h264_round_trip(16, 16, 1, 1).unwrap();

    permission_denied(wifi::get_status());
    assert_eq!(wifi::last_error(), "permission denied");
    permission_denied(ip::get_status());
    permission_denied(bluetooth::enable(1));
    assert_eq!(bluetooth::last_error(), "permission denied");
    permission_denied(modem::query_snapshot(1));
    assert_eq!(modem::last_error(), "permission denied");
    permission_denied(oos_app::device_storage::enumerate(
        oos_app::DeviceStorageVolume::Internal,
    ));
    permission_denied(system_services::request("settings", "get", "{}"));
}

fn storage_read_only() {
    let volume = oos_app::DeviceStorageVolume::Internal;
    assert_eq!(
        device::get_access(device::Feature::PrimaryDisplay),
        device::AccessState::Granted
    );
    assert_eq!(
        device::get_access(device::Feature::Wifi),
        device::AccessState::Denied
    );
    oos_app::device_storage::enumerate(volume).unwrap();
    assert!(oos_app::device_storage::free_space(volume).unwrap() > 0);
    permission_denied(oos_app::device_storage::write(
        volume,
        "wit-smoke/read-only.bin",
        oos_app::DeviceStorageWriteMode::Create,
        b"blocked",
    ));
    permission_denied(oos_app::device_storage::write(
        volume,
        "wit-smoke/read-only.bin",
        oos_app::DeviceStorageWriteMode::Replace,
        b"blocked",
    ));
    permission_denied(oos_app::device_storage::delete(
        volume,
        "wit-smoke/read-only.bin",
    ));
}

impl AppLifecycle for App {
    fn init() -> Result<(), ErrorCode> {
        assert_eq!(runtime::abi_version(), oos_app::ABI_VERSION);
        runtime::set_status_bar_style(0x12_34_56, runtime::StatusBarIconTheme::Dark)?;
        runtime::set_surface_mode(runtime::SurfaceMode::Immersive)?;
        runtime::set_surface_mode(runtime::SurfaceMode::Normal)?;
        let font = oos_app::font_assets::load(FontRole::UiProportional).unwrap();
        assert!(font.starts_with(b"OTTO"));
        unavailable(oos_app::font_assets::load(FontRole::UiMonospace));
        unavailable(oos_app::font_assets::load(FontRole::Emoji));
        let limits = graphics::graphics_limits();
        assert_eq!(limits.max_texture_size, oos_app::MAX_TEXTURE_SIZE as u32);
        graphics_smoke();
        let descriptor = device::get_descriptor();
        if descriptor.id == "local-denied" {
            permission_filtered();
            return Ok(());
        }
        if descriptor.id == "local-storage-readonly" {
            storage_read_only();
            return Ok(());
        }
        if descriptor.id == "local" {
            oos_app::kv_set("smoke", b"persistent").unwrap();
            assert_eq!(
                oos_app::kv_get("smoke").unwrap(),
                Some(b"persistent".to_vec())
            );
            oos_app::kv_delete("smoke").unwrap();
            assert_eq!(oos_app::kv_get("smoke").unwrap(), None);
            runtime::log(runtime::LogLevel::Info, "storage kv passed");
            oos_app::sqlite::execute(
                "smoke",
                "CREATE TABLE IF NOT EXISTS sample(id INTEGER, name TEXT, payload BLOB, ratio REAL, absent_value TEXT)",
            )
            .unwrap();
            runtime::log(runtime::LogLevel::Info, "storage sqlite create passed");
            oos_app::sqlite::execute("smoke", "DELETE FROM sample").unwrap();
            let insert = oos_app::sqlite::Statement::prepare(
                "smoke",
                "INSERT INTO sample VALUES(?, ?, ?, ?, ?)",
            )
            .unwrap();
            insert.bind_integer(1, 7).unwrap();
            insert.bind_text(2, "orange").unwrap();
            insert.bind_blob(3, &[1, 2, 255]).unwrap();
            insert.bind_float(4, 1.5).unwrap();
            insert.bind_null(5).unwrap();
            assert!(matches!(insert.step().unwrap(), oos_app::SqlRowState::Done));
            insert.finish().unwrap();
            runtime::log(runtime::LogLevel::Info, "storage sqlite insert passed");
            let statement = oos_app::sqlite::Statement::prepare(
                "smoke",
                "SELECT id, name, payload, ratio, absent_value FROM sample",
            )
            .unwrap();
            runtime::log(runtime::LogLevel::Info, "storage sqlite prepare passed");
            assert!(matches!(
                statement.step().unwrap(),
                oos_app::SqlRowState::Row
            ));
            runtime::log(runtime::LogLevel::Info, "storage sqlite row passed");
            assert_eq!(statement.column_count().unwrap(), 5);
            runtime::log(runtime::LogLevel::Info, "storage sqlite count passed");
            assert_eq!(statement.integer(0).unwrap(), 7);
            runtime::log(runtime::LogLevel::Info, "storage sqlite integer passed");
            assert_eq!(statement.text(1).unwrap(), "orange");
            runtime::log(runtime::LogLevel::Info, "storage sqlite text passed");
            assert_eq!(statement.blob(2).unwrap(), [1, 2, 255]);
            runtime::log(runtime::LogLevel::Info, "storage sqlite blob passed");
            assert_eq!(statement.float(3).unwrap(), 1.5);
            assert!(matches!(
                statement.column_kind(4).unwrap(),
                oos_app::SqlValueKind::Null
            ));
            assert!(matches!(
                statement.step().unwrap(),
                oos_app::SqlRowState::Done
            ));
            statement.finish().unwrap();
            let volume = oos_app::DeviceStorageVolume::Internal;
            let path = "wit-smoke/device-storage.bin";
            oos_app::device_storage::write(
                volume,
                path,
                oos_app::DeviceStorageWriteMode::Replace,
                b"device",
            )
            .unwrap();
            oos_app::device_storage::write(
                volume,
                path,
                oos_app::DeviceStorageWriteMode::Append,
                b"-storage",
            )
            .unwrap();
            assert_eq!(
                oos_app::device_storage::read(volume, path).unwrap(),
                b"device-storage"
            );
            assert!(oos_app::device_storage::enumerate(volume)
                .unwrap()
                .iter()
                .any(|entry| entry.path == path));
            assert!(oos_app::device_storage::free_space(volume).unwrap() > 0);
            assert!(oos_app::device_storage::used_space(volume).unwrap() > 0);
            assert!(oos_app::device_storage::delete(volume, path).unwrap());
            assert!(!oos_app::device_storage::delete(volume, path).unwrap());
            runtime::log(runtime::LogLevel::Info, "device storage passed");
            assert_eq!(
                system_services::request(
                    "settings",
                    "set",
                    r#"{"name":"wit-smoke","value":"{\"ok\":true}"}"#,
                )
                .unwrap(),
                "null"
            );
            assert!(
                system_services::request("settings", "get", r#"{"name":"wit-smoke"}"#,)
                    .unwrap()
                    .contains(r#""ok":true"#)
            );
            runtime::log(runtime::LogLevel::Info, "system services passed");
            assert_eq!(
                (descriptor.primary_width, descriptor.primary_height),
                (240, 320)
            );
            assert!(matches!(
                device::get_capability(device::Feature::PrimaryDisplay),
                device::CapabilityState::Validated
            ));
            mocked();
            return Ok(());
        }
        assert_eq!(descriptor.primary_width, 0);
        assert!(matches!(
            device::get_capability(device::Feature::PrimaryDisplay),
            device::CapabilityState::Unsupported
        ));

        unavailable(assets::open("test.dat"));
        assert!(audio::supported_formats().len() >= 9);
        unavailable(audio::play_tone(440.0, 1, 0.1, audio::Usage::Media));
        unavailable(audio::record_wav("recording.wav", 1));
        assert_eq!(audio::last_error(), "service unavailable");

        unavailable(camera::enumerate());
        unavailable(camera::set_torch("0", false));
        unavailable(camera::capture_jpeg("0", "photo.jpg", 320, 240, false, 1));
        assert_eq!(camera::last_error(), "service unavailable");

        unavailable(power::query_battery());
        unavailable(power::wait_for_battery_event(1));
        unavailable(power::set_interactive(true));
        unavailable(power::acquire_wake_lock("smoke"));
        unavailable(power::release_wake_lock("smoke"));
        unavailable(power::enable_auto_suspend());
        unavailable(power::disable_auto_suspend());
        unavailable(power::schedule_rtc_wake(1));
        unavailable(power::clear_rtc_wake());
        unavailable(power::suspend(1));
        unavailable(power::query_flip_state());
        assert_eq!(power::last_error(), "service unavailable");

        unavailable(vibrator::vibrate(1));
        unavailable(vibrator::stop());
        assert!(!vibrator::supports_amplitude_control());
        unavailable(vibrator::set_amplitude(1));
        assert_eq!(vibrator::last_error(), "service unavailable");

        unavailable(wifi::get_status());
        unavailable(wifi::scan(1));
        unavailable(wifi::list_networks());
        unavailable(wifi::connect("ssid", wifi::Security::Open, ""));
        unavailable(wifi::disconnect());
        unavailable(wifi::reconnect());
        unavailable(wifi::forget(0));
        unavailable(wifi::save_configuration());
        assert_eq!(wifi::last_error(), "service unavailable");

        unavailable(ip::get_status());
        unavailable(ip::use_dhcp(1));
        unavailable(ip::use_static(&ip::Configuration {
            interface_name: "wlan0".into(),
            address: "192.0.2.2".into(),
            prefix_length: 24,
            gateway: "192.0.2.1".into(),
            dns1: "192.0.2.1".into(),
            dns2: String::new(),
        }));
        assert_eq!(ip::last_error(), "service unavailable");

        unavailable(bluetooth::enable(1));
        unavailable(bluetooth::disable(1));
        unavailable(bluetooth::classic_scan(1));
        unavailable(bluetooth::le_scan(1));
        unavailable(bluetooth::pair(
            "00:00:00:00:00:00",
            bluetooth::Transport::Auto,
        ));
        unavailable(bluetooth::unpair("00:00:00:00:00:00"));
        unavailable(bluetooth::cancel_pairing("00:00:00:00:00:00"));
        unavailable(bluetooth::profile_connect(
            "00:00:00:00:00:00",
            bluetooth::Profile::Hid,
        ));
        unavailable(bluetooth::profile_disconnect(
            "00:00:00:00:00:00",
            bluetooth::Profile::Hid,
        ));
        unavailable(bluetooth::profile_connection_cycle(
            "00:00:00:00:00:00",
            bluetooth::Profile::Hid,
            1,
        ));
        unavailable(bluetooth::le_connection_cycle("00:00:00:00:00:00", 1, 1));
        assert_eq!(bluetooth::last_error(), "service unavailable");

        unavailable(modem::query_snapshot(1));
        unavailable(modem::set_radio_power(false, 1));
        assert_eq!(modem::last_error(), "service unavailable");

        unavailable(codec::test_h264_round_trip(16, 16, 1, 1));
        assert_eq!(codec::last_error(), "service unavailable");
        Ok(())
    }

    fn event(_: KeyEvent) {}

    fn frame(_: u64) -> Result<u32, ErrorCode> {
        Ok(1000)
    }

    fn shutdown() {}
}

oos_app::bindings::export!(App with_types_in oos_app::bindings);
