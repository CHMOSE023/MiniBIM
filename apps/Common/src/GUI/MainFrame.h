#pragma once
#include "Commands/Autosave.h"
#include "Commands/Session.h"
#include "GUI/AppPlatform.h"
#include "GUI/OpenQueue.h"
#include "GUI/Recent.h"
#include "GUI/RevitReport.h"
#include "GUI/Shortcuts.h"
#include "Model/Align.h"
#include "Model/Render.h"
#include "Model/Sketch.h"
#include "Model/Snap.h"
#include "View/ModelScene.h"
#include "View/OrbitCamera.h"
#include "Data/CommandRegistry.h"
#include "Data/Json.h"
#include "Render/IRenderBackend.h"
#include "Widgets/Menu.h"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace MiniGUI
{
    class UIContext;
    class DockSpace;
    enum class DockSide;
    class Label;
    class Node;
    class Popup;
    class TextBox;
    class TitleBar;
    class TreeView;
    class UiLayout;
    class ViewportHost;
    struct ViewportPointerEvent;
    struct KeyEvent;
}

namespace MiniBIM
{
    // MiniBIM 主窗口的界面与逻辑（与平台无关，平台服务经 AppPlatform）。结构沿用 MiniCAD 的 MainFrame：
    //   - 整个界面由 MiniGUI 绘制：标题栏自绘，菜单栏在标题栏里；菜单与布局由界面描述文件 ui/minibim_ui.json 描述（UiLayout），
    //     保存后自动重新加载
    //   - 模型：Bim::Session（移植自 BIMCraft 的命令与撤销），界面只经命令改它；启动时显示开始页（StartPage.cpp）
    //   - 停靠区（Panels.cpp）：特性面板 "properties"（类型选择器与选中元素的参数，改动经 modify 命令）、项目浏览器 "browser"
    //     （视图与族的树，双击打开视图）；功能区与快速访问工具栏在界面描述文件里
    //   - 工具（Tools.cpp）：功能区"建筑"里放东西的工具（墙、门、窗、构件、屋顶、楼板、天花板、房间、标高、轴网），
    //     在平面上点下去的点经捕捉（Bim::Snap），视图上画出会放下的东西；选项栏 "options" 是当前工具的选项
    //   - 三维视口（面板 "viewport"）：模型经 ModelScene 用实体内核建成三角形；OrbitCamera，拖动左键环绕、拖动中键或右键平移、
    //     Shift + 拖动环绕、滚轮缩放。"视图 > 楼层平面"把视口换成一个标高的平面（Bim::Render 的显示列表，PlanView.cpp）：
    //     拖动中键或右键平移、滚轮缩放、单击左键选中元素（Ctrl + 单击加进或拿出选择集，Esc 清空）
    //   - 状态栏（面板 "statusbar"）：操作提示、模型统计、语言、图形接口、版本
    //   - 多语言：界面文字经 MiniGUI::Tr / MiniBIM::Tr 翻译（I18n/Translator.h），"文件 > 语言"切换后整个界面按新语言重建；
    //     选择保存在 AppPlatform::GetUserDataDir() 下的 settings.json，没有保存过时跟随系统语言
    //
    // 帧顺序（同 MiniCAD）：输入到达时立即更新相机并请求重绘 → 宿主调用 RenderFrame：
    // UIContext::Update 布局 → ViewportHost::RenderContent 按最新尺寸和相机渲染视口 → UIContext::Render 画界面 → 呈现
    class MainFrame
    {
    public:
        MainFrame();
        ~MainFrame();

