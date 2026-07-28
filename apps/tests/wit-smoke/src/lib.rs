use oos_app::bindings::oos::platform::{
    audio, bluetooth, camera, codec, device, graphics, ip, modem, power, runtime, vibrator, wifi,
};
use oos_app::{App as AppLifecycle, ErrorCode, KeyEvent};

struct App;

fn unavailable<T>(result: Result<T, ErrorCode>) {
    assert!(matches!(result, Err(ErrorCode::Unavailable)));
}

impl AppLifecycle for App {
    fn init() -> Result<(), ErrorCode> {
        assert_eq!(runtime::abi_version(), oos_app::ABI_VERSION);
        let limits = graphics::graphics_limits();
        assert_eq!(limits.max_texture_size, oos_app::MAX_TEXTURE_SIZE as u32);
        let descriptor = device::get_descriptor();
        assert_eq!(descriptor.primary_width, 0);
        assert!(matches!(
            device::get_capability(device::Feature::PrimaryDisplay),
            device::CapabilityState::Unsupported
        ));

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

    fn frame(_: u64) -> Result<(), ErrorCode> {
        Ok(())
    }

    fn shutdown() {}
}

oos_app::bindings::export!(App with_types_in oos_app::bindings);
