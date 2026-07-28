use std::cell::RefCell;

use egui::{Color32, Context, FontId, Key, Pos2, RawInput, Rect, RichText, Vec2};

const LINUX_KEY_UP: u32 = 103;
const LINUX_KEY_LEFT: u32 = 105;
const LINUX_KEY_RIGHT: u32 = 106;
const LINUX_KEY_DOWN: u32 = 108;
const LINUX_KEY_OK: u32 = 139;
const LINUX_KEY_BACK: u32 = 158;
const LINUX_KEY_OK_ALT: u32 = 352;
const LINUX_KEY_BACK_ALT: u32 = 357;

const ACTION_PRESSED: u32 = 1;

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Home,
    Apps,
}

struct AppEntry {
    name: &'static str,
    symbol: &'static str,
    color: Color32,
}

const APPS: [AppEntry; 6] = [
    AppEntry {
        name: "Phone",
        symbol: "P",
        color: Color32::from_rgb(33, 163, 102),
    },
    AppEntry {
        name: "Messages",
        symbol: "M",
        color: Color32::from_rgb(39, 119, 211),
    },
    AppEntry {
        name: "Contacts",
        symbol: "C",
        color: Color32::from_rgb(128, 90, 213),
    },
    AppEntry {
        name: "Camera",
        symbol: "C",
        color: Color32::from_rgb(220, 90, 69),
    },
    AppEntry {
        name: "Browser",
        symbol: "B",
        color: Color32::from_rgb(21, 151, 165),
    },
    AppEntry {
        name: "Settings",
        symbol: "S",
        color: Color32::from_rgb(104, 114, 125),
    },
];

struct Launcher {
    context: Context,
    view: View,
    selected: usize,
    notice_until_us: u64,
    notice: Option<&'static str>,
    pending_events: Vec<egui::Event>,
}

impl Launcher {
    fn new() -> Self {
        let context = Context::default();
        context.set_visuals(egui::Visuals::dark());
        Self {
            context,
            view: View::Home,
            selected: 0,
            notice_until_us: 0,
            notice: None,
            pending_events: Vec::new(),
        }
    }

    fn key(&mut self, code: u32, action: u32, now_us: u64) {
        if action != ACTION_PRESSED {
            return;
        }
        let egui_key = match code {
            LINUX_KEY_UP => Some(Key::ArrowUp),
            LINUX_KEY_DOWN => Some(Key::ArrowDown),
            LINUX_KEY_LEFT => Some(Key::ArrowLeft),
            LINUX_KEY_RIGHT => Some(Key::ArrowRight),
            LINUX_KEY_OK | LINUX_KEY_OK_ALT => Some(Key::Enter),
            LINUX_KEY_BACK | LINUX_KEY_BACK_ALT => Some(Key::Escape),
            _ => None,
        };
        if let Some(key) = egui_key {
            self.pending_events.push(egui::Event::Key {
                key,
                physical_key: Some(key),
                pressed: true,
                repeat: false,
                modifiers: egui::Modifiers::NONE,
            });
        }

        match (self.view, code) {
            (View::Home, LINUX_KEY_OK | LINUX_KEY_OK_ALT) => self.view = View::Apps,
            (View::Apps, LINUX_KEY_BACK | LINUX_KEY_BACK_ALT) => {
                self.view = View::Home;
                self.notice = None;
            }
            (View::Apps, LINUX_KEY_LEFT) => {
                self.selected = (self.selected + APPS.len() - 1) % APPS.len()
            }
            (View::Apps, LINUX_KEY_RIGHT) => self.selected = (self.selected + 1) % APPS.len(),
            (View::Apps, LINUX_KEY_UP) => {
                self.selected = (self.selected + APPS.len() - 3) % APPS.len()
            }
            (View::Apps, LINUX_KEY_DOWN) => self.selected = (self.selected + 3) % APPS.len(),
            (View::Apps, LINUX_KEY_OK | LINUX_KEY_OK_ALT) => {
                self.notice = Some("App is not installed");
                self.notice_until_us = now_us + 1_400_000;
            }
            _ => {}
        }
    }

