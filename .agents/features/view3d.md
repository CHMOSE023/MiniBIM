---
covers: src/View/*, apps/Common/src/GUI/DemoScene.*, src/Solid/*, apps/Render/*, src/Render/*
---

# 三维视口

## 概述

主窗口的视口（面板 `viewport`）显示三维模型：`OrbitCamera`（`src/View/OrbitCamera.h`）给出视图投影矩阵，`MainFrame::RenderViewport` 用
`IRenderer`（`src/Render/IRenderer.h`，D3D12 / D3D11 实现在 `apps/Render`）画进离屏渲染目标，再作为纹理交给 `MiniGUI::ViewportHost` 显示。
几何只经 `Solid::ISolidKernel`（SMLib）得到。现在的模型是临时的 `DemoScene`，第 2 阶段换成 BIMCraft 的模型。

## 入口与数据流

- 相机：正交投影、绕目标环绕；世界坐标 Z 向上、单位毫米。方位角自 +X 逆时针，仰角限制在 ±89°。`OrbitCamera::HomeView` 是西南等轴测并框住模型，
  `OrbitCamera::ZoomAt` 缩放时光标下的点不动。`Mat4` 为行向量约定（P' = P · M），深度 [0, 1]
- 交互（`MainFrame::OnViewportPointer`）：左键拖动环绕，中键或右键拖动平移，Shift + 任意键拖动环绕，滚轮缩放。以后有了选择，左键要让给选择（照 Revit：Shift + 中键环绕）
- 几何：`DemoScene::Build` 用 `Solid::ISolidKernel` 建实体（`Box`、`Boolean` 减出门窗洞口、`Cylinder`），`Tessellate` 出三角网格、`Edges` 出边线。
  顶点只有位置和颜色（`Vertex_P3_C4`），所以着色在 CPU 上按固定的平行光算好（不随视角变）
- 绘制顺序：背景渐变（裁剪空间坐标、单位矩阵、不写深度）→ 面（`IRenderer::SubmitCached`，几何没变时后端复用上传过的缓冲）→ 边线（每帧沿视线往相机挪一点，见下）
- 渲染目标尺寸变了后着色器资源会变，要重新登记成界面纹理（`AppPlatform::RegisterViewportTexture`，`MainFrame::RenderViewport` 末尾）

## 约束与坑

- 边线与所在的面深度相同，深度比较是 `LESS`，直接画会时隐时现：沿视线方向往相机挪约一个像素对应的长度。正交投影下这样挪不改变屏幕位置
- 正交投影的近、远平面取目标前后 `m_depth`（模型对角线的 4 倍，至少 10 m），太大会损失深度精度
- SMLib 不能并发使用：`SMLibKernel` 内部加锁；`ISolidKernel` 的实现只有一个，由宿主第一次请求时创建（`Win32Window::GetSolidKernel`）
- 关掉 `MINIBIM_WITH_SOLID` 时 `AppPlatform::GetSolidKernel` 返回空，`DemoScene` 不建模型，状态栏说明原因
- 渲染器的接口来自 MiniCAD 的 CAD 视口，有些能力（图像、字形实例、缓存层、浅色背景反色）MiniBIM 还没用到

## 测试与排查

- `ViewTests`（`tests/View/OrbitCameraTests.cpp`）：屏幕方向、框住包围盒、缩放与平移时点的屏幕位置
- `SMLibSmokeTests`：SMLib 静态库能建模、布尔、求体积、离散化
- 画面不对时先用 skill `ui-check` 截图；换图形接口对比（`--gpu d3d11`、`--gpu warp`）
