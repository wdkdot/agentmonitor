# AI Usage Monitor

Waveshare ESP32-S3-Touch-AMOLED-1.64용 AI 사용량 모니터입니다. PC 프로그램이
Codex와 Claude Code의 사용량을 USB HID로 전달하고, ESP32-S3 펌웨어가 AMOLED에 표시합니다.

```text
firmware/               ESP-IDF + LVGL 기기 펌웨어
ai-usage-display-host/  Rust 기반 사용량 수집·USB 전송 프로그램
concept.png              UI 스타일 참조 이미지
```

## Build

펌웨어는 ESP-IDF 5.5.2에서 빌드합니다.

```powershell
cd firmware
idf.py set-target esp32s3
idf.py build
```

호스트 프로그램은 Rust가 필요합니다.

```powershell
cd ai-usage-display-host
cargo build --release
```

계정 정보는 장치에 저장하지 않습니다. 호스트가 정규화한 사용량 데이터만 USB로 전달합니다.

상세 빌드·실행 방법은 각 프로젝트의 README를 참고하세요.
