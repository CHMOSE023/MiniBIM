// 移植自 BIMCraft crates/rvt/src/tests/mod.rs、walls.rs 与 floors.rs 的辅助函数（f3bbb33），Apache-2.0 许可（不是 MIT），
// 已改写为 C++，见 src/Rvt/NOTICE
#include "ImportFixture.h"

#include "Rvt/Container.h"
#include "Rvt/Import.h"

#include <algorithm>

namespace Rvt = MiniBIM::Rvt;
using ProjectFixture::Block;
using ProjectFixture::Lev;
using ProjectFixture::Mat;
using ProjectFixture::Rec;
using ProjectFixture::TypeRec;
using RvtTest::Check;
using RvtTest::Contains;

namespace ImportFixture
{
    Imported Import(const Project& p)
    {
        std::string error;
        if (auto i = TryImport(p.File(), &error))
            return std::move(*i);
        Check(false, "导入合成的项目：" + error);
        return {};
    }

    std::optional<Imported> TryImport(const Bytes& file, std::string* error)
    {
        Imported out;
        std::string why;
        if (Rvt::Import(file, "house", out, why))
            return out;
        if (error)
            *error = why;
        return std::nullopt;
    }

    std::pair<Imported, std::map<Bim::Id, std::string>> ImportWithOrigins(const Project& p)
    {
        Rvt::Revit r;
        Rvt::Error e;
        Imported out;
        std::map<Bim::Id, std::string> from;
        if (!Rvt::Revit::FromBytes(p.File(), Rvt::Limits{}, r, e))
            Check(false, "打开合成的项目：" + e.message);
        else if (std::string error; !Rvt::Build(r, "house", out, from, error))
            Check(false, "导入合成的项目：" + error);
        return { std::move(out), std::move(from) };
    }

    Bim::Guid LevelNamed(const Imported& i, const std::string& name)
    {
        for (const auto& [id, l] : i.model.Levels())
            if (l->name == name)
                return i.model.GuidOf(id);
        Check(false, "没有名为 " + name + " 的标高");
        return {};
    }

    std::vector<std::pair<std::string, double>> LevelList(const Imported& i)
    {
        std::vector<std::pair<std::string, double>> out;
        for (const auto& [_, l] : i.model.Levels())
            out.emplace_back(l->name, l->elevation);
        return out;
    }

    std::string Said(const Imported& i, const std::string& what)
    {
        for (const std::string& s : i.skipped)
            if (Contains(s, what))
                return s;
        return {};
    }