    fn frame(&mut self, now_us: u64) -> Result<(), &'static str> {
        if self.notice.is_some() && now_us >= self.notice_until_us {
            self.notice = None;
        }
        let [width, height] = oos_app::surface_size();
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(
                Pos2::ZERO,
                Vec2::new(width as f32, height as f32),
            )),
            time: Some(now_us as f64 / 1_000_000.0),
            events: std::mem::take(&mut self.pending_events),
            ..Default::default()
        };

        let view = self.view;
        let selected = self.selected;
        let notice = self.notice;
        let output = self.context.run_ui(input, |ui| {
            render_launcher(ui, view, selected, notice);
        });
        oos_egui::submit(&self.context, output, [16, 18, 22, 255]).map_err(|error| error.message())
    }
}

fn render_launcher(ui: &mut egui::Ui, view: View, selected: usize, notice: Option<&str>) {
    egui::Panel::top("status")
        .exact_size(20.0)
        .frame(
            egui::Frame::new()
                .fill(Color32::from_rgb(8, 9, 11))
                .inner_margin(egui::Margin::symmetric(7, 2)),
        )
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(RichText::new(format_time()).size(10.0).strong());
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label(
                        RichText::new("BAT  SIG  WIFI")
                            .size(8.0)
                            .color(Color32::LIGHT_GRAY),
                    );
                });
            });
        });
    egui::Panel::bottom("softkeys")
        .exact_size(28.0)
        .frame(
            egui::Frame::new()
                .fill(Color32::from_rgb(243, 244, 246))
                .inner_margin(egui::Margin::symmetric(5, 5)),
        )
        .show(ui, |ui| {
            let labels = if view == View::Home {
                ["Alerts", "Apps", "Camera"]
            } else {
                ["Back", "Open", "Options"]
            };
            ui.columns(3, |columns| {
                columns[0].label(
                    RichText::new(labels[0])
                        .size(10.0)
                        .strong()
                        .color(Color32::from_rgb(21, 23, 26)),
                );
                columns[1].with_layout(egui::Layout::top_down(egui::Align::Center), |ui| {
                    ui.label(
                        RichText::new(labels[1])
                            .size(10.0)
                            .strong()
                            .color(Color32::from_rgb(212, 92, 0)),
                    );
                });
                columns[2].with_layout(egui::Layout::top_down(egui::Align::RIGHT), |ui| {
                    ui.label(
                        RichText::new(labels[2])
                            .size(10.0)
                            .strong()
                            .color(Color32::from_rgb(21, 23, 26)),
                    );
                });
            });
        });
    egui::CentralPanel::default()
        .frame(
            egui::Frame::new()
                .fill(Color32::from_rgb(17, 20, 26))
                .inner_margin(egui::Margin::same(9)),
        )
        .show(ui, |ui| {
            match view {
                View::Home => render_home(ui),
                View::Apps => render_apps(ui, selected),
            }
            if let Some(message) = notice {
                let rect = Rect::from_min_max(Pos2::new(8.0, 257.0), Pos2::new(232.0, 284.0));
                ui.painter()
                    .rect_filled(rect, 0.0, Color32::from_black_alpha(235));
                ui.painter().rect_filled(
                    Rect::from_min_size(rect.min, Vec2::new(3.0, rect.height())),
                    0.0,
                    Color32::from_rgb(255, 122, 0),
                );
                ui.painter().text(
                    rect.center(),
                    egui::Align2::CENTER_CENTER,
                    message,
                    FontId::proportional(11.0),
                    Color32::WHITE,
                );
            }
        });
}

fn render_home(ui: &mut egui::Ui) {
    ui.add_space(39.0);
    ui.vertical_centered(|ui| {
        let center = ui.cursor().center_top() + Vec2::new(0.0, 24.0);
        ui.painter().circle_stroke(
            center,
            20.0,
            egui::Stroke::new(3.0, Color32::from_rgb(255, 122, 0)),
        );
        ui.painter().line_segment(
            [
                center + Vec2::new(-13.0, 13.0),
                center + Vec2::new(13.0, -13.0),
            ],
            egui::Stroke::new(2.0, Color32::from_rgb(255, 122, 0)),
        );
        ui.add_space(54.0);
        ui.label(
            RichText::new(format_time())
                .size(46.0)
                .color(Color32::WHITE),
        );
        ui.label(
            RichText::new("Orange OS")
                .size(13.0)
                .color(Color32::from_rgb(199, 203, 210)),
        );
        ui.add_space(52.0);
        ui.label(
            RichText::new("Orange OS   Ready")
                .size(10.0)
                .color(Color32::from_rgb(174, 179, 188)),
        );
    });
}

