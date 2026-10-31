# Revit 文件读取器（src/Rvt，目标 MiniBIMRvt）：移植自 BIMCraft 的 crates/rvt，Apache-2.0（不是 MIT），见 src/Rvt/NOTICE。
# 核心库 MiniBIM 不依赖它；宿主链接 MiniBIMRvt 后得到宏 MINIBIM_WITH_RVT，打开对话框里才有 .rvt。
# 关掉开关（-DMINIBIM_WITH_RVT=OFF）时不编译，发行包里也就没有 Apache-2.0 的这一部分（SMLib 仍是 Apache-2.0）
option(MINIBIM_WITH_RVT "Build the Revit file reader (Apache-2.0, src/Rvt)" ON)
