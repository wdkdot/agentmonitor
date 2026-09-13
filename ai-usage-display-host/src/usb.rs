use std::path::Path;
use std::time::{Duration, Instant};

use hidapi::{HidApi, HidDevice};

pub const DEFAULT_VID: u16 = 0x303a;
pub const DEFAULT_PID: u16 = 0x4004;
const PRODUCT_NAME: &str = "AI Usage Monitor";

pub struct UsbTarget {
    pub vid: u16,
    pub pid: u16,
}

impl Default for UsbTarget {
    fn default() -> Self {
        Self {
            vid: DEFAULT_VID,
            pid: DEFAULT_PID,
        }
    }
}

pub fn send(packet: &[u8; 64], target: &UsbTarget) -> Result<(), String> {
    let api = HidApi::new().map_err(|e| format!("HID 초기화 실패: {e}"))?;
    let device = find_device(&api, target)?;
    write_report(&device, packet)
}

fn write_report(device: &HidDevice, packet: &[u8; 64]) -> Result<(), String> {
    // HIDAPI reserves byte zero for the report ID. This descriptor has no
    // report IDs, so zero is prepended and the 64-byte AIUD payload follows.
    let mut report = [0u8; 65];
    report[1..].copy_from_slice(packet);
    let written = device
        .write(&report)
        .map_err(|e| format!("HID 패킷 전송 실패: {e}"))?;
    if written < packet.len() {
        return Err(format!("HID 패킷 일부만 전송됨: {written} bytes"));
    }
    Ok(())
}

pub fn capture_bmp(path: &Path, target: &UsbTarget) -> Result<(u16, u16), String> {
    let api = HidApi::new().map_err(|e| format!("HID 초기화 실패: {e}"))?;
    let device = find_device(&api, target)?;
    let mut command = [0u8; 64];
    command[..13].copy_from_slice(b"AIUD-SNAPSHOT");
    write_report(&device, &command)?;

    let deadline = Instant::now() + Duration::from_secs(15);
    let mut input = [0u8; 65];
    let (width, height, size, report_prefix) = loop {
        if Instant::now() >= deadline {
            return Err("화면 캡처 헤더 수신 시간이 초과되었습니다.".into());
        }
        let count = device
            .read_timeout(&mut input, 500)
            .map_err(|e| format!("화면 캡처 헤더 수신 실패: {e}"))?;
        if count < 16 {
            continue;
        }
        let prefix = if input[..count].starts_with(b"AIUDSNAP") {
            0
        } else if count >= 17 && input[0] == 0 && input[1..].starts_with(b"AIUDSNAP") {
            1
        } else {
            continue;
        };
        let width = u16::from_le_bytes([input[prefix + 8], input[prefix + 9]]);
        let height = u16::from_le_bytes([input[prefix + 10], input[prefix + 11]]);
        let size = u32::from_le_bytes([
            input[prefix + 12],
            input[prefix + 13],
            input[prefix + 14],
            input[prefix + 15],
        ]) as usize;
        if width == 0 || height == 0 || size != width as usize * height as usize * 2 {
            return Err(format!(
                "잘못된 화면 캡처 크기: {width}x{height}, {size} bytes"
            ));
        }
        break (width, height, size, prefix);
    };

    let mut pixels = Vec::with_capacity(size);
    let deadline = Instant::now() + Duration::from_secs(30);
    while pixels.len() < size {
        if Instant::now() >= deadline {
            return Err(format!(
                "화면 캡처 데이터 부족: {}/{} bytes",
                pixels.len(),
                size
            ));
        }
        let count = device
            .read_timeout(&mut input, 1000)
            .map_err(|e| format!("화면 캡처 데이터 수신 실패: {e}"))?;
        if count <= report_prefix {
            continue;
        }
        let available = count - report_prefix;
        let needed = size - pixels.len();
        pixels.extend_from_slice(&input[report_prefix..report_prefix + available.min(needed)]);
    }

    std::fs::write(path, rgb565_be_to_bmp(width, height, &pixels))
        .map_err(|e| format!("화면 캡처 파일 저장 실패: {e}"))?;
    Ok((width, height))
}

fn rgb565_be_to_bmp(width: u16, height: u16, pixels: &[u8]) -> Vec<u8> {
    let row_bytes = width as usize * 3;
    let row_stride = (row_bytes + 3) & !3;
    let image_size = row_stride * height as usize;
    let mut bmp = Vec::with_capacity(54 + image_size);
    bmp.extend_from_slice(b"BM");
    bmp.extend_from_slice(&(54u32 + image_size as u32).to_le_bytes());
    bmp.extend_from_slice(&[0; 4]);
    bmp.extend_from_slice(&54u32.to_le_bytes());
    bmp.extend_from_slice(&40u32.to_le_bytes());
    bmp.extend_from_slice(&(width as i32).to_le_bytes());
    bmp.extend_from_slice(&(height as i32).to_le_bytes());
    bmp.extend_from_slice(&1u16.to_le_bytes());
    bmp.extend_from_slice(&24u16.to_le_bytes());
    bmp.extend_from_slice(&0u32.to_le_bytes());
    bmp.extend_from_slice(&(image_size as u32).to_le_bytes());
    bmp.extend_from_slice(&[0; 16]);

    for y in (0..height as usize).rev() {
        for x in 0..width as usize {
            let index = (y * width as usize + x) * 2;
            let value = u16::from_be_bytes([pixels[index], pixels[index + 1]]);
            let red = ((((value >> 11) & 0x1f) as u32 * 255) / 31) as u8;
            let green = ((((value >> 5) & 0x3f) as u32 * 255) / 63) as u8;
            let blue = (((value & 0x1f) as u32 * 255) / 31) as u8;
            bmp.extend_from_slice(&[blue, green, red]);
        }
        bmp.resize(bmp.len() + row_stride - row_bytes, 0);
    }
    bmp
}

fn find_device(api: &HidApi, target: &UsbTarget) -> Result<HidDevice, String> {
    let exact = api
        .device_list()
        .find(|info| info.vendor_id() == target.vid && info.product_id() == target.pid);
    if let Some(info) = exact {
        return info
            .open_device(api)
            .map_err(|e| format!("HID 장치를 열 수 없습니다: {e}"));
    }

    // Product string fallback makes development builds resilient when the
    // TinyUSB default PID changes with enabled interface classes.
    if let Some(info) = api
        .device_list()
        .find(|info| info.product_string() == Some(PRODUCT_NAME))
    {
        return info
            .open_device(api)
            .map_err(|e| format!("HID 장치를 열 수 없습니다: {e}"));
    }

    Err(format!(
        "AI Usage Monitor HID를 찾지 못했습니다 (VID={:04X}, PID={:04X})",
        target.vid, target.pid
    ))
}