fn render_apps(ui: &mut egui::Ui, selected: usize) {
    ui.horizontal(|ui| {
        ui.label(
            RichText::new("O")
                .size(16.0)
                .strong()
                .color(Color32::from_rgb(255, 122, 0)),
        );
        ui.label(RichText::new("Apps").size(16.0).strong());
    });
    ui.add_space(7.0);
    egui::Grid::new("apps_grid")
        .num_columns(3)
        .spacing([6.0, 7.0])
        .show(ui, |ui| {
            for (index, app) in APPS.iter().enumerate() {
                let fill = if index == selected {
                    Color32::from_rgb(42, 46, 53)
                } else {
                    Color32::from_rgb(32, 36, 43)
                };
                let stroke = if index == selected {
                    egui::Stroke::new(2.0, Color32::from_rgb(255, 138, 31))
                } else {
                    egui::Stroke::NONE
                };
                ui.allocate_ui_with_layout(
                    Vec2::new(68.0, 78.0),
                    egui::Layout::top_down(egui::Align::Center),
                    |ui| {
                        let rect = ui.max_rect();
                        ui.painter()
                            .rect(rect, 4.0, fill, stroke, egui::StrokeKind::Inside);
                        let icon_rect = Rect::from_center_size(
                            rect.center_top() + Vec2::new(0.0, 28.0),
                            Vec2::splat(36.0),
                        );
                        ui.painter().rect_filled(icon_rect, 6.0, app.color);
                        ui.painter().text(
                            icon_rect.center(),
                            egui::Align2::CENTER_CENTER,
                            app.symbol,
                            FontId::proportional(19.0),
                            Color32::WHITE,
                        );
                        ui.add_space(53.0);
                        ui.label(RichText::new(app.name).size(9.0).strong());
                    },
                );
                if index % 3 == 2 {
                    ui.end_row();
                }
            }
        });
}

fn format_time() -> String {
    let minutes = oos_app::wall_clock_minutes() % (24 * 60);
    format!("{:02}:{:02}", minutes / 60, minutes % 60)
}

thread_local! {
    static LAUNCHER: RefCell<Option<Launcher>> = const { RefCell::new(None) };
}

struct App;

impl oos_app::App for App {
    fn init() -> Result<(), oos_app::ErrorCode> {
        if oos_app::abi_version() != oos_app::ABI_VERSION {
            return Err(oos_app::ErrorCode::Failed);
        }
        LAUNCHER.with(|launcher| *launcher.borrow_mut() = Some(Launcher::new()));
        oos_app::log(1, "egui launcher initialized");
        Ok(())
    }

    fn event(event: oos_app::KeyEvent) {
        let action = match event.action {
            oos_app::KeyAction::Released => 0,
            oos_app::KeyAction::Pressed => 1,
            oos_app::KeyAction::Repeated => 2,
        };
        LAUNCHER.with(|launcher| {
            if let Some(launcher) = launcher.borrow_mut().as_mut() {
                launcher.key(event.code, action, event.monotonic_time_us);
            }
        });
    }

    fn frame(monotonic_time_us: u64) -> Result<(), oos_app::ErrorCode> {
        LAUNCHER.with(|launcher| {
            launcher
                .borrow_mut()
                .as_mut()
                .ok_or("launcher is not initialized")
                .and_then(|launcher| launcher.frame(monotonic_time_us))
                .map_err(|message| {
                    oos_app::log(3, message);
                    oos_app::ErrorCode::Failed
                })
        })
    }

    fn shutdown() {
        LAUNCHER.with(|launcher| *launcher.borrow_mut() = None);
    }
}

oos_app::bindings::export!(App with_types_in oos_app::bindings);
