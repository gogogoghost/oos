import {
  BatteryMedium,
  Camera,
  ContactRound,
  Globe2,
  MessageSquareText,
  Phone,
  Settings,
  Signal,
  Wifi,
} from "lucide-solid";
import { createEffect, createMemo, createSignal, For, onCleanup, Show } from "solid-js";
import { Dynamic } from "solid-js/web";

import citrusMark from "./citrus.svg";

type ViewName = "home" | "apps";
type NavigationAction =
  | "up"
  | "down"
  | "left"
  | "right"
  | "ok"
  | "back"
  | "soft-left"
  | "soft-right";

interface AppEntry {
  label: string;
  icon: typeof Phone;
  accent: string;
}

declare global {
  interface Window {
    oosHandleKey?: (code: number, action: string) => void;
  }
}

const applications: AppEntry[] = [
  { label: "Phone", icon: Phone, accent: "#21a366" },
  { label: "Messages", icon: MessageSquareText, accent: "#2777d3" },
  { label: "Contacts", icon: ContactRound, accent: "#805ad5" },
  { label: "Camera", icon: Camera, accent: "#dc5a45" },
  { label: "Browser", icon: Globe2, accent: "#1597a5" },
  { label: "Settings", icon: Settings, accent: "#68727d" },
];

const linuxKeyMap: Record<number, NavigationAction> = {
  103: "up",
  105: "left",
  106: "right",
  108: "down",
  139: "soft-left",
  158: "back",
  352: "ok",
  357: "soft-right",
};

function formatTime(date: Date) {
  return date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", hour12: false });
}

function formatDate(date: Date) {
  return date.toLocaleDateString([], { weekday: "short", month: "short", day: "numeric" });
}

export function Launcher() {
  const [view, setView] = createSignal<ViewName>("home");
  const [selected, setSelected] = createSignal(0);
  const [now, setNow] = createSignal(new Date());
  const [notice, setNotice] = createSignal("");
  let noticeTimer: number | undefined;

  const clockTimer = window.setInterval(() => setNow(new Date()), 1000);
  onCleanup(() => {
    window.clearInterval(clockTimer);
    if (noticeTimer !== undefined) window.clearTimeout(noticeTimer);
  });

  const showNotice = (message: string) => {
    setNotice(message);
    if (noticeTimer !== undefined) window.clearTimeout(noticeTimer);
    noticeTimer = window.setTimeout(() => setNotice(""), 1400);
  };

  const navigate = (action: NavigationAction) => {
    if (view() === "home") {
      if (action === "ok") setView("apps");
      if (action === "soft-left") showNotice("No alerts");
      if (action === "soft-right") showNotice("Camera is not installed");
      return;
    }

    if (action === "back" || action === "soft-left") {
      setView("home");
      setNotice("");
      return;
    }
    if (action === "ok") {
      showNotice(`${applications[selected()].label} is not installed`);
      return;
    }
    if (action === "soft-right") {
      showNotice("Options are not available");
      return;
    }

    const current = selected();
    if (action === "left") setSelected((current + applications.length - 1) % applications.length);
    if (action === "right") setSelected((current + 1) % applications.length);
    if (action === "up") setSelected((current + applications.length - 3) % applications.length);
    if (action === "down") setSelected((current + 3) % applications.length);
  };

  const browserKey = (event: KeyboardEvent) => {
    const keyMap: Record<string, NavigationAction> = {
      ArrowUp: "up",
      ArrowDown: "down",
      ArrowLeft: "left",
      ArrowRight: "right",
      Enter: "ok",
      Escape: "back",
      Backspace: "back",
      ContextMenu: "soft-left",
      Unidentified: "soft-right",
      q: "soft-left",
      w: "soft-right",
    };
    const action = keyMap[event.key];
    if (!action) return;
    event.preventDefault();
    navigate(action);
  };

  window.addEventListener("keydown", browserKey);
  onCleanup(() => window.removeEventListener("keydown", browserKey));

  createEffect(() => {
    window.oosHandleKey = (code, action) => {
      if (action !== "pressed") return;
      const mapped = linuxKeyMap[code];
      if (mapped) navigate(mapped);
    };
  });
  onCleanup(() => delete window.oosHandleKey);

  const softKeys = createMemo(() =>
    view() === "home"
      ? { left: "Alerts", center: "Apps", right: "Camera" }
      : { left: "Back", center: "Open", right: "Options" },
  );

  return (
    <main class="launcher-shell" aria-label="Orange OS launcher">
      <header class="status-bar">
        <span class="status-clock">{formatTime(now())}</span>
        <div class="status-icons" aria-label="Network and battery status">
          <Signal size={11} strokeWidth={2.4} />
          <Wifi size={11} strokeWidth={2.4} />
          <BatteryMedium size={13} strokeWidth={2.2} />
        </div>
      </header>

      <section class="screen-content">
        <Show
          when={view() === "home"}
          fallback={
            <div class="apps-view">
              <div class="apps-heading">
                <img src={citrusMark} alt="" />
                <h1>Apps</h1>
              </div>
              <div class="app-grid" role="listbox" aria-label="Applications">
                <For each={applications}>
                  {(app, index) => (
                    <button
                      class="app-tile"
                      classList={{ selected: selected() === index() }}
                      style={{ "--app-accent": app.accent }}
                      type="button"
                      role="option"
                      aria-selected={selected() === index()}
                      onClick={() => {
                        setSelected(index());
                        showNotice(`${app.label} is not installed`);
                      }}
                    >
                      <span class="app-icon">
                        <Dynamic component={app.icon} size={25} strokeWidth={2} />
                      </span>
                      <span class="app-label">{app.label}</span>
                    </button>
                  )}
                </For>
              </div>
            </div>
          }
        >
          <div class="home-view">
            <div class="brand-mark" aria-hidden="true">
              <img src={citrusMark} alt="" />
            </div>
            <time class="hero-clock" dateTime={now().toISOString()}>{formatTime(now())}</time>
            <p class="hero-date">{formatDate(now())}</p>
            <div class="home-shortcuts" aria-label="Quick status">
              <span>Orange OS</span>
              <span class="status-dot" />
              <span>Ready</span>
            </div>
          </div>
        </Show>

        <Show when={notice()}>
          <div class="notice" role="status">{notice()}</div>
        </Show>
      </section>

      <footer class="softkey-bar">
        <button type="button" onClick={() => navigate("back")}>{softKeys().left}</button>
        <button class="softkey-primary" type="button" onClick={() => navigate("ok")}>
          {softKeys().center}
        </button>
        <button type="button" onClick={() => view() === "home" && showNotice("Camera is not installed")}>
          {softKeys().right}
        </button>
      </footer>
    </main>
  );
}
