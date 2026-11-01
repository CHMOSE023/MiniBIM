// 移植自 BIMCraft crates/core/src/step.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ISO 10303-21，IFC 文件的写法（STEP 物理文件）：头部写出模式，然后是编号的实体，
// `#12=IFCWALL('2O2Fr$t4X7Zf8NOew3FLOH',#5,'Wall',$,…);`。整个文件读进内存：每个实体的名称（大写）与参数
namespace MiniBIM::Bim::Ifc::Step
{
    // 一个参数
    struct V
    {
        enum class Kind
        {
            Null,   // `$`（没有值）或 `*`（派生）
            Int,
            Real,
            Str,
            Enum,   // `.T.`、`.ELEMENT.`：两个点之间的名称
            Ref,    // `#12`
            List,
            Typed,  // `IFCLABEL('Wall')`：有名称的类型的值
        };
        Kind           kind = Kind::Null;
        int64_t        i    = 0;
        double         x    = 0.0;
        uint64_t       ref  = 0;
        std::string    s;     // Str；Enum 的名称；Typed 的类型名
        std::vector<V> list;  // List；Typed 只有一个：它的值

        static V Int(int64_t v);
        static V Real(double v);
        static V Str(std::string v);
        static V Enum(std::string v);
        static V Ref(uint64_t v);
        static V List(std::vector<V> v);
        static V Typed(std::string name, V v);

        bool operator==(const V& o) const;
    };

    struct Entity
    {
        std::string    name;
        std::vector<V> args;
    };

    struct File
    {
        // FILE_SCHEMA 给出的第一个模式，大写：IFC4、IFC2X3
        std::string                            schema;
        std::unordered_map<uint64_t, Entity> entities;
    };

    // 读整个文件；读不了时原因（带行号）放进 error
    bool Parse(std::string_view text, File& out, std::string& error);

    // 字符串里的转义还原：`\X2\00D6\X0\`（UTF-16）、`\X4\…\X0\`（UTF-32）、`\X\D6`（ISO 8859-1）、
    // `\S\V`（代码页的上半）、`\\`；切换代码页的 `\PA\` 丢掉
    std::string Decode(std::string_view s);
}
