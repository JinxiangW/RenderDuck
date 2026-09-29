# Free camera profiles

这里保存已验证的游戏相机配置及配方，仅支持本地 D3D11 回放。RDC 文件不随仓库发布。

## 文件与适用范围

| 游戏 | 文件 | 类型与适用截帧 | 坐标系 |
|---|---|---|---|
| Endfield | [showroom 配置](endfield/showroom-frame9057.rdc.freecamera.json) | v1 sidecar，适用于 `showroom-frame9057.rdc`，184 个字段 | 世界 +Y 向上，相机 -Z 前向 |
| Endfield | [植被场景配置](endfield/Endfield_2026.05.19_22.53_frame56480.rdc.freecamera.json) | v1 sidecar，适用于 `Endfield_2026.05.19_22.53_frame56480.rdc`，755 个字段 | 世界 +Y 向上，相机 -Z 前向 |
| OW | [相机布局](ow/overwatch.json) | v1 Recipe，描述已验证的 CB 布局，每份截帧重新扫描 | 世界 +Y 向上，相机 +Z 前向 |
| OW | [range 完整配方](ow/overwatch-range-v2.json) | v2 Recipe，适用于已验证的 `range.rdc`，包含相机及颜色旁路 | 世界 +Y 向上，相机 +Z 前向 |

## Endfield：恢复已验证配置

两份 Endfield 文件从原 sidecar **按字节原样归档**，保留完整 RDC 指纹、相机基线、字段区间和独立投影。
它们不是 `Recipe...` 的输入，不要在配方编辑器里导入。

1. 使用与配置中 `capture.size`、`capture.sha256` 完全匹配的 RDC。
2. 将相应 JSON 放到 RDC 旁，名称必须为 `<RDC 文件名>.freecamera.json`。
3. 打开 RDC，等待配置恢复；确认字段后手动启用 Free camera。

改名或移动 RDC 时同步调整 sidecar 路径。恢复后默认保持原画面，不会自动启用相机。
文件未包含 `world_up`，新版按兼容默认值 `+Y` 读取；旧文件仍保留原格式。
这些配置没有 Pass 跳过或时域旁路规则。不能把其中的 Shader ID、资源 ID、EID 或投影变体直接套到其他场景。

## OW：导入 JSON 配方

在 **Free camera... → Recipe... → Import JSON...** 导入，点击 **Use and run**，
阅读扫描报告后选择 **Apply confirmed configuration**。执行过程不依赖 MCP。

- `overwatch.json` 不含具体事件、资源或截帧指纹。它只复用布局规则；多个候选相机时，
  agent 需要补充已确认的 `reference_event` 和当前 RDC 的完整指纹。
- `overwatch-range-v2.json` 已包含参考事件 2043 和指纹，只有匹配的截帧可以执行。
  它将 EID 4799–4811 的时域重建／锐化替换为颜色旁路：纹理 1842 → 153044，
  从 2560×1440 双线性缩放到 3840×2160，保留后续色调映射和 UI。
- 完整配方绑定的 `range.rdc` 为 888160593 字节，SHA-256 为
  `955894e95ebbd306d7b6dfddc1fb96116ecfb076f2787f39883ea1f18b34d179`。

## 已知限制与维护

两类文件都只覆盖已确认的相机字段。历史矩阵、阴影、屏幕空间效果、第一人称独立视角，
以及 CPU 未提交的物体、原视角决定的 LOD 和排序，仍可能造成瑕疵。
OW 的旁路不重建辅助历史纹理，普通缩放也不等同于原时域升分辨率算法。

同名 RDC 不代表内容相同。新截帧应由 agent 分析后生成新的配置，不能复制旧事件/资源编号来绕过指纹检查。
迁移到新的游戏布局时，保持程序入口中性，不增加按游戏名称自动套用的规则。

详细接口、矩阵约定和恢复流程见 [Free camera 与 Pass 覆盖说明](../how/how_replay_overrides.rst)。
