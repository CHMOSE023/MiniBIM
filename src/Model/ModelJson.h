// 移植自 BIMCraft crates/core/src/model.rs、ids.rs、library.rs、lenient.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Model.h"
#include <string>
#include <vector>

// 元素与模型的 JSON：字段名、写法与 BIMCraft 的 serde 相同（元素是带 "category" 的对象，引用写 GUID，
// 还是 ID 的引用写数）。命令读元素时严格：拼错的键报错；读文件时宽松：本版本不认识的内容略去并列出（Model::Read）
namespace MiniBIM::Bim
{
    Json ToJson(Guid g);
    Json ToJson(P2 p);
    Json ToJson(const Element& e);
    // 草图线（文件与命令里的写法，同 BIMCraft：{id?, guid?, start, end, arc?}）
    Json SketchJson(const std::vector<SketchLine>& lines);
    // 类型与材质（用名称，不是 GUID）：inspect 显示它们
    Json ToJson(const Types& t);
    Json ToJson(const std::map<std::string, Material>& materials);

    // GUID：数（ID）或 GUID 字符串
    bool FromJson(const Json& v, Guid& out, std::string& error);
    // [x, y]
    bool FromJson(const Json& v, P2& out, std::string& error);
    // 严格地读一个元素：不认识的键、类别报错
    bool FromJson(const Json& v, Element& out, std::string& error);
    // 族：字段与写法同 BIMCraft 的 family.rs；读时严格
    Json ToJson(const Family& f);
    bool FromJson(const Json& v, Family& out, std::string& error);
}