        // 界面描述文件（UTF-8 路径）；不设置时优先用源码目录里的文件，其次是可执行文件旁的 ui/minibim_ui.json
        void SetUiFile(std::string utf8Path) { m_uiPath = std::move(utf8Path); }
        // 启动参数指定的语言（--lang），优先于用户设置，不保存
        void SetLanguageOverride(std::string code) { m_languageOverride = std::move(code); }
        // 启动参数 --run 给的界面命令（如 view.plan）：打开示例房屋之后依次执行（不显示开始页），看界面效果时用
        void AddStartupCommand(std::string id) { m_startupCommands.push_back(std::move(id)); }
        // 启动参数给的项目文件（双击 .minibim 打开程序时）：打开它；没给文件也没给 --run 时启动后显示开始页
        void SetStartupFile(std::string utf8Path) { m_startupFile = std::move(utf8Path); }
        // Revit 文件的读取器：读 path，读进来的项目叫 name，读不了时原因放在 error。读取器（src/Rvt，Apache-2.0）主窗口不依赖：
        // 宿主链接了 MiniBIMRvt（MINIBIM_WITH_RVT）时接上 Rvt::ImportPath，在 Initialize 之前。没接时打开对话框里没有 .rvt，
        // 给了 .rvt 时说这个版本读不了。它在工作线程上运行（平台有后台线程时）
        using RevitReader = RevitReport::Reader;
        void SetRevitReader(RevitReader reader) { m_revitReader = std::move(reader); }
        bool HasRevitReader() const { return static_cast<bool>(m_revitReader); }
        // 正在读 Revit 文件
        bool ImportingRevit() const { return m_revitJob.has_value(); }
        struct RevitRead;                       // 工作线程交给界面线程的结果（Revit.cpp）
        // 系统要打开的文件（宿主收到拖进窗口的文件时调用）：同"打开"，先问未保存的改动；读 Revit 文件或有对话框时排队，
        // 之后只开最后一个（OpenQueue）
        void OpenFiles(const std::vector<std::string>& utf8Paths);

        // 平台对象必须比 MainFrame 活得久。尺寸为逻辑像素，pixelScale = 物理像素 / 逻辑像素
        bool Initialize(AppPlatform& platform, float width, float height, float pixelScale);

        // ── 由宿主调用 ───────────────────────────────────────────
        void OnResize(float width, float height, float pixelScale);
        void RenderFrame();
        void RequestExit();
        void CheckUiFileChanged();              // 界面描述文件变了就重新加载（宿主监视文件所在目录）

        const std::string&  GetUiPath() const { return m_uiPath; }
        MiniGUI::UIContext& GetUI() const { return *m_ui; }
        MiniGUI::TitleBar*  GetTitleBar() const;
        // 有没有没保存的改动（网页版关闭标签页前据此让浏览器询问）
        bool HasUnsavedChanges() const { return m_ui && Dirty(); }

        // 语言：code 为空 = 跟随系统。save 时写进用户设置
        void               SetLanguage(const std::string& code, bool save);
        const std::string& GetLanguageSetting() const { return m_languageSetting; }

    private:
        void InitUI();
        void RegisterCommands();                                  // MainCommands.cpp
        std::unique_ptr<MiniGUI::Node> CreateViewport();
        std::unique_ptr<MiniGUI::Node> CreateViewArea();
        void UpdateFitInsets(int pixelWidth, int pixelHeight);    // 视口被功能区、面板挡住的留边：框住模型、整个平面时用
        std::unique_ptr<MiniGUI::Node> CreateStatusBar();
        std::vector<MiniGUI::MenuItem> BuildLanguageMenu();
        // restore：重建后恢复用户的面板布局（换语言时）；界面描述文件改了时为 false，用文件里的布局
        void ReloadUi(bool restore = true);
        void RetranslateUi();                                     // 切换语言或布局后：重建界面描述生成的部分，刷新宿主自己的文字
        void UpdateTitle();
        void UpdateStatus();
        void ShowAbout();

        // ── 项目（MainCommands.cpp）────────────────────────────────
        // 执行模型命令；失败时弹出对话框（title 为英文原文），返回 false
        bool RunCommand(const char* id, const Bim::Json& params, const char* title, Bim::Json* result = nullptr);
        // 有没有保存过之后的改动（模型不是最后打开或保存的那一份）
        bool Dirty() const { return m_session.Snapshot() != m_savedModel; }
        // 有未保存的改动时先问保存、不保存还是取消，然后（没取消、保存成功时）做 then
        void Guard(std::function<void()> then);
        void NewProject();
        void OpenProject();
        // 打开一个文件（打开对话框选的、最近打开里的）：成功时记进最近打开，文件已经不在时从里面去掉
        bool OpenPath(const std::string& path);
        // 保存（另存为时、项目不是 .minibim 时先问存到哪里）；保存成功后做 then。问路径可能是异步的，所以不返回结果
        void SaveProject(bool saveAs, std::function<void()> then);
        // 默认文件名（不带扩展名）：项目文件的名字，没保存过时用项目名称
        std::string ProjectStem() const;
        // 问存到哪里（suggestedName 带扩展名），选好时调用 done；平台没有系统对话框时先用自己的对话框问文件名
        void AskSavePath(SaveKind kind, const std::string& suggestedName, std::function<void(const std::string&)> done);
        void StepHistory(bool redo);
        void DeleteSelection();
        void FlipSelection();
        // 模型导出为 IFC4 文件：问存到哪里，不改项目自己的路径
        void ExportIfc();
        void OpenSample();

