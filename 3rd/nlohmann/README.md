# nlohmann/json（第三方：单头文件 JSON 库）

来源：[nlohmann/json](https://github.com/nlohmann/json) v3.11.3 的发布文件 `json.hpp`（单头文件版，即上游的 `single_include/nlohmann/json.hpp`），
MIT 许可，全文见本目录的 `LICENSE.MIT`。

| 文件 | 版本 | SHA-256 |
|---|---|---|
| `include/nlohmann/json.hpp` | 3.11.3 | `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6` |
| `LICENSE.MIT` | 3.11.3 | `86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14` |

用途：模型核心（`src/Model`、`src/Commands`）的命令参数与结果、项目文件（`.minibim`，与 BIMCraft 的 `.bimcraft` 同一格式）。
核心库把 `include/` 加进头文件搜索路径（`cmake/MiniBIM.cmake`），代码里经 `Model/Json.h` 使用（`Bim::Json` 即 `nlohmann::ordered_json`）。

## 本地修改

无。更新时整体替换 `json.hpp` 与 `LICENSE.MIT`，并改上表的版本与校验和。
