# GitHub 开源发布流程指南

> 跟着以下步骤，一步步把你的 ChronoCube 项目发布到 GitHub。

---

## 第一步：创建 GitHub 仓库

1. 打开 [github.com](https://github.com) 并登录
2. 点击右上角 **+** → **New repository**
3. 填写仓库信息：
   - **Repository name**：`chronocube`（推荐全小写）
   - **Description**：`通过翻转方块切换模式的物理交互番茄钟 | ESP32-C6 AMOLED`
   - **Public** ✅（公开仓库）
   - **不要**勾选 "Add a README file"（我们已经有 README 了）
   - **不要**勾选 ".gitignore"（我们已经有了）
   - **不要**勾选 "Choose a license"（我们已经有了 MIT License）
4. 点击 **Create repository**

---

## 第二步：初始化本地 Git 仓库并推送

打开终端，进入 `github-release/` 目录：

```bash
cd "d:/Dev/ESP32/ChronoCube - AI/github-release"

# 初始化 Git 仓库
git init

# 设置默认分支名为 main
git branch -M main

# 添加远程仓库（把 YOUR_USERNAME 换成你的 GitHub 用户名）
git remote add origin https://github.com/YOUR_USERNAME/chronocube.git
```

---

## 第三步：添加所有文件并提交

```bash
# 添加所有文件（.gitignore 会自动排除不需要的文件）
git add .

# 提交
git commit -m "🎉 Initial commit: ChronoCube v5.3.x open source release"
```

---

## 第四步：推送到 GitHub

```bash
# 推送到 main 分支
git push -u origin main
```

---

## 第五步：创建 Release（发布固件包）

1. 在仓库主页点击 **Releases** → **Create a new release**
2. 填写：
   - **Tag version**：`v5.3.16`（和固件版本一致）
   - **Release title**：`ChronoCube v5.3.16`
   - **Description**：
     ```markdown
     ## 功能
     - 9 状态机完整实现
     - LVGL 9.5 UI 界面
     - Catppuccin Mocha 配色
     - 中英文双字库显示
     - SD 卡事件日志（CRC16 校验）
     - 串口调试控制台（23 条命令）
     
     ## 硬件
     - 微雪 ESP32-C6 2.16" AMOLED 触控开发板
     ```
   - **不要**上传二进制文件（.bin），Arduino 用户自己编译
3. 点击 **Publish release**

---

## 第六步：完善仓库信息（可选但推荐）

### 添加 Topics（标签）
在仓库主页点击 ⚙️ 齿轮 → 在 **Topics** 框中添加：
```
esp32 arduino pomodoro-timer focus-timer lvgl physical-computing amoled-display catppuccin
```

### 添加 About 描述
在仓库主页右侧 **About** 区域点击 ⚙️，填写简短描述和网站链接（如果有的话）。

---

## 第七步：准备字库 Release（关键！）

固件编译需要中英文字库文件，这些文件太大不适合放 Git 仓库，应该作为 Release 附件单独分发：

1. 准备以下文件：
   - `cn_24.bin` — 中文字库（文泉驿点阵正黑 24×24）
   - `en_12x24.bin` — 英文字库（Cascadia Mono 12×24）

2. 在 Releases 页面创建新 Release：
   - **Tag**：`fonts-v1.0`
   - **Title**：`字体文件 v1.0`
   - 将两个字库 `.bin` 文件拖入附件区域

3. 在 README.md 中更新字体下载链接指向这个 Release。

---

## 本地文件对照（发布前检查清单）

| 检查项 | 状态 |
|--------|------|
| `config.h` WiFi 密码已脱敏 | ✅ 已替换为占位符 |
| `config.h` MQTT 地址已脱敏 | ✅ 已替换为占位符 |
| `LICENSE` 文件已添加 | ✅ MIT License |
| `README.md` 已编写 | ✅ 英文/中文双语 |
| `.gitignore` 排除编译产物 | ✅ |
| `chronocube.conf.example` 不含个人信息 | ✅ 纯配置项 |
| 无 `hardware_ref/` 第三方资料 | ✅ 已排除 |
| 无 Agent 内部文档 | ✅ 已排除 |
| 固件源码完整可编译 | ⚠️ 需你自己验证一次 |

---

## 注意事项

1. **WiFi 密码**：config.h 中的 `WIFI_SSID` 和 `WIFI_PASS` 已替换为 `YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD`。发布前请再次确认。
2. **MQTT 地址**：已替换为 `your-mqtt-broker.local`。
3. **字库文件**：`.bin` 字库文件不在源码仓库中，需要单独作为 Release 附件分发。
4. **硬件资料**：ESP32-C6 数据手册、原理图等请从微雪官网下载，不包含在本仓库中。

---

## 常见问题

**Q: 需要 GitHub Desktop 吗？**
A: 不需要。上面所有操作既可以用命令行，也可以在 GitHub 网页端完成（推荐网页端 + 拖拽上传文件）。

**Q: 纯网页端怎么上传？**
A: 创建空仓库后，直接在网页端将 `github-release/` 文件夹拖入仓库页面，GitHub 会自动识别并创建提交。

**Q: 怎么更新固件？**
A: 修改代码后，`git add . && git commit -m "描述" && git push` 即可。重大更新建议创建新的 Release。