        // ── Revit 文件（Revit.cpp）：在后台线程上读，提示框盖住窗口；读进来的是新的、没保存的项目 ──
        // 开始读（一次一个）；返回开始了没有。没有读取器时说为什么
        bool OpenRevit(const std::string& path);
        // 平台没有后台线程（网页版）：提示框画出来之后在界面线程上读
        void ReadRevit();
        // 读完了（界面线程）：换上读进来的项目，或说为什么不行
        void FinishRevit();
        // 退出时：不等读完，读完的结果没人要
        void AbandonRevit();
        void InstallRevit(const std::string& file, Bim::Ifc::Imported imported);
        // 文件 > Revit 导入报告：最后一次导入进来了什么、略去了什么与原因，项目还是那一个时才有
        bool HasRevitReport() const;
        void ShowRevitReport();
        // 排队的文件（OpenFiles）：没在读 Revit 文件、没有对话框时打开最后一个；忙时 RenderFrame 末尾再安排
        void TakeOpens();

        // 模型变了（命令、新建、打开）之后：重建三维场景，刷新标题与状态栏；换了项目时相机重新框住模型
        void OnModelChanged();
        void RebuildScene();

        // ── 选择集、特性面板、项目浏览器（Panels.cpp）─────────────────
        void SetSelection(std::vector<Bim::Id> ids);
        std::unique_ptr<MiniGUI::Node> CreateProperties();
        std::unique_ptr<MiniGUI::Node> CreateBrowser();
        // 模型、选择集或视图变了：下一轮消息循环里重建两块面板（不在控件自己的回调里拆掉它）
        void RefreshPanels();
        void BuildProperties();
        void BuildBrowser();
        // 特性面板里改了一个参数：经 modify 改选中的（或平面视图的标高）元素
        void EditParameter(const std::vector<Bim::Id>& ids, const std::string& key, Bim::Json value);
        void TogglePanel(const char* name);
        bool PanelShown(const char* name) const;

        // ── 面板的停靠与浮动（Palettes.cpp）──────────────────────────
        MiniGUI::DockSpace* MainDock() const;
        void AttachDock(bool restore);              // 界面描述文件应用之后：记下默认布局、恢复用户的布局、接上回调
        void ShowPanelMenu(const std::string& panel, MiniGUI::Vec2 at);   // 右键标签或浮动窗口的标题栏
        void DockPanelTo(const std::string& panel, MiniGUI::DockSide side);
        void FloatPanelAt(const std::string& panel, MiniGUI::Vec2 at);
        void ResetPanelLayout();
        void SavePanelLayout();                     // 布局变了：记下并写进 settings.json
        void RestorePanelLayout();

        void RenderViewport(int pixelWidth, int pixelHeight);
        void OnViewportPointer(const MiniGUI::ViewportPointerEvent& e);
        void HomeView();
        void ZoomToFit();

        // ── 平面视图（PlanView.cpp）──────────────────────────────
        void ShowModel3D();
        // 显示 level 的平面；没给时为正在显示的，或最低的标高
        void ShowPlan(std::optional<Bim::Guid> level);
        // 上一层（by = 1）、下一层（by = -1）的平面
        void StepLevel(int by);
        // 模型变了：显示的标高不在了就换到最低的，下次画时重画显示列表
        void OnPlanModelChanged();
        bool Selected(Bim::Id id) const;

