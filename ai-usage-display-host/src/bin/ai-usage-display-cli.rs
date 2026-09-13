use std::path::PathBuf;
use std::time::Duration;

use ai_usage_display_host::model::{HostSnapshot, ProviderSnapshot, ProviderStatus};
use ai_usage_display_host::{collect_snapshot, demo_snapshot, protocol, usb, UsageCache};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let json_output = args.iter().any(|arg| arg == "--json");
    let packet_hex = args.iter().any(|arg| arg == "--packet-hex");
    let no_usb = args.iter().any(|arg| arg == "--no-usb");
    let watch = args.iter().any(|arg| arg == "--watch");
    let bootloader = args.iter().any(|arg| arg == "--bootloader");
    let demo = args.iter().any(|arg| arg == "--demo");
    let capture_path = parse_string_option(&args, "--capture").map(PathBuf::from);
    let interval = parse_interval(&args).unwrap_or(300).max(30);
    let usb_target = usb::UsbTarget {
        vid: parse_hex_option(&args, "--vid").unwrap_or(usb::DEFAULT_VID),
        pid: parse_hex_option(&args, "--pid").unwrap_or(usb::DEFAULT_PID),
    };

    if bootloader {
        let mut command = [0u8; protocol::PACKET_SIZE];
        command[..15].copy_from_slice(b"AIUD-BOOTLOADER");
        match usb::send(&command, &usb_target) {
            Ok(()) => println!("USB: device restarting into ROM download mode"),
            Err(error) => eprintln!("USB: {error}"),
        }
        return;
    }
    if let Some(path) = capture_path {
        match usb::capture_bmp(&path, &usb_target) {
            Ok((width, height)) => println!("USB: captured {width}x{height} screen to {}", path.display()),
            Err(error) => eprintln!("USB: {error}"),
        }
        return;
    }

    let mut cache = UsageCache::default();
    loop {
        let mut snapshot = if demo { demo_snapshot() } else { collect_snapshot() };
        cache.apply(&mut snapshot);
        if json_output {
            println!("{}", serde_json::to_string_pretty(&snapshot).expect("serialize snapshot"));
        } else {
            print_human(&snapshot);
        }
        let packet = protocol::encode(&snapshot);
        if packet_hex {
            println!("HID packet: {}", hex(&packet));
        }
        if !no_usb {
            match usb::send(&packet, &usb_target) {
                Ok(()) => println!("USB: usage packet sent"),
                Err(error) => eprintln!("USB: {error}"),
            }
        }
        if !watch { break; }
        std::thread::sleep(Duration::from_secs(interval));
    }
}

fn print_human(snapshot: &HostSnapshot) {
    println!("AI Usage Display Host v{}", env!("CARGO_PKG_VERSION"));
    print_provider("Codex", &snapshot.codex);
    print_provider("Claude", &snapshot.claude);
}

fn print_provider(name: &str, provider: &ProviderSnapshot) {
    match provider.status {
        ProviderStatus::Available => {
            println!("\n{name}: available");
            if let Some(data) = &provider.data {
                if let Some(plan) = &data.plan { println!("  plan: {plan}"); }
                print_window("session", data.session.as_ref());
                print_window("weekly", data.weekly.as_ref());
            }
        }
        ProviderStatus::NotConfigured => println!("\n{name}: not configured (정상)"),
        ProviderStatus::TemporarilyUnavailable => println!("\n{name}: temporarily unavailable"),
        ProviderStatus::AuthenticationRequired => println!("\n{name}: authentication required"),
    }
    if let Some(message) = &provider.message { println!("  {message}"); }
}

fn print_window(label: &str, window: Option<&ai_usage_display_host::model::UsageWindow>) {
    match window {
        Some(window) => println!("  {label:<12} {:>5.1}%", window.used_percent),
        None => println!("  {label:<12} --"),
    }
}

fn parse_interval(args: &[String]) -> Option<u64> {
    args.windows(2).find(|pair| pair[0] == "--interval").and_then(|pair| pair[1].parse().ok())
}

fn parse_hex_option(args: &[String], name: &str) -> Option<u16> {
    args.windows(2).find(|pair| pair[0] == name).and_then(|pair| {
        let value = pair[1].strip_prefix("0x").unwrap_or(&pair[1]);
        u16::from_str_radix(value, 16).ok()
    })
}

fn parse_string_option<'a>(args: &'a [String], name: &str) -> Option<&'a str> {
    args.windows(2).find(|pair| pair[0] == name).map(|pair| pair[1].as_str())
}

fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(text, "{byte:02X}");
    }
    text
}