    bool Says(const Imported& i, std::initializer_list<std::string> parts)
    {
        return std::any_of(i.skipped.begin(), i.skipped.end(),
                           [&](const std::string& s) { return std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return Contains(s, p); }); });
    }

    std::string Skipped(const Imported& i)
    {
        std::string out = "：[";
        for (size_t k = 0; k < i.skipped.size(); ++k)
            out += (k ? " | " : "") + i.skipped[k];
        return out + "]";
    }

    bool Checks(const Imported& i)
    {
        std::string error;
        return i.model.Check(error);
    }

    Project Base(uint32_t release)
    {
        Project p(release);
        p.levels = { Lev(10, "Level 1", 0.0), Lev(11, "Level 2", 10.0) };
        p.mats   = { Mat{ 700, "Brick" }, Mat{ 701, "Concrete" } };
        p.types  = { TypeRec(500, Walls), TypeRec(700, Materials), TypeRec(701, Materials) };
        p.types[1].kind = ProjectFixture::KindMaterial;
        p.types[2].kind = ProjectFixture::KindMaterial;
        p.data = { Block{ 500, Fixture::Concat({ ProjectFixture::NameBytes("Brick wall"), ProjectFixture::LayersBytes({ { 0.25, 700, 4 }, { 0.5, 701, 1 } }, release) }) } };
        return p;
    }

    void Straight(Project& p, uint32_t id, bool flip)
    {
        // Revit 2015 到 2020 不写那个字
        const bool word = !(p.release >= 2015 && p.release <= 2020);
        p.recs.push_back(Rec(id, Walls).At({ 0, -0.375, 0, 20, 0.375, 10 }, { 10, 11, 500, id }));
        p.data.push_back(Block{ id, Fixture::Concat({ ProjectFixture::LineBytes(0, 20, { 0, 0, 0 }, { 1, 0, 0 }), ProjectFixture::OrientationBytes(0, 1, flip, word) }) });
    }

    void WallOf(Project& p, uint32_t id, const Box& bbox, std::vector<uint64_t> refs, Bytes line)
    {
        p.recs.push_back(Rec(id, Walls).At(bbox, std::move(refs)));
        p.data.push_back(Block{ id, Fixture::Concat({ line, ProjectFixture::OrientationBytes(0, 1, false, true) }) });
    }

    Project FloorProject(uint32_t release)
    {
        Project p = Base(release);
        p.types.push_back(TypeRec(510, Floors));
        p.data.push_back(Block{ 510, Fixture::Concat({ ProjectFixture::NameBytes("Slab"), ProjectFixture::LayersBytes({ { 0.5, 701, 1 } }, release) }) });
        p.types.push_back(TypeRec(520, Ceilings));
        p.data.push_back(Block{ 520, Fixture::Concat({ ProjectFixture::NameBytes("Tiles"), ProjectFixture::LayersBytes({ { 0.25, 700, 4 } }, release) }) });
        return p;
    }

    void Line(Project& p, uint32_t owner, uint32_t id, std::pair<double, double> a, std::pair<double, double> b, double z, bool data)
    {
        const double dx = b.first - a.first, dy = b.second - a.second;
        const double len = std::hypot(dx, dy);
        const Box bbox{ std::min(a.first, b.first), std::min(a.second, b.second), z, std::max(a.first, b.first), std::max(a.second, b.second), z };
        Rec r = Rec(id, Sketch).At(bbox, { 1 });
        r.refs2 = { 30, owner };
        p.recs.push_back(r);
        if (data)
            p.data.push_back(Block{ id, ProjectFixture::LineBytes(0, len, { a.first, a.second, z }, { dx / len, dy / len, 0 }) });
    }

    void Rectangle(Project& p, uint32_t owner, uint32_t first, std::array<double, 4> r, double z, bool data)
    {
        const auto [x0, y0, x1, y1] = r;
        const std::pair<double, double> corners[4][2] = { { { x0, y0 }, { x1, y0 } }, { { x1, y0 }, { x1, y1 } }, { { x1, y1 }, { x0, y1 } }, { { x0, y1 }, { x0, y0 } } };
        for (uint32_t k = 0; k < 4; ++k)
            Line(p, owner, first + k, corners[k][0], corners[k][1], z, data);
    }

    std::pair<size_t, size_t> Sweep(const Project& p, uint64_t seed, size_t rounds, const std::string& label, const std::function<bool(const Imported&)>& check)
    {
        const Bytes bytes = p.PartitionBytes();
        RvtTest::Xorshift next{ seed };
        size_t ok = 0, failed = 0;
        std::optional<size_t> wrong;
        for (size_t round = 0; round < rounds; ++round)
        {
            Bytes bad = bytes;
            for (uint64_t k = 0, n = 1 + next() % 8; k < n; ++k)
            {
                const size_t at = static_cast<size_t>(next() % bad.size());
                switch (next() % 4)
                {
                case 0: bad[at] = 0xff; break;
                case 1: bad[at] = 0; break;
                default: bad[at] = static_cast<uint8_t>(next()); break;
                }
            }
            if (round % 7 == 0)
                bad.resize(bad.size() - std::min<size_t>(bad.size(), static_cast<size_t>(next() % 400)));
            const auto i = TryImport(p.FileFrom(bad));
            if (!i)
            {
                ++failed;
                continue;
            }
            ++ok;
            if (!wrong && !(Checks(*i) && check(*i)))
                wrong = round;
        }
        Check(!wrong, label + (wrong ? "（第 " + std::to_string(*wrong) + " 份不对）" : "") + "，" + std::to_string(ok) + " 份读得出");
        return { ok, failed };
    }
}
