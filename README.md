# libcemuextend

`libcemuextend` は、CemuExtend Bridge の **byte-exact wire ABI** と **Wii U ゲスト向け C++20 SDK** です。CemuExtend のホスト実装と PowerPC ゲストが、同じ共有メモリ形式・サービス ID・メッセージ型を使用するための共通基盤を提供します。

このライブラリ自体は Cemu、特定のゲーム、Cafe OS SDK の型に依存しません。プラットフォーム依存処理はコールバックとして注入するため、タイトルごとのゲストランタイムへ組み込めます。

## CemuExtend との関係

| コンポーネント | 役割 |
| --- | --- |
| [CemuExtend](https://github.com/PinkDiamondTeam/CemuExtend) | `cemuextend::wire` を使って HLE ホストと標準サービスを実装する |
| **libcemuextend**（このリポジトリ） | ホスト／ゲスト共通の wire ABI と、ゲスト用 Client API を定義する |

```text
CemuExtend BridgeHost
        │
        ├──── include/cemuextend/wire.hpp ────┐
        │                                      │ ABI 1.0 / big-endian
        └──── include/cemuextend/services.hpp ─┤
                                               │
                         guest::Client ── 対応する Wii U ゲスト Mod
```

ABI の変更はホストとゲストの互換性に直結します。CemuExtend とゲスト側プロジェクトは、それぞれ動作確認済みの revision をサブモジュールとして固定してください。commit hash が異なる場合でも ABI 1.0 の互換範囲で接続できますが、新しい feature や wire layout を使う変更は両側を一緒に更新する必要があります。

## 提供するもの

| パス／ターゲット | 内容 |
| --- | --- |
| `include/cemuextend/wire.hpp` | endian 型、共有メモリ layout、ring、state page、bulk block、検証処理 |
| `include/cemuextend/services.hpp` | 標準サービス、operation、payload、Encoder / Decoder |
| `include/cemuextend/guest.hpp` | ゲスト接続、要求、購読、状態／イベント公開の高水準 API |
| `src/guest.cpp` | `cemuextend::guest::Client` の実装 |
| `cemuextend::wire` | header-only の CMake INTERFACE target |
| `cemuextend::guest` | ゲスト SDK の CMake STATIC target |

`cemuextend/cemuextend.hpp` を include すると、上記の公開ヘッダーをまとめて読み込めます。

## Bridge ABI 1.0

wire format はすべて big-endian です。既定の 256 KiB 共有メモリ領域には次の領域が 64-byte alignment で配置されます。

- 256-byte の Bridge header
- 2 KiB の service directory
- ホスト／ゲストそれぞれ 16 KiB の state page
- 双方向の control ring と event ring（計4本）
- 64 KiB payload の bulk block 2個

control ring は要求／応答、event ring は購読イベント、state page はフレーム単位の最新スナップショット、bulk block は ring に収まらないデータに使用します。`ValidateLayout()` はアクセス前に alignment、範囲、重複、overflow、ring、state page、service directory、bulk metadata を検査します。

既定領域は 256 KiB、許容上限は 1 MiB です。接続の heartbeat timeout は 5秒です。ABI major が一致しない場合や layout が不正な場合、接続は開始されません。

## 標準サービス

`services.hpp` は次のホストサービスを定義します。

| Service ID | Client API | 内容 |
| --- | --- | --- |
| Core | `GetServices`, `Ping`, `GetVersion`, `Subscribe` | 能力検出、疎通、購読、統計 |
| Input | `InputInjectGuest`, `InputInjectMapped` | 入力イベント、VPAD 状態、入力注入 |
| Logging | `Log` | ホストログへの出力 |
| Configuration | `Configuration*` | 型付き Key-Value 設定 |
| File | `File*` | ホストが隔離するファイル領域の操作 |
| Clipboard | `ClipboardGet/Set` | ホストクリップボード |
| Window | `WindowGet` | ウィンドウ／描画面の状態 |
| Capture | `CaptureOpen/Read/Close` | TV / DRC フレーム取得 |
| Diagnostics | `DiagnosticsGet` | ring、heartbeat、要求数などの診断 |

`GameState`（`0x100`）はゲスト提供サービス用に予約され、`CustomBase`（`0x8000`）以降はアプリケーション固有サービスに利用できます。実際に利用可能なサービスと権限は、接続後に service directory または `GetServices()` で確認してください。

## ゲストへの組み込み

### CMake

```cmake
add_subdirectory(third_party/libcemuextend)
target_link_libraries(my_guest PRIVATE cemuextend::guest)
```

wire 定義だけが必要なホスト側コードやツールでは `cemuextend::wire` をリンクします。

### devkitPPC Makefile

リポジトリに含まれる `libcemuextend.mk` は include path と `src/guest.cpp` を既存ビルドへ追加します。

```make
LIBCEMUEXTEND_ROOT := third_party/libcemuextend
include $(LIBCEMUEXTEND_ROOT)/libcemuextend.mk
```

既定の相対パスと異なる場合は、include 前に `CEMUEXTEND_DIR` を設定してください。

## 最小のゲストコード

ゲーム側の DynLoad、allocator、単調増加時刻をラップし、クライアントを初期化します。

```cpp
#include <cemuextend/cemuextend.hpp>

cemuextend::guest::PlatformCallbacks callbacks{
    DynLoadAcquire,
    DynLoadFindExport,
    AlignedAllocate,
    Free,
    MonotonicTimeNs,
};

if (!cemuextend::guest::ConfigurePlatform(callbacks)) {
    // コールバックが不足または不正
}

cemuextend::guest::Client bridge;
const auto error = bridge.Initialize();
if (error == cemuextend::wire::Error::Ok) {
    bridge.Ping(0x43455854ULL,
        [](cemuextend::wire::Status status, std::span<const std::byte> payload) {
            // 応答は Pump() を呼んだスレッドで実行される
        });
}

// ゲームの update/render thread から毎フレーム呼ぶ
bridge.Pump();
```

`Initialize()` は `cemuextend` HLE モジュールの export を動的解決し、共有領域を確保・初期化してホストへ登録します。通常の Cemu や実機などホスト機能が存在しない環境では `Unavailable` になり得るため、ゲスト Mod 本体は Bridge なしでも安全に動けるようにしてください。

終了時は `Shutdown()` を呼びます。再接続する場合は、切断後に改めて `Initialize()` を呼べます。

## 要求、イベント、状態

要求は専用メソッド、または汎用の `Send(Request)` で送信できます。

```cpp
bridge.Log(cemuextend::wire::LogLevel::Info, "guest started");

bridge.Subscribe(cemuextend::wire::ServiceId::Window,
    [](std::uint16_t operation, std::span<const std::byte> payload) {
        // WindowEvent を処理
    });
```

ゲスト固有サービスをホストへ公開する場合は、`InitializeOptions::guestServices` に `ServiceDescriptor` を渡し、`PublishState()` と `PublishEvent()` を使用します。

```cpp
cemuextend::wire::ServiceDescriptor gameState{};
gameState.serviceId = static_cast<std::uint16_t>(cemuextend::wire::ServiceId::GameState);
gameState.major = 1;
gameState.minor = 0;
gameState.direction = static_cast<std::uint8_t>(
    cemuextend::wire::ServiceDirection::GuestProvides);
gameState.features = static_cast<std::uint8_t>(
    cemuextend::wire::ServiceFeature::State);

const std::array services{gameState};
cemuextend::guest::InitializeOptions options{};
options.guestServices = services;
bridge.Initialize(options);
```

ホストの最新状態は `ReadHostState()` で取得できます。複数項目を同一 snapshot として読む場合は、呼び出し側が用意した output / scratch buffer を使う `ReadHostStates()` を使用してください。

## スレッドとライフタイムの注意

- `Pump()` はゲーム callback を実行してよい同じスレッドから、継続的に呼び出してください。
- 応答 callback と購読した event callback は `Pump()` の呼び出し中に実行されます。
- 他スレッドから送った要求は、既定で最大128件の bounded queue に入ります。満杯の場合は `Busy` となり、`Statistics::queueOverflows` に記録されます。
- callback に渡される `std::span` は callback 中だけ有効です。後で使うデータはその場でコピーしてください。
- state page は seqlock snapshot、bulk block は owner / generation を検証します。受信側がコピーした後、bulk block は自動解放されます。
- `Client` は copy できません。破棄前に処理中の callback や、callback が参照するオブジェクトの寿命を整理してください。

## ネイティブテスト

wire layout とゲスト Client はホスト環境上でテストできます。

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

テストを無効化して依存プロジェクトへ組み込む場合:

```sh
cmake -S . -B build -DCEMUEXTEND_BUILD_TESTS=OFF
```

テストは endian / layout 検証、ring の wrap と破損検知、state snapshot、bulk generation、要求 queue、callback、heartbeat 切断などを対象にします。
