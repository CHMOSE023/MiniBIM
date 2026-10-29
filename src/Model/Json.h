#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>

// 模型核心用的 JSON：命令的参数与结果、项目文件。ordered_json 保留键的写入顺序，
// 写出的文件与 BIMCraft（serde）一样按字段的声明顺序排列，便于对照。
// 读取一律经 Json::parse(text, nullptr, false)（失败时得到 discarded 值，不抛异常），取值前先查类型。
namespace MiniBIM::Bim
{
    using Json = nlohmann::ordered_json;

    // 非负整数：解析出的是无符号整数，代码里构造的（Json(1000)）是有符号整数，两种都认
    inline std::optional<uint64_t> AsWhole(const Json& v)
    {
        if (v.is_number_unsigned())
            return v.get<uint64_t>();
        if (v.is_number_integer() && v.get<int64_t>() >= 0)
            return static_cast<uint64_t>(v.get<int64_t>());
        return std::nullopt;
    }
}
