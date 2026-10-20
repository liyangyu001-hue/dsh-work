#!/usr/bin/env node
/**
 * 编译原生件（Windows / MSVC）。
 *
 * 为什么不用 CMake：这里只有两个"一个 cpp 出一个 exe"的小工程，引入构建系统
 * 只会多一层要维护的抽象。直接 vcvars64 + cl，十秒内可复现。
 *
 * 找不到 MSVC 时的提示要具体——这个脚本的失败模式几乎全是"没装 Build Tools"
 * 或者"装了但不在默认路径"，泛泛而报错会让人白查半小时。
 */
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");

/** 每一项：源码目录 + 产物名 + 子系统。加新的原生件就在这里加一行。 */
const projects = [
  // 输入助手是父进程的管道对端，必须有控制台（stdin/stdout）
  { name: "dshwork-input", srcDir: join(root, "native", "input-helper", "src"), subsystem: "CONSOLE", libs: "user32.lib" },
  // 小条是纯窗口程序，带控制台会闪一个黑框
  { name: "dshwork-shell", srcDir: join(root, "native", "shell", "src"), subsystem: "WINDOWS", libs: "user32.lib gdi32.lib shell32.lib d2d1.lib dwrite.lib winhttp.lib d3d11.lib dxgi.lib" },
];

const VC_VARS_CANDIDATES = [
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/Professional/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat",
];

const vcVars = VC_VARS_CANDIDATES.find((path) => existsSync(path));
if (vcVars === undefined) {
  console.error(
    [
      "找不到 vcvars64.bat，无法编译原生件。",
      "需要 Visual Studio 2022 生成工具（勾选「使用 C++ 的桌面开发」）：",
      "  https://visualstudio.microsoft.com/visual-cpp-build-tools/",
      "如果装在别的路径，把那个路径加到 scripts/build-native.mjs 的 VC_VARS_CANDIDATES。",
    ].join("\n"),
  );
  process.exit(1);
}

const vcVarsWin = vcVars.replaceAll("/", "\\");
const only = process.argv[2];  // 可选：只编某一个

for (const project of projects) {
  if (only !== undefined && only !== project.name) continue;

  const outDir = resolve(project.srcDir, "..", "build");
  mkdirSync(outDir, { recursive: true });
  const exe = join(outDir, `${project.name}.exe`);
  if (existsSync(exe)) rmSync(exe, { force: true });

  const subsystem = project.subsystem === "WINDOWS" ? "/SUBSYSTEM:WINDOWS" : "/SUBSYSTEM:CONSOLE";
  // 窗口程序仍然写 int main()，所以要把入口指回 CRT 的 mainCRTStartup，
  // 否则链接器会去找 WinMain 并报"无法解析的外部符号"。
  const entry = project.subsystem === "WINDOWS" ? "/ENTRY:mainCRTStartup" : "";
  // 走临时 .bat：cmd /c 的引号规则太容易把带空格的路径吃掉，
  // 落成文件后行为确定，出错时也能直接把 .bat 拿去手动复现。
  const batPath = join(outDir, "build.bat");
  writeFileSync(
    batPath,
    [
      "@echo off",
      `call "${vcVarsWin}" >nul || exit /b 1`,
      `cd /d "${project.srcDir.replaceAll("/", "\\")}" || exit /b 1`,
      // /utf-8 是必须的：源码里的中文注释与 L"..." 宽字面量都是 UTF-8，
      // 不加这个开关 MSVC 会按本地代码页（GBK）解释，注释能吞掉下一行代码。
      `cl /nologo /std:c++17 /utf-8 /EHsc /W4 /O2 /DUNICODE /D_UNICODE main.cpp /link ${subsystem} ${project.libs} ${entry} /OUT:"${exe.replaceAll("/", "\\")}"`,
      "exit /b %ERRORLEVEL%",
    ].join("\r\n"),
    "utf8",
  );

  console.log(`编译 ${project.name} …`);
  try {
    execFileSync("cmd.exe", ["/d", "/c", batPath], { stdio: "inherit" });
  } catch {
    console.error(`${project.name} 编译失败，看上面的 cl 输出（可手动跑 ${batPath} 复现）。`);
    process.exit(1);
  }

  if (!existsSync(exe)) {
    console.error(`${project.name} 编译似乎成功但没找到产物：${exe}`);
    process.exit(1);
  }
  console.log(`  已生成 ${exe}（${(statSync(exe).size / 1024).toFixed(1)} KB）`);
}