        // ── 工具（Tools.cpp）──────────────────────────────────────
        // 平面上单击做什么：选（修改），或放一类东西
        // Stand 放结构柱或独立基础、Run 放梁、条形基础或别的 run，类别在 m_toolOf（照 BIMCraft 的 Tool::Stand、Tool::Run）
        enum class Tool { Modify, Wall, Door, Window, Column, Component, Roof, Floor, Ceiling, Room, Level, Grid, Stand, Run,
                          Move, Copy, Rotate, MirrorPick, MirrorDraw, Align, Trim, Split, Offset,
                          SketchLine, SketchRect, SketchPick, Mass };
        // 改选择集的工具（移动、复制、旋转、镜像）要先选东西
        static bool NeedsSelection(Tool t) { return t == Tool::Move || t == Tool::Copy || t == Tool::Rotate || t == Tool::MirrorPick || t == Tool::MirrorDraw; }
        // 画草图线的工具（只在草图模式里）
        static bool SketchTool(Tool t) { return t == Tool::SketchLine || t == Tool::SketchRect || t == Tool::SketchPick; }
        void SetTool(Tool tool);
        // 放类别 category 的 Stand 或 Run 工具；已是它时回到修改
        void SetToolOf(Tool tool, const char* category);
        // Run 工具：换类别时选项栏的对齐与偏移换成这个工具上次的，或它的默认
        void KeepRunOptions(const char* category);
        // run 与 Stand 工具的命令参数（选项栏的高度、选的类型）；读不出高度时为空，原因放在 error
        std::optional<Bim::Json> RunParams(Bim::P2 a, Bim::P2 b, std::string& error) const;
        std::optional<Bim::Json> StandParams(Bim::P2 at, std::string& error) const;
        // Esc：先丢掉输入的长度，再丢掉画了一半的（墙链、轴网的起点），再退出工具，最后清空选择集
        void Escape();
        std::unique_ptr<MiniGUI::Node> CreateOptionsBar();
        void BuildOptionsBar();
        // 工具放的元素的类别（"wall"…）：特性面板没选东西时显示"新建 …"与它的类型；修改与标高为空
        const char* ToolCategory() const;
        // 状态栏的提示：工具要的下一步（英文原文，显示前 Tr）
        const char* ToolPrompt() const;
        void ToolClick(MiniGUI::Vec2 pixel, uint8_t modifiers);
        bool OnPlanKey(const MiniGUI::KeyEvent& e);
        bool OnPlanText(std::string_view text);
        // 输入的长度：从工具的上一点朝光标（落在引导线上时沿它）量这么长，当作点下的点
        void PlaceTyped();
        void PlacePoint(Bim::P2 p);
        Bim::Snap::Found SnapAt(MiniGUI::Vec2 pixel, std::span<const Bim::P2> from, bool square) const;
        // 平面上画在模型之上的：会放下的东西、捕捉的记号与引导线、长度与角度（像素坐标的显示列表）
        void BuildPreview(Bim::Render::Drawing& d);
        // 选项栏与特性面板选的类型（和门窗的族）放进命令参数
        Bim::Json Typed(const std::string& category, Bim::Json p) const;
        // 墙、柱的顶部（选项栏的"高度"）：选的标高（没选时上一层），或未连接时输入的高度；读不出高度时为空，原因放在 error
        std::optional<Bim::Json> TopParams(const std::optional<std::optional<Bim::Guid>>& top, const std::string& height, std::string& error) const;
        // 构件工具放的族与类型：选的，否则第一个可载入族的第一个类型
        std::optional<std::pair<std::string, std::string>> Placing() const;
        // 构件工具能放这一类的族吗：卫浴、电气等设备的按钮（m_toolOf 为设备的键）只放它的几类
        bool PlacingFits(Bim::Fam::Category c) const;
        // 体量工具：点过的角点围成底面轮廓，按选项栏的顶部做体量
        void FinishMass();
        // 选中的体量；体量选项卡"按面建模"的按钮对每个选中的体量执行 command（移植自 BIMCraft 的 by_mass）
        std::vector<Bim::Id> SelectedMasses() const;
        void ByMass(const char* command, const char* title);

        // ── 修改（Modify.cpp）：框选、Tab 切换、拖动、夹点、修改工具 ──────────
        // 光标下的元素：几个叠在一起时按 Tab 轮换
        std::optional<Bim::Id> Hovered() const;
        // 光标下离它最近的一条直线（镜像的轴、对齐的线）：光标附近各元素的对齐线里最近的，Tab 轮换
        std::optional<std::pair<Bim::Id, Bim::Align::Line>> HoveredLine() const;
        // 光标下的墙（修剪、拆分、偏移只认墙）
        std::optional<Bim::Id> HoveredWall() const;
        // 左键在修改工具下按下：夹点、拖动元素或框选，等拖出几个像素才开始
        void BeginPlanDrag(MiniGUI::Vec2 pixel);
        void UpdatePlanDrag(MiniGUI::Vec2 pixel);
        void FinishPlanDrag(MiniGUI::Vec2 pixel, uint8_t modifiers);
        // 单击选中（修改工具下）：Ctrl 加进或拿出
        void PickAt(MiniGUI::Vec2 pixel, uint8_t modifiers);
        // 修改工具（移动、复制、旋转、镜像、对齐、修剪、拆分、偏移）的单击
        void ModifyClick(MiniGUI::Vec2 pixel, uint8_t modifiers);
        void BuildModifyPreview(Bim::Render::Drawing& d, std::string& tip);
        // 捕捉到的点的记号与引导线（读数 tip 写捕捉到什么）；返回那个点
        Bim::P2 DrawSnap(Bim::Render::Drawing& d, const Bim::Snap::Found& f, std::string& tip);
        // 选择集的中心（外框的中点）：旋转的默认中心
        std::optional<Bim::P2> SelectionMiddle() const;
        // 旋转或镜像之后回到修改，复制的选中副本
        void Edited(const Bim::Json& made, bool copied);

