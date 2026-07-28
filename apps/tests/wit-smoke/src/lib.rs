use oos_app::bindings::oos::platform::{
    audio, bluetooth, camera, codec, device, graphics, ip, modem, power, runtime, vibrator, wifi,
};
use oos_app::{App as AppLifecycle, ErrorCode, KeyEvent};

struct App;

fn unavailable<T>(result: Result<T, ErrorCode>) {
    assert!(matches!(result, Err(ErrorCode::Unavailable)));
}

fn mocked() {
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
    assert_eq!(wifi::scan(1).unwrap().len(), 1);
    assert_eq!(wifi::list_networks().unwrap().len(), 1);
    assert_eq!(wifi::connect("ssid", wifi::Security::Open, "").unwrap(), 1);
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

impl AppLifecycle for App {
    fn init() -> Result<(), ErrorCode> {
        assert_eq!(runtime::abi_version(), oos_app::ABI_VERSION);
        let limits = graphics::graphics_limits();
        assert_eq!(limits.max_texture_size, oos_app::MAX_TEXTURE_SIZE as u32);
        let descriptor = device::get_descriptor();
        if descriptor.id == "local" {
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
