#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::sync::mpsc::{self, RecvTimeoutError, Sender};
use std::sync::Arc;
use std::time::Duration;

use ai_usage_display_host::model::HostSnapshot;
use ai_usage_display_host::{collect_snapshot, protocol, provider_summary, usb, UsageCache};
use tauri::menu::{CheckMenuItem, Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Manager};
use tauri_plugin_autostart::ManagerExt;

const POLL_INTERVAL: Duration = Duration::from_secs(300);

struct TrayItems {
    device: MenuItem<tauri::Wry>,
    codex: MenuItem<tauri::Wry>,
    claude: MenuItem<tauri::Wry>,
    updated: MenuItem<tauri::Wry>,
    autostart: CheckMenuItem<tauri::Wry>,
}

struct AppState {
    refresh_tx: Sender<()>,
    items: Arc<TrayItems>,
}

fn main() {
    let autostart = tauri_plugin_autostart::Builder::new()
        .app_name("AI Usage Display");
    #[cfg(target_os = "macos")]
    let autostart = autostart.macos_launcher(tauri_plugin_autostart::MacosLauncher::LaunchAgent);

    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|_app, _args, _cwd| {}))
        .plugin(autostart.build())
        .setup(setup)
        .run(tauri::generate_context!())
        .expect("AI Usage Display 실행 실패");
}

fn setup(app: &mut tauri::App) -> Result<(), Box<dyn std::error::Error>> {
    let device = MenuItem::with_id(app, "device", "USB: 확인 중…", false, None::<&str>)?;
    let codex = MenuItem::with_id(app, "codex", "Codex: 확인 중…", false, None::<&str>)?;
    let claude = MenuItem::with_id(app, "claude", "Claude: 확인 중…", false, None::<&str>)?;
    let updated = MenuItem::with_id(app, "updated", "마지막 업데이트: --", false, None::<&str>)?;
    let refresh_item = MenuItem::with_id(app, "refresh", "지금 새로고침", true, None::<&str>)?;
    let autostart_enabled = app.autolaunch().is_enabled().unwrap_or(false);
    let autostart = CheckMenuItem::with_id(app, "autostart", "로그인할 때 자동 실행", true, autostart_enabled, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", "종료", true, None::<&str>)?;
    let prepare_remove = MenuItem::with_id(app, "prepare-remove", "제거 준비 후 종료", true, None::<&str>)?;
    let separator = PredefinedMenuItem::separator(app)?;
    let separator2 = PredefinedMenuItem::separator(app)?;
    let menu = Menu::with_items(app, &[&device, &codex, &claude, &updated, &separator, &refresh_item, &autostart, &separator2, &prepare_remove, &quit])?;

    let items = Arc::new(TrayItems { device, codex, claude, updated, autostart });
    let (refresh_tx, refresh_rx) = mpsc::channel();
    app.manage(AppState { refresh_tx, items: items.clone() });

    TrayIconBuilder::with_id("main")
        .icon(tray_icon())
        .tooltip("AI Usage Display")
        .menu(&menu)
        .show_menu_on_left_click(true)
        .on_menu_event(|app, event| match event.id.as_ref() {
            "refresh" => { let _ = app.state::<AppState>().refresh_tx.send(()); }
            "autostart" => toggle_autostart(app),
            "prepare-remove" => {
                let _ = app.autolaunch().disable();
                app.exit(0);
            }
            "quit" => app.exit(0),
            _ => {}
        })
        .build(app)?;

    let handle = app.handle().clone();
    std::thread::spawn(move || {
        let mut cache = UsageCache::default();
        loop {
            refresh(&handle, &mut cache, &items);
            match refresh_rx.recv_timeout(POLL_INTERVAL) {
                Ok(()) | Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => break,
            }
        }
    });
    Ok(())
}

fn refresh(app: &AppHandle, cache: &mut UsageCache, items: &Arc<TrayItems>) {
    let mut snapshot = collect_snapshot();
    cache.apply(&mut snapshot);
    let packet = protocol::encode(&snapshot);
    let usb_result = usb::send(&packet, &usb::UsbTarget::default());
    let device_text = if usb_result.is_ok() { "USB: 디스플레이 연결됨" } else { "USB: 디스플레이를 찾을 수 없음" }.to_string();
    let tooltip = if usb_result.is_ok() { "AI Usage Display · 연결됨" } else { "AI Usage Display · 연결 안 됨" };
    let updated_text = format!("마지막 업데이트: {}", local_time(&snapshot));
    let codex_text = provider_summary("Codex", &snapshot.codex);
    let claude_text = provider_summary("Claude", &snapshot.claude);
    let items = items.clone();
    let _ = app.run_on_main_thread(move || {
        let _ = items.device.set_text(device_text);
        let _ = items.codex.set_text(codex_text);
        let _ = items.claude.set_text(claude_text);
        let _ = items.updated.set_text(updated_text);
    });
    if let Some(tray) = app.tray_by_id("main") {
        let _ = tray.set_tooltip(Some(tooltip));
    }
}

fn toggle_autostart(app: &AppHandle) {
    let manager = app.autolaunch();
    let currently_enabled = manager.is_enabled().unwrap_or(false);
    let result = if currently_enabled { manager.disable() } else { manager.enable() };
    let enabled = result.and_then(|_| manager.is_enabled()).unwrap_or(currently_enabled);
    let _ = app.state::<AppState>().items.autostart.set_checked(enabled);
}

fn local_time(snapshot: &HostSnapshot) -> String {
    chrono::DateTime::from_timestamp(snapshot.generated_at, 0)
        .map(|time| time.with_timezone(&chrono::Local).format("%H:%M").to_string())
        .unwrap_or_else(|| "--".into())
}

fn tray_icon() -> tauri::image::Image<'static> {
    const SIZE: u32 = 32;
    let mut rgba = vec![0u8; (SIZE * SIZE * 4) as usize];
    for y in 0..SIZE {
        for x in 0..SIZE {
            let dx = x as i32 - 15;
            let dy = y as i32 - 15;
            let distance = dx * dx + dy * dy;
            let on_ring = (78..=190).contains(&distance);
            let on_dot = (dx + 7) * (dx + 7) + (dy - 7) * (dy - 7) <= 16;
            if on_ring || on_dot {
                let offset = ((y * SIZE + x) * 4) as usize;
                rgba[offset] = 44;
                rgba[offset + 1] = 225;
                rgba[offset + 2] = 196;
                rgba[offset + 3] = 255;
            }
        }
    }
    tauri::image::Image::new_owned(rgba, SIZE, SIZE)
}