        // ── 对话框（Dialogs.cpp）：类型属性、族类型、材质 ─────────────
        // 类型属性编辑的类型：类别与类型名，族的门窗与构件还有族；以及选中的元素（没选时空，编辑工具要放的类型）
        struct TypeSubject
        {
            std::string category;
            std::string name;
            std::optional<std::string> family;
            std::vector<Bim::Id> elements;
        };
        std::optional<TypeSubject> CurrentTypeSubject() const;
        void EditType();
        void OpenTypeDialog(const TypeSubject& t);
        void OpenFamilyDialog(const TypeSubject& t);
        void OpenMaterials();
        // 执行模型命令，失败时不弹窗，原因放进 error（对话框里显示）
        bool RunQuiet(const char* id, const Bim::Json& params, std::string& error, Bim::Json* result = nullptr);

        // ── 草图模式（SketchMode.cpp）：编辑边界、画新楼板 / 天花板 / 屋顶的轮廓 ──
        enum class SketchOf { Floor, Ceiling, Roof, Edit };
        struct SketchState
        {
            SketchOf of = SketchOf::Edit;
            Bim::Id  edit = 0;                              // 编辑边界的元素
            Bim::Guid level;                                // 它在哪个标高的平面上开始（完成前不离开）
            std::vector<Bim::SketchLine> lines;
            std::set<size_t>    picked;                     // 修改工具下选中的线（删除、夹点）
            std::vector<size_t> wrong;                      // 上一次完成时说有问题的线（标红，改了草图就清掉）
            std::vector<std::vector<Bim::SketchLine>> history;   // 每次改动前的线，Ctrl+Z 退回一步
            std::optional<Bim::P2> grip;                    // 正在拖的线端
        };
        bool Sketching() const { return m_sketch.has_value(); }
        void StartSketch(SketchOf of, Bim::Id edit, std::vector<Bim::SketchLine> lines);
        void EditBoundary();
        bool CanEditBoundary() const;
        void FinishSketch();
        void CancelSketch();
        // 改草图之前调：记下现在的线（Ctrl+Z 用），清掉选中与标红
        void SketchChanging();
        void UndoSketch();
        void DeleteSketchLines();
        void SketchClick(MiniGUI::Vec2 pixel, uint8_t modifiers);
        // 直线工具点下（或输入长度得到）的点：从上一点画一条，回到起点时环闭合
        void SketchPoint(Bim::P2 p);
        // 修改工具下：在草图线上单击选中（Ctrl 加减），拖选中线的端点（夹点），空白处框选
        void BeginSketchDrag(MiniGUI::Vec2 pixel);
        void FinishSketchDrag(MiniGUI::Vec2 pixel, uint8_t modifiers, bool dragged);
        // 草图线与草图工具的预览（像素坐标）
        void BuildSketchPreview(Bim::Render::Drawing& d, std::string& tip);
        std::optional<size_t> SketchLineAt(MiniGUI::Vec2 pixel) const;
        // 说给画的人听：每种问题一行，前几处在哪（项目单位）、哪几条线
        std::string FriendlyProblems(const std::vector<Bim::Sketch::Problem>& problems) const;
        // 草图模式里要离开它的平面（别的标高、三维）：说不行，返回 true
        bool LeavingSketch();
        void RenderPlan(IRenderer& renderer, int pixelWidth, int pixelHeight);
        void OnPlanPointer(const MiniGUI::ViewportPointerEvent& e);
        // 平面的名称（标高名），以及选中的元素：状态栏用
        std::string PlanStatus() const;

        void LoadSettings();
        void SaveSettings() const;
        // 文件 > 设置（Settings.cpp）；主题按 m_themeSetting
        void OpenSettings();
        void ApplyTheme();
        void SetCompactLayout(bool compact);                      // 布局：紧凑 / 浮动（默认），存进设置并重建界面
        // 两个字母的快捷键（ShortcutsDialog.cpp）：默认的表、在视图上打的字母、键盘快捷键对话框（KS）
        void RegisterShortcuts();
        bool OnShortcutText(std::string_view text);
        void OpenShortcuts();
        // 快捷键文件里的说明是哪个界面命令：名称（英文原文或译文，不分大小写）或 ID
        std::string ShortcutCommand(const std::string& description) const;
        std::string SettingsPath() const;

