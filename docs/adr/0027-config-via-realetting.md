# ADR-0027：settings.json 的读写交给 realetting

- 状态：已采纳（2026-10-07）
- 修订：ADR-0010 的读取与写回方式（默认树、只认 HOME、不热重载不变）

## 背景

`config.cpp` 里为 settings.json 手写了一整套：读文件、逐键合并默认树、tmp + rename 写回、坏文件拒写、一把 mutex。审下来有三处毛病：`persist` 读—改—写不在锁内，两个 agent 同时写会丢键；`get()` 用 `value()`，用户把字符串键写成数字就抛异常；注释说断电安全，但没有 fsync。

这些都不是 realagent 特有的问题，抽成一个库：[frevlm/realetting](https://github.com/frevlm/realetting)。指定配置目录，拿到一个 json 位置，改了就写回文件，只写差别。

## 决策

1. **CMake `FetchContent` 引入，钉 tag**（现为 `v0.1.0`），与 cpp-httplib 同一种方式。升级只改 `GIT_TAG`。
2. **nlohmann/json 只留一份：realetting 带的那份。** 删掉 `core/include/json.hpp`，全项目改为 `#include <realetting/json.hpp>`。两份并存时 include guard 同名，先进来的那份静默生效，版本一旦分叉就是暗病。nlohmann 的版本从此跟着 realetting 走。
3. `Config` 只剩一个 `realetting::Ref`：`get` 读内存里那份 默认值 ⊕ 文件，`persist` 就是 `settings_[key] = v`。
4. 删掉没有调用方的 `has()` 与 `to_json()`。
5. models.json 不动：它的根是数组、语义是整表替换、core 从不写它，不是 realetting 管的那种文件。

## 后果

- **启动时 settings.json 不存在就建一个 `{}`**（连同 `~/.realagent/`），新文件权限 0600。从前只读不建。
- `persist` 的读—改—写整体持锁；写入 fsync 后 rename，再 fsync 目录。
- `get()` 遇到非字符串返回空串，不再抛。
- `persist` 写的值与当前值相同时不碰文件。
- `persist` 之后内存跟着文件走：期间用户手改过的键也一起读进来。启动后的其余时间仍不热重载。
