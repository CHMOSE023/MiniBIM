// 移植自 BIMCraft crates/rvt/src/error.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Revit 文件哪里不对，说成人能照着做的话。说明照 BIMCraft 写英文（给人和 AI 代理读），不经 Tr
#pragma once

#include <string>

namespace MiniBIM::Rvt
{
    enum class ErrorKind
    {
        NotRevit,     // 不是复合文档，或是复合文档但不是 Revit 的
        TooBig,       // 超过 Limits 的某一项
        Damaged,      // 容器坏了
        NoStream,     // 没有这个名字的流
        NoGzip,       // 该有 gzip 的流里没有
        BadDeflate,   // 流里的一段 gzip 解不开
        TooMuch,      // 流解开后超过上限（压缩炸弹，或文件超出上限）
        Io,           // 读不了文件
        Format,       // 流在、也解得开，内容却不是 Revit 写的样子
    };

    struct Error
    {
        ErrorKind   kind = ErrorKind::Format;
        std::string message;
    };
}
