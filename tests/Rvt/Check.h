// RvtTests 各个源文件共用的检查与测试组。MiniBIM 为 Revit 读取器的测试写的，随 src/Rvt 以 Apache-2.0 发布，见 src/Rvt/NOTICE
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace RvtTest
{
    inline int g_failed = 0;

    inline void Check(bool ok, const std::string& label)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
        g_failed += ok ? 0 : 1;
    }

    inline bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // 与 Rust 测试一样的 xorshift
    struct Xorshift
    {
        uint64_t seed;
        uint64_t operator()()
        {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            return seed;
        }
    };

    // 记录层（R4）的测试组：RecordTests.cpp 与 DecoderTests.cpp
    void TestSchema();
    void TestRecords();
    void TestInventory();
    void TestDecoders();
    void TestSketch();
}
