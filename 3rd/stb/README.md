# stb（第三方：单文件库）

来源：[nothings/stb](https://github.com/nothings/stb)，公有领域（或 MIT，二选一，见各文件末尾）。

| 文件 | 版本 | 用途 |
|---|---|---|
| `stb_image.h` | 见文件开头 | 光栅图像解码（MiniGUI 的 `src/UI/Paint/Image.cpp`） |
| `stb_truetype.h` | v1.26 | TrueType 字形轮廓：MiniGUI 的界面字体（`src/UI/Text/Font.cpp`） |

## 本地修改

复制自 [MiniCAD](https://github.com/CHMOSE023/MiniCAD) 提交 `0bd1687` 的 `3rd/stb`。对上游源码的改动都记在这里，便于更新时重新应用；改动处标有"MiniCAD 修改"（改动是在 MiniCAD 里做的，标记照原样保留）。

- `stb_truetype.h` 的 `stbtt__GetGlyphShapeTT`：轮廓以曲线外的点开头时，上游无条件读取下一个点 `vertices[off+i+1]` 来定起点。轮廓只有这一个点时（如宋体 simsun.ttc 的空格），读到的是下一个轮廓的点，最后一个轮廓则越过数组末尾读到未初始化的堆内存，起点随机，字形画成一串长斜线，而且每次运行都不同。改为：下一个点不在本轮廓里时从这个点自身开始（字形只剩一条零长度线段）。测试在 MiniCAD 的 `tests/Core/TextStyleTests.cpp` 第 7 节
