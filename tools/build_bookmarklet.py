#!/usr/bin/env python3
"""由源码版 timetable_bookmarklet.js 生成两个产物：
  1. timetable_bookmarklet.oneline.txt —— 单行 bookmarklet（直接复制到书签）。
  2. src/ui/bookmarklet.inc —— C++ 头文件（供应用内「复制书签脚本」使用）。

用法：python3 tools/build_bookmarklet.py
"""
import re
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # 项目根目录
SRC = os.path.join(HERE, "timetable_bookmarklet.js")
OUT_ONELINE = os.path.join(HERE, "timetable_bookmarklet.oneline.txt")
OUT_INC = os.path.join(ROOT, "src", "ui", "bookmarklet.inc")


def strip_comments(src: str) -> str:
    """去除块注释与行注释（本项目源码的 // 注释不落在字符串内）。"""
    # 块注释
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    # 行注释：逐行截断到首个 //
    lines = []
    for line in src.splitlines():
        idx = line.find("//")
        if idx != -1:
            line = line[:idx]
        lines.append(line)
    return "\n".join(lines)


def main() -> None:
    with open(SRC, encoding="utf-8") as f:
        src = f.read()
    src = strip_comments(src)
    # 压缩空白（字符串内不包含需要保留的连续空白，安全）。
    src = re.sub(r"\s+", " ", src).strip()
    bookmark = "javascript:" + src

    with open(OUT_ONELINE, "w", encoding="utf-8") as f:
        f.write(bookmark)
    print(f"生成 {OUT_ONELINE}（{len(bookmark)} 字符）")

    # 生成 C++ 头文件（用自定义定界符 BOOKMARK 包裹，避免转义问题）。
    # 脚本内不含 ")BOOKMARK" 序列，可安全使用。
    assert ")BOOKMARK" not in bookmark, "脚本含定界符冲突，请更换定界符"
    header = (
        "// 由 tools/build_bookmarklet.py 自动生成。请勿手改；\n"
        "// 要改书签脚本请编辑 tools/timetable_bookmarklet.js 后重新生成。\n"
        "#pragma once\n\n"
        "namespace bp::ui {\n\n"
        "// 教务系统课表抓取书签脚本（单行，含 javascript: 前缀，UTF-8）。\n"
        'constexpr const char* kBookmarkletJs = R"BOOKMARK(' + bookmark + ')BOOKMARK";\n\n'
        "}  // namespace bp::ui\n"
    )
    with open(OUT_INC, "w", encoding="utf-8") as f:
        f.write(header)
    print(f"生成 {OUT_INC}（{len(bookmark)} 字符）")


if __name__ == "__main__":
    main()
