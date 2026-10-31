---
covers: src/View/*, src/Solid/*, apps/Render/*, src/Render/*
---

# 三维视口

## 概述

主窗口的视口（面板 `canvas`，铺满标题栏以下，见 main-window.md 的"视图铺满"）显示三维模型：`OrbitCamera`（`src/View/OrbitCamera.h`）给出视图投影矩阵，`MainFrame::RenderViewport` 用
`IRenderer`（`src/Render/IRenderer.h`，D3D12 / D3D11、Linux 的 Vulkan / GLES3、网页的 WebGPU / WebGL2 实现在 `apps/Render`，都复制自 MiniCAD）画进离屏渲染目标，再作为纹理交给 `MiniGUI::ViewportHost` 显示。
几何只经 `Solid::ISolidKernel`（SMLib）得到：模型（`MainFrame` 的 `Bim::Session`，`.agents/features/model.md`）由 `ModelScene`（`src/View/ModelScene.h`）
建成实体，再离散成三角形与边线。

## 入口与数据流

- 相机：正交投影、绕目标环绕；世界坐标 Z 向上、单位毫米。方位角自 +X 逆时针，仰角限制在 ±89°。`OrbitCamera::HomeView` 是西南等轴测并框住模型，
  `OrbitCamera::ZoomAt` 缩放时光标下的点不动。`Mat4` 为行向量约定（P' = P · M），深度 [0, 1]
- 留边（`OrbitCamera::SetFitInsets`）：视口上叠着功能区、面板时，`OrbitCamera::HomeView` / `OrbitCamera::ZoomToFit` 把模型放进没被挡住的区域，
  并记下目标点在屏幕上的偏移（投影矩阵最后整体平移），所以环绕时模型仍绕自己的中心转；留边再变不挪动画面，下次框住时才用。
  MiniCAD 相机的 SetFitInsets 挪的是目标点
- 交互（`MainFrame::OnViewportPointer`）：左键拖动环绕，中键或右键拖动平移，Shift + 任意键拖动环绕，滚轮缩放。以后有了选择，左键要让给选择（照 Revit：Shift + 中键环绕）
- 几何：`ModelScene::Build` 照 BIMCraft 的 `view3d.rs` 决定画什么、用什么颜色，但用 `Solid::ISolidKernel` 建实体（BIMCraft 自己拼三角形、软件光栅化，不移植）：
  - 墙：`Bim::Walls::Outline`（斜接好的平面轮廓）从底拉伸到顶，再 `Boolean` 减去每个门窗洞（沿墙从洞的一边到另一边、比墙面再出去 50 毫米的条带），
    门窗洞四周的边是整面墙的边。贴着屋顶或楼板的墙照 BIMCraft 用 `Bim::Walls::Prisms` 分成棱柱：底顶都平的拉伸，倾斜的围成封闭网格经 `FromMesh` 转成实体，
    没有高度的（窗台为 0 的门下面那块）跳过。这些棱柱的边线自己给出（底圈、顶圈、竖边），不取内核的边（`FromMesh` 的实体会带出三角剖分的对角线）
  - 楼板：平放时从 `-厚度` 拉伸到 0、减去洞口，再用 `Transform` 错切到顶面所在的平面（`Model::FloorTop`），坡楼板也是一个实体
  - 天花板：底面在标高上方 `offset`，拉伸类型的厚度、减去洞口
  - 屋顶：`Model::RoofPieces` 的每块（坡面减去洞与开洞）一个实体，底面在坡面上、顶面高出 `Model::RoofRise`（铅垂量的厚度）；
    平的拉伸，斜的同贴附的墙一样围成网格。房间与房间分隔线不画
  - 柱：`Model::ColumnOutline` 从底拉伸到顶（`Model::ColumnSpan`），建筑柱同墙的颜色、结构柱与独立基础灰蓝（BIMCraft 的 `STRUCTURE`）
  - run（梁、条形基础、管道……）：矩形的经 `Runs::Geom::Prism` 拉伸（坡的用 `SlopedPrism`），圆的是 `Runs::Geom::Tube` 的三角形（同构件，不经内核）；
    颜色取类型的材质，没有时灰蓝；`Counts::runs` 数它们与独立基础
  - 体量：底面轮廓从 `Model::MassSpan` 的底拉伸到顶；放样的是 `Massing::Tris` 的三角形（同构件，不经内核）；浅蓝（BIMCraft 的 `MASS`），`Counts::masses` 数它们
  - 门窗（没有族时）：墙中间 50 毫米厚的板，与墙正交，门为木色、窗为玻璃色
  - 构件与带族的门窗：族的形体（`Model::ComponentPieces`、`Model::OpeningPieces`）本来就是朝外的三角形（族的扫掠、旋转与空心切割移植自 BIMCraft），
    直接交给渲染器，不经实体内核；按形体材质的颜色着色（库里没有时构件用米色、门窗用门或玻璃的颜色），边只画折角处（相邻面夹角超过 10°）和开口处
  - `Tessellate` 出三角网格、`Edges` 出边线。顶点只有位置和颜色（`Vertex_P3_C4`），所以着色在 CPU 上按固定的平行光算好（不随视角变）
- 什么时候重建：命令改了模型（`Bim::Session::Snapshot` 换了）之后 `MainFrame::OnModelChanged` → `MainFrame::RebuildScene`，场景版本加一
  （`IRenderer::SubmitCached` 的 version 变了，后端才重新上传）；换了项目时相机重新 `HomeView`
- 绘制顺序：背景渐变（裁剪空间坐标、单位矩阵、不写深度）→ 面（`IRenderer::SubmitCached`，几何没变时后端复用上传过的缓冲）→ 边线（每帧沿视线往相机挪一点，见下）
  → 选中的元素（它的三角形 `ModelScene::RangeOf` 再画一遍，强调色，同样往相机挪一点）
- 拾取：建场景时记下每个元素的三角形从哪个顶点起（按元素顺序），`ModelScene::Pick` 用射线与每个三角形求交（Möller–Trumbore），取最近的。
  三维视图里左键单击（没拖动）时，`MainFrame::OnViewportPointer` 把像素经 `OrbitCamera::GetViewProj` 的逆矩阵变到近平面，沿 `OrbitCamera::GetForward` 拾取
- 渲染目标尺寸变了后着色器资源会变，要重新登记成界面纹理（`AppPlatform::RegisterViewportTexture`，`MainFrame::RenderViewport` 末尾）

## 约束与坑

- 边线与所在的面深度相同，深度比较是 `LESS`，直接画会时隐时现：沿视线方向往相机挪约一个像素对应的长度。正交投影下这样挪不改变屏幕位置
- 正交投影的近、远平面取目标前后 `m_depth`（模型对角线的 4 倍，至少 10 m），太大会损失深度精度
- SMLib 不能并发使用：`SMLibKernel` 内部加锁；`ISolidKernel` 的实现只有一个，由宿主第一次请求时创建（`Win32Window::GetSolidKernel`）
- 关掉 `MINIBIM_WITH_SOLID` 时 `AppPlatform::GetSolidKernel` 返回空，`ModelScene` 不建场景，状态栏说明原因；个别元素建模失败时其余照样显示，
  失败的元素记在日志（`LOG_WARN`），状态栏提示看日志
- 布尔运算遇到重合的面容易失败：减去的门窗洞要比墙面再出去一点（`kPast`），楼板的洞口上下也各出去一点
- 墙的轮廓是平面多边形（圆弧切成短直段），拉伸前统一成逆时针（`Extrude` 的轮廓方向）
- 渲染器的接口来自 MiniCAD 的 CAD 视口，有些能力（图像、字形实例、缓存层、浅色背景反色）MiniBIM 还没用到

## 测试与排查

- `ViewTests`（`tests/View/OrbitCameraTests.cpp`）：屏幕方向、框住包围盒（含留边）、缩放与平移时点的屏幕位置、有偏移时环绕与缩放
- `SMLibSmokeTests`：SMLib 静态库能建模、布尔、求体积、离散化
- `ModelSceneTests`（`tests/View/ModelSceneTests.cpp`）：示例房屋的墙、楼板、门、窗、天花板、屋顶、家具都建出来，范围（檐口到屋脊）对，墙在窗洞处扣掉，柱的范围与拾取，梁、圆梁与基础建出来、拾取得到；没有内核时不建
- 画面不对时先用 skill `ui-check` 截图；换图形接口对比（`--gpu d3d11`、`--gpu warp`）
