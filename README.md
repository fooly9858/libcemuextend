# libcemuextend

CemuExtend ABI 2専用のwire定義と、`isolated`／`trusted_native` `.cemod`向けC++20クライアントです。

ABI 2はCemu所有のcopy transportだけを使います。共有Header、Ring、State Page、Bulk Poolとguest提供serviceはありません。ABI 1クライアントはCemu側で常に`AbiMismatch`になります。Client APIとwire schemaは両実行モードで共通です。

ABI 2.1では、既存のMouseV2 payloadサイズを維持したままRaw Mouse相対移動フラグを追加し、UTF-32 TextイベントとRaw Mouseの選択ポリシーを追加しました。2.1クライアントは2.0ホストへ接続せず、必要な入力機能がない構成を明示的に`AbiMismatch`として扱います。

`InputInjectMapped`の`ObservedVpadState::flags`には
`MappedInputFlag::ReplacePhysical`を指定できます。有効な注入が存在する間は、
Cemuの通常コントローラープロファイルから来るVPADボタンと左右スティックを
消去してからmapped入力を適用します。gyro、touchなどのsensor状態は保持されます。

## 組み込み

```cmake
add_subdirectory(third_party/libcemuextend)
target_link_libraries(my_cemod PRIVATE cemuextend::guest)
```

devkitPPCでは`libcemuextend.mk`をincludeできます。`isolated` buildには6個のCEX2 import stubが含まれ、`.cemod` loaderが固定placeholderだけを検査済みHLE IDへ結線します。

`trusted_native`はMod自身の`OSDynLoad_Acquire`／`OSDynLoad_FindExport`で同じ6関数を解決します。

```cpp
#include <cemuextend/cemuextend.hpp>

std::uint64_t ModMonotonicTimeNs();

cemuextend::guest::Client client;

extern "C" int cemod_init() {
    if (!cemuextend::guest::ConfigureCemodPlatform(ModMonotonicTimeNs))
        return -1;
    return client.Initialize() == cemuextend::wire::Error::Ok ? 0 : -1;
}

extern "C" void cemod_tick() {
    client.Pump();
}

extern "C" void cemod_event(std::uint32_t event, const void*, std::uint32_t) {
    // 1: title loaded, 2: permissions changed
}

extern "C" void cemod_shutdown() {
    client.Shutdown();
}
```

単調時刻callbackはMod自身のtick counterから実装でき、Cafe OS APIを必要としません。native unit testでは`ConfigurePlatform`へcopy transportの6関数と時刻関数を直接渡せます。

trusted Modでは次のresolver設定を使用します。

```cpp
cemuextend::guest::ConfigureTrustedCafePlatform(
    OSDynLoad_Acquire, OSDynLoad_FindExport, MonotonicTimeNs);
```

このAPIが解決するのは`cemuextend` moduleのCEX2 exportだけです。ゲームmemory、GX2／Cafe API、hookはtrusted Mod自身のruntimeが管理し、CEX2 wire層には混在させません。CMB1 bootstrap tableの固定layoutは`<cemuextend/bootstrap.hpp>`で共有します。

## 制約

- request/responseは1件64 KiB以下です。
- queue/pendingの既定上限は各128件、timeoutは5秒です。
- File/Captureは64 KiB単位のstream read、一覧は最大128件のpaginationを使います。
- timeout、cancel、disconnect、shutdownではcallbackが必ず完了します。
- 自動再送は行いません。
- `Disconnected`と`Cancelled`は通常の完了statusとして通知されます。
- callback中の`Shutdown`や`Client`破棄を考慮し、内部状態は共有所有されます。

## テスト

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