        // ── 最近打开（Recent.h，存在用户设置目录的 recent.json）──────────
        // path 是刚打开或保存的文件：成为列表里最新的一项
        void NoteRecent(const std::string& path);
        void KeepRecent();
        std::vector<MiniGUI::MenuItem> BuildRecentMenu();

        // ── 开始页（StartPage.cpp）：盖在标题栏下面的整个窗口上 ──────────
        void CreateStartPage();
        // over：叫到项目上面（有"回到项目"，Ctrl+D 回去）；启动时打开的没有项目可回
        void ShowStartPage(bool show, bool over);
        void ToggleStartPage();
        void BuildStartPage();
        void PostStartPage();
        // 右边一栏一行放几张卡片：窗口宽度变了、放的张数也变了时重建
        int StartColumns() const;

        // ── 自动保存与恢复（Autosave.cpp；写文件的是核心的 Bim::Autosave::Worker）──────────
        // 没保存的改动每隔几分钟写一份恢复副本：保存过的项目写在它旁边（<项目>.autosave.minibim），
        // 没保存过的写在临时文件夹的 MiniBIM/ 里。保存后、不保存就关掉时删掉；启动或打开项目时找到了就问恢复、丢弃还是以后再说
        void EnableAutosave();
        // 换了项目（新建、打开、示例房屋）：上一个的副本删掉，新的写到哪里，找它旁边的副本
        void AutosaveDocument();
        // 保存成功之后：副本删掉，以后写到新路径旁边
        void AutosaveSaved();
        // 删掉这份文档的副本（关掉、换项目时；还在问要不要恢复的不删）
        void RetireAutosave();
        void ScheduleAutosave();
        void AutosaveTick();
        void PollAutosave();
        void ShowRecovery();
        void RecoverBackup(const Bim::Autosave::Backup& backup);
        void SelectAutosaveDestination();

        AppPlatform* m_platform = nullptr;
        float        m_dpiScale = 1.0f;

        std::unique_ptr<MiniGUI::UIContext> m_ui;
        MiniGUI::CommandRegistry            m_commands;
        std::unique_ptr<MiniGUI::UiLayout>  m_layout;
        MiniGUI::Node*                      m_appHost    = nullptr;  // m_uiHost 与开始页
        MiniGUI::Node*                      m_uiHost     = nullptr;
        MiniGUI::ViewportHost*              m_viewport   = nullptr;   // 面板 "canvas"：铺满标题栏以下，其余的叠在它上面
        MiniGUI::Node*                      m_viewArea   = nullptr;   // 面板 "viewport"：停靠区里视图的位置，透明的"洞"（没被挡住的部分）
        double                              m_fitInsets[4] = { 0.0, 0.0, 0.0, 0.0 };   // 视口被挡住的留边（物理像素，左、上、右、下）
        MiniGUI::Label*                     m_statusHint = nullptr;
        MiniGUI::Label*                     m_statusInfo = nullptr;

        std::string                     m_uiPath;
        std::filesystem::file_time_type m_uiFileTime{};
        std::string                     m_uiError;            // 界面描述文件读不出或写错时的原因（状态栏显示）

        std::string m_languageSetting;                        // 用户选的语言，空 = 跟随系统
        std::string m_themeSetting = "dark";                  // 界面主题：dark、light
        bool        m_compactLayout = false;                  // 布局（settings.json 的 "layout"）：false = 浮动，各部分是卡片；true = 紧凑，紧贴窗口边缘
        MiniGUI::JsonValue       m_dockLayout;                // 用户的面板布局（settings.json 的 "dock"）；空 = 用界面描述文件的
        std::vector<std::string> m_dockPanels;                // 保存布局时有的面板（"dock_panels"）：之后新增的面板据此识别
        MiniGUI::JsonValue       m_dockDefault;               // 界面描述文件里的布局
        std::string m_languageOverride;
        std::vector<std::string> m_startupCommands;
        RecentFiles m_recent;
        Shortcuts   m_shortcuts;
        std::string m_recentPath;                             // 空 = 不记（平台没有用户设置目录）
        MiniGUI::Node* m_startPage = nullptr;
        bool m_startShown = false;
        bool m_startOver  = false;
        int  m_startColumns = 0;
        std::string m_startupFile;                            // 启动参数给的项目文件：打开它，不显示开始页

