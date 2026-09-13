# AI Usage Display Host

Codex + Claude Code 사용량을 읽어 ESP32-S3 기반 USB 디스플레이에 전달하기 위한 PC-side Rust 브리지의 V1입니다.

## 현재 구현된 것

- Codex와 Claude Code provider를 **서로 완전히 독립적으로** 조회
  - Codex만 사용하는 PC: 정상
  - Claude만 사용하는 PC: 정상
  - 둘 다 사용하는 PC: 정상
  - 둘 다 없는 PC: 정상 종료(상태만 `not_configured`)
- Codex
  - `CODEX_HOME/auth.json` 또는 `~/.codex/auth.json` 자동 탐색
  - ChatGPT OAuth `wham/usage` 조회
  - 5h/weekly를 위치가 아니라 window duration으로 분류
  - 인증/스키마 문제가 있으면 `codex app-server`의 `account/rateLimits/read` fallback
- Claude Code
  - `CLAUDE_CONFIG_DIR` 및 `~/.claude/.credentials.json` 자동 탐색
  - macOS에서는 `Claude Code-credentials` Keychain 항목도 fallback으로 읽음
  - `GET https://api.anthropic.com/api/oauth/usage`
  - legacy `five_hour` / `seven_day`와 새 `limits[]` / `model_scoped[]` 모두 파싱
  - OAuth token이 만료되거나 401/403이면 Claude Code CLI의 headless `get_usage` control request 사용
  - 429는 프로그램 오류로 종료하지 않고 `temporarily_unavailable`로 표현
- watch 모드에서 일시적 오류/429가 발생하면 마지막 정상값을 메모리에 유지
- Codex `account/usage/read`의 공식 `dailyUsageBuckets`로 이번 주 일별 token activity 수집
- Claude Code 로컬 JSONL usage 레코드를 중복 제거해 이번 주 일별 token activity 집계
- USB HID 고정 64-byte packet encoder와 테스트
- Espressif 개발 VID와 TinyUSB HID 기본 PID 또는 product string으로 장치를 찾아 전송
- 배포 전 제품용 VID/PID 할당 필요

## 빌드

Windows에서 Rust stable toolchain을 설치한 뒤, 이 폴더에서 실행합니다.

```powershell
cargo build --release
```

트레이 앱 실행 파일은 `target/release/ai-usage-display-host.exe`에 생성됩니다.

Windows 설치 프로그램이 필요할 때만 Tauri CLI를 설치해 번들링합니다.

```powershell
cargo install tauri-cli --version "^2" --locked
cargo tauri build
```

생성된 설치 프로그램은 `target/release/bundle/` 아래에 있습니다. `target/`은 생성물이므로
GitHub에 올리지 않습니다.

## 테스트 실행

사람이 읽기 쉬운 출력:

```bash
cargo run --release
```

JSON:

```bash
cargo run --release -- --json
```

실제로 전송되는 64-byte HID packet까지 함께 확인:

```bash
cargo run --release -- --packet-hex
```

장치 없이 provider 출력만 확인:

```bash
cargo run --release -- --no-usb
```

개발 VID/PID를 직접 지정:

```bash
cargo run --release -- --vid 303a --pid 4004
```

펌웨어 업데이트를 위해 장치를 ROM 다운로드 모드로 재시작:

```bash
cargo run --release -- --bootloader
```

실제 LVGL 홈 화면을 BMP로 저장:

```bash
cargo run --release -- --capture screen.bmp
```

5분마다 반복 조회:

```bash
cargo run --release -- --watch --interval 300
```

Claude usage endpoint가 429를 낼 수 있으므로 기본 운영 주기는 5분 정도를 권장합니다.

## 상태 모델

provider 하나가 없다고 프로그램 전체를 실패시키지 않습니다.

```json
{
  "codex": {
    "status": "available",
    "data": { "session": {}, "weekly": {} }
  },
  "claude": {
    "status": "not_configured",
    "message": "...정상입니다."
  }
}
```

실제 디스플레이에서는 `not_configured` provider를 숨기면 됩니다.

## 다음 단계

1. 제품용 VID/PID 할당
2. 마지막 정상 데이터를 디스크 cache에 보관하여 프로세스 재시작 후에도 화면 유지
3. Windows tray + 로그인 시 자동 시작

## 구현 참고

- OpenAI Codex app-server protocol (`account/rateLimits/read`)
- Sundogs-s/Claude-Codex-Usage-Monitor의 Claude `get_usage` control request 및 OAuth usage parsing
- upstream-ray/codex-usage-monitor의 independent provider/error-state 설계

외부 프로젝트의 UI 코드는 포함하지 않았고, provider 동작 원리만 참고하여 새로 작성한 코드입니다.
