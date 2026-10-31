// 移植自 BIMCraft crates/rvt/src/import.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// Revit 项目读成 MiniBIM 的模型，同 IFC 导入：经模型自己的构造与 Model::Check，列出略去了什么与原因。
//
// Revit 文件的长度是英尺，MiniBIM 的是毫米（x 向东、y 向北、z 向上，同 Revit 的项目坐标）。读取器只照文件的记录说的建：
// 几何读不出的元素略去并列出。MiniBIM 的元素是参数化的，所以只在一组参数如实描述文件里的东西时才建元素；
// 记录自己的包围盒能给出替身时（幕墙是它包围盒的平面、带坡度的楼板放平、什么也围不住的草图是它包围盒的轮廓）
// 元素以替身进来，并说明哪里没读：那是近似，不算读全，也不用来减少"略去"的数目。
// 读两遍文件：第一遍找到记录（也就知道要读哪些 id），第二遍读它们的数据
#pragma once

#include "Ifc/Ifc.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace MiniBIM::Rvt
{
    class Revit;

    // bytes 里的 Revit 项目读成模型，以项目的标题（没有时为 name）命名；读不了时原因放在 error
    bool Import(std::span<const uint8_t> bytes, std::string_view name, Bim::Ifc::Imported& out, std::string& error);
    // 读磁盘上 path（UTF-8）的 Revit 项目：大文件不在内存里存两份
    bool ImportPath(const std::string& path, std::string_view name, Bim::Ifc::Imported& out, std::string& error);
    // 同 ImportPath，以及建出的每个元素在文件里是什么（Wall 423099）：拿导入与别的读取器对同一文件的导出比对
    bool ImportPathWithOrigins(const std::string& path, std::string_view name, Bim::Ifc::Imported& out, std::map<Bim::Id, std::string>& from,
                               std::string& error);
    // 打开的文件的导入，以及建出的每个元素在文件里是什么
    bool Build(Revit& r, std::string_view name, Bim::Ifc::Imported& out, std::map<Bim::Id, std::string>& from, std::string& error);

    // 元素在文件里是什么（Wall 423099、Structural column 12、Level 'Roof'）：小写的类，以及 Revit id；
    // 标高有名字没有 id，是照写的名字
    std::optional<std::pair<std::string, std::variant<uint32_t, std::string>>> Origin(std::string_view from);
}
