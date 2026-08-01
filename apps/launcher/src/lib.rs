use std::cell::RefCell;

use egui::{Color32, Context, FontId, Pos2, Rect, RichText, Vec2};

const LINUX_KEY_UP: u32 = 103;
const LINUX_KEY_LEFT: u32 = 105;
const LINUX_KEY_RIGHT: u32 = 106;
const LINUX_KEY_DOWN: u32 = 108;
const LINUX_KEY_SOFT_LEFT: u32 = 139;
const LINUX_KEY_BACK: u32 = 158;
const LINUX_KEY_OK: u32 = 352;
const LINUX_KEY_SOFT_RIGHT: u32 = 357;

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
    renderer: oos_egui::Renderer,
    view: View,
    selected: usize,
    notice_until_us: u64,
    notice: Option<&'static str>,
    input: oos_egui::Input,
}

impl Launcher {
    fn new() -> Result<Self, oos_egui::Error> {
        let context = Context::default();
        oos_egui::install_system_fonts(&context)?;
        context.set_visuals(egui::Visuals::dark());
        Ok(Self {
            context,
            renderer: oos_egui::Renderer::new(),
            view: View::Home,
            selected: 0,
            notice_until_us: 0,
            notice: None,
            input: oos_egui::Input::new(),
        })
    }

    fn key(&mut self, event: oos_app::KeyEvent) {
        let code = event.code;
        let now_us = event.monotonic_time_us;
        let pressed = matches!(event.action, oos_app::KeyAction::Pressed);
        self.input.push_key(event);
        if !pressed {
            return;
        }

        match (self.view, code) {
            (View::Home, LINUX_KEY_OK) => self.view = View::Apps,
            (View::Home, LINUX_KEY_SOFT_LEFT) => {
                self.notice = Some("No notifications");
                self.notice_until_us = now_us + 1_400_000;
            }
            (View::Home, LINUX_KEY_SOFT_RIGHT) => {
                self.notice = Some("Camera is not installed");
                self.notice_until_us = now_us + 1_400_000;
            }
            (View::Apps, LINUX_KEY_BACK | LINUX_KEY_SOFT_RIGHT) => {
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
            (View::Apps, LINUX_KEY_OK) => {
                self.notice = Some("App is not installed");
                self.notice_until_us = now_us + 1_400_000;
            }
            (View::Apps, LINUX_KEY_SOFT_LEFT) => {
                self.notice = Some("Options are not available");
                self.notice_until_us = now_us + 1_400_000;
            }
            _ => {}
        }
    }

    fn frame(&mut self, now_us: u64) -> Result<(), &'static str> {
        if self.notice.is_some() && now_us >= self.notice_until_us {
            self.notice = None;
        }
        let input = self.input.take(now_us);

        let view = self.view;
        let selected = self.selected;
        let notice = self.notice;
        let output = self.context.run_ui(input, |ui| {
            render_launcher(ui, view, selected, notice);
        });
        let backend_output = self
            .renderer
            .submit(&self.context, output, [13, 16, 16, 255])
            .map_err(|error| error.message())?;
        if !backend_output.platform_output.commands.is_empty()
            || backend_output
                .viewport_output
                .iter()
                .any(|(id, output)| *id != egui::ViewportId::ROOT || !output.commands.is_empty())
        {
            return Err("launcher requested an unsupported OOS platform action");
        }
        Ok(())
    }
}

fn render_launcher(ui: &mut egui::Ui, view: View, selected: usize, notice: Option<&str>) {
    egui::Panel::top("status")
        .exact_size(20.0)
        .frame(
            egui::Frame::new()
                .fill(Color32::from_rgb(13, 16, 16))
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
                .fill(Color32::from_rgb(32, 33, 33))
                .inner_margin(egui::Margin::symmetric(5, 5)),
        )
        .show(ui, |ui| {
            let labels = if view == View::Home {
                ["Notices", "Apps", "Camera"]
            } else {
                ["Options", "Open", "Back"]
            };
            ui.columns(3, |columns| {
                columns[0].label(
                    RichText::new(labels[0])
                        .size(10.0)
                        .strong()
                        .color(Color32::from_rgb(240, 237, 233)),
                );
                columns[1].with_layout(egui::Layout::top_down(egui::Align::Center), |ui| {
                    ui.label(
                        RichText::new(labels[1])
                            .size(10.0)
                            .strong()
                            .color(Color32::from_rgb(230, 81, 0)),
                    );
                });
                columns[2].with_layout(egui::Layout::top_down(egui::Align::RIGHT), |ui| {
                    ui.label(
                        RichText::new(labels[2])
                            .size(10.0)
                            .strong()
                            .color(Color32::from_rgb(240, 237, 233)),
                    );
                });
            });
        });
    egui::CentralPanel::default()
        .frame(
            egui::Frame::new()
                .fill(Color32::from_rgb(21, 22, 22))
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
                    Color32::from_rgb(230, 81, 0),
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
            egui::Stroke::new(3.0, Color32::from_rgb(230, 81, 0)),
        );
        ui.painter().line_segment(
            [
                center + Vec2::new(-13.0, 13.0),
                center + Vec2::new(13.0, -13.0),
            ],
            egui::Stroke::new(2.0, Color32::from_rgb(230, 81, 0)),
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
                .color(Color32::from_rgb(230, 81, 0)),
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
                    Color32::from_rgb(43, 44, 45)
                } else {
                    Color32::from_rgb(21, 22, 22)
                };
                let stroke = if index == selected {
                    egui::Stroke::new(2.0, Color32::from_rgb(230, 81, 0))
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
        let launcher = Launcher::new().map_err(|error| {
            oos_app::log(3, error.message());
            oos_app::ErrorCode::Unavailable
        })?;
        LAUNCHER.with(|slot| *slot.borrow_mut() = Some(launcher));
        oos_app::log(1, "egui launcher initialized");
        Ok(())
    }

    fn event(event: oos_app::KeyEvent) {
        LAUNCHER.with(|launcher| {
            if let Some(launcher) = launcher.borrow_mut().as_mut() {
                launcher.key(event);
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