        struct RevitJob
        {
            std::string path;
            std::string file;                                 // 文件名（提示框、报错）
            std::shared_ptr<RevitRead> read;
            MiniGUI::Popup* box = nullptr;                    // 提示框
            bool here  = false;                               // 没有后台线程：在界面线程上读
            bool drawn = false;                               // here 时：提示框画出来了，接着读
        };
        RevitReader m_revitReader;
        std::optional<RevitJob> m_revitJob;
        std::optional<RevitReport::Report> m_revitReport;     // 最后一次导入的报告
        size_t m_revitResets = 0;                             // 报告说的是哪一个项目（Session::Resets）
        std::shared_ptr<const Bim::Model> m_revitModel;      // 读进来的那一份：模型还是它时状态栏说进来了多少
        OpenQueue m_opens;                                    // 拖进窗口、还没打开的文件（OpenFiles）
        bool m_opensPosted = false;                           // 已经安排了 TakeOpens

        struct AutosaveState
        {
            std::unique_ptr<Bim::Autosave::Worker> worker;
            std::string root;                                 // 临时文件夹里的 MiniBIM/
            std::string temporary;                            // 这份没保存过的文档的副本
            std::optional<std::string> project;               // 副本属于哪个项目文件
            std::optional<std::string> active;                // 现在往哪里写
            int minutes = 2;                                  // 间隔（分钟），0 = 不自动保存（设置）
            std::weak_ptr<const Bim::Model> snapshot;         // 最后写下（或正在写）的那一份
            uint64_t generation = 0;                          // 换一次文档加一：之前的查找结果作废
            int discovering = 0;                              // 还没回来的查找
            std::vector<Bim::Autosave::Backup> candidates;    // 找到的、还没决定的副本（不覆盖、不删）
            std::set<std::string> postponed;                  // 这次说了"以后再说"的
            std::optional<std::string> deleting;              // 点了丢弃、正在删的
            std::vector<std::string> retired;                 // 换了地方之后旧的副本：新的一份写好或保存后再删
            bool asking = false;                              // 恢复的对话框开着
            uint64_t timer = 0;                               // 定时器的代号：改了间隔时旧的作废
        };
        AutosaveState m_autosave;

        Bim::Session m_session;
        std::shared_ptr<const Bim::Model> m_savedModel;      // 最后打开、新建或保存的那一份：别的都是没保存的
        std::shared_ptr<const Bim::Model> m_sceneModel;      // 场景是按哪一份模型建的
        size_t       m_sceneResets = 0;                      // 场景建好时项目换过几次：换了项目时相机重新框住模型
        uint64_t     m_sceneVersion = 0;                     // 交给 IRenderer::SubmitCached：场景重建后加一，后端重新上传

        OrbitCamera m_camera;
        bool        m_cameraFitted = false;
        ModelScene  m_scene;
        std::string m_sceneError;
        std::vector<Vertex_P3_C4> m_edgeVerts;                // 每帧按视线方向把边往相机挪一点再画（免得与面争深度）

        MiniGUI::TextureId m_viewTex = MiniGUI::InvalidTextureId;
        void*              m_viewSRV = nullptr;

        enum class Drag { None, Orbit, Pan };
        Drag         m_drag = Drag::None;
        MiniGUI::Vec2 m_lastPixel;

        std::optional<Bim::Guid>         m_planLevel;        // 显示哪个标高的平面；空 = 三维
        std::optional<Bim::Render::View> m_planView;         // 平面的比例与中心；空 = 下次画时框住整个平面
        std::unique_ptr<Bim::Render::Drawing> m_plan;        // 显示列表，按 m_planView 画的
        bool                             m_planDirty = true; // 模型或视图变了，要重画显示列表
        std::vector<Bim::Id>             m_selection;        // 选中的元素（平面上单击、项目浏览器）
        bool                             m_planClick = false;// 左键按下后没拖动：松开时拾取
        MiniGUI::Vec2                    m_downPixel;
        std::vector<Vertex_P3_C4>        m_planVerts;

        MiniGUI::Node*     m_propsBody   = nullptr;          // 特性面板里随选择集重建的部分
        MiniGUI::Node*     m_browserBody = nullptr;          // 项目浏览器：搜索框下面的树
        MiniGUI::TreeView* m_browserTree = nullptr;
        std::string        m_browserFind;                    // 搜索框里的文字
        std::set<std::string> m_collapsedGroups;             // 特性面板里收起的组（英文原文）
        std::map<std::string, bool> m_branchOpen;            // 项目浏览器里各分支（按路径）展开着没有：重建时照旧
        std::vector<std::function<void()>> m_browserActs;    // 树上各项双击时做什么（TreeItem 的 UserData 是下标 + 1）
        std::vector<Bim::Json>             m_browserEntries; // 同一下标：类型或族，右键删除用（delete_type 的参数），别的为 null
        bool               m_panelsPosted = false;

        Tool                 m_tool = Tool::Modify;
        std::vector<Bim::P2> m_toolPts;                     // 墙链点过的点、轴网的起点、体量点过的角点
        std::optional<MiniGUI::Vec2> m_hoverPixel;          // 光标在平面上的位置（预览用）
        bool                 m_hoverShift = false;
        std::string          m_typedLength;                 // 在视图里输入的长度，Enter 时用
        Bim::Snap::Snaps     m_snaps;
        std::optional<Bim::Snap::Found> m_found;            // 最后一次预览捕捉到的点
        // 选项栏的设置（同 BIMCraft，值是输入框里的文字，用时再读）
        struct ToolOptions
        {
            std::optional<std::optional<Bim::Guid>> wallTop;    // 没设：上一层；空：未连接
            std::string   wallHeight   = "3000";
            Bim::Location wallLocation = Bim::Location::Centre;
            bool          chain        = true;
            std::optional<std::optional<Bim::Guid>> columnTop;  // 柱的顶部，同 wallTop
            std::string   columnHeight = "3000";
            std::optional<std::optional<Bim::Guid>> massTop;    // 体量的顶部，同 wallTop
            std::string   massHeight   = "3000";
            std::string   roofSlope    = "30";
            std::string   roofOverhang = "500";
            bool          roofGable    = false;
            std::string   ceilingHeight = "2700";
            std::string   levelElevation;
            std::string   levelName;
            double        componentAngle = 0.0;
            bool          floorStructural = false;              // 楼板工具放结构楼板（结构选项卡的"楼板：结构"）
            std::string   footingThickness = "400";
            std::string   footingTop   = "0";                   // 独立基础、条形基础的顶在标高上多高
            Bim::Justify  runJustify   = Bim::Justify::Top;     // run 工具：高度在截面哪条线上，与它的高度
            std::string   runOffset    = "0";
            const char*   runLoaded    = "beam";                // 上面两项是哪个 run 工具的
            std::map<std::string, std::pair<Bim::Justify, std::string>> runKept;   // 别的 run 工具上次的
            std::map<std::string, std::string> types;            // 类别 → 类型
            std::map<std::string, std::string> openingFamilies;  // door / window → 族（选了项目自己的类型时没有）
            std::optional<std::pair<std::string, std::string>> component;
        } m_toolOpt;
        const char*     m_toolOf = nullptr;                 // Stand、Run 工具放的类别；构件工具放的设备（MEP 的按钮）
        MiniGUI::Node*  m_optionsBody = nullptr;
        // 修改工具的状态
        std::optional<Bim::P2>          m_rotateCentre;     // 旋转中心：选工具时为选择集的中心
        bool                            m_placeCentre = false;  // 下一下单击放旋转中心
        std::optional<Bim::Align::Line> m_alignRef;         // 对齐：已拾取的参照线
        std::optional<std::pair<Bim::Id, Bim::P2>> m_trimFirst;  // 修剪到角：第一面墙与拾取处（保留的那部分）
        struct ModifyOptions
        {
            bool        constrain  = false;
            bool        multiple   = false;
            bool        rotateCopy = false;
            std::string rotateAngle;
            bool        mirrorCopy = true;
            std::string offset     = "1000";
            bool        offsetCopy = true;
        } m_modifyOpt;
        size_t m_tabIndex = 0;                              // 光标下几个元素时 Tab 轮到第几个
        // 修改工具下左键拖动：框选、拖动选中的元素、拖墙端的夹点
        enum class PlanDrag { None, Box, Move, Grip };
        PlanDrag                        m_planDrag = PlanDrag::None;
        PlanDrag                        m_pendingDrag = PlanDrag::None;
        std::optional<std::pair<Bim::Id, bool>> m_grip;     // 拖的是哪面墙的哪一端（true 为起点）
        std::optional<Bim::Id>          m_dragElement;      // 按下时光标下的元素：拖动时没选中就先选中它
        MiniGUI::Vec2                   m_dragNow;
        std::optional<SketchState> m_sketch;
        bool            m_sketchBox = false;                // 草图模式下修改工具的拖动是框选（不是夹点）
        MiniGUI::Node*  m_planTipBox  = nullptr;            // 平面上跟着光标的读数（长度、角度、面积、捕捉到什么）
        MiniGUI::Label* m_planTip     = nullptr;
    };
}
