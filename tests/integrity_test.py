#!/usr/bin/env python3
# integrity_test.py — 字节级数据完整性校验（tcpbench 只验字节数，这里验内容）
# 场景: 经代理 echo 回读，逐字节比对。覆盖: 小包 / 环形缓冲回绕边界(64K±1 与 512K±1，
# 对应历史与现行测试配置的 2 的幂环宽) / MB 级 / 半关闭
# 用法: python3 integrity_test.py <port>
import os
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
SIZES = [
    1,
    64 * 1024 - 1, 64 * 1024 + 1,          # 64K 环宽回绕边界（历史口径）
    512 * 1024 - 1, 512 * 1024, 512 * 1024 + 1,  # 512K 环宽（现行测试配置）
    1024 * 1024, 8 * 1024 * 1024,
]


def roundtrip(size: int) -> float:
    """发 size 字节 → echo 回读 → 逐字节比对。返回耗时秒。"""
    payload = os.urandom(size)
    with socket.create_connection(("127.0.0.1", PORT), timeout=15) as s:
        s.settimeout(30)
        t0 = time.time()
        # 边发边收（全双工，防管道饱和自锁）
        sent = 0
        recved = bytearray()
        view = memoryview(payload)
        while sent < size or len(recved) < size:
            if sent < size:
                try:
                    sent += s.send(view[sent:sent + 256 * 1024])
                except (BlockingIOError, InterruptedError):
                    pass
            if len(recved) < size:
                chunk = s.recv(256 * 1024)
                if not chunk:
                    raise AssertionError(f"对端提前关闭: 已收 {len(recved)}/{size}")
                recved += chunk
        assert bytes(recved) == payload, f"内容不一致 @size={size}"
        # 半关闭闭环: FIN → 等回程 FIN
        s.shutdown(socket.SHUT_WR)
        assert not s.recv(4096), "半关闭后仍有数据"
        return time.time() - t0


def main() -> int:
    ok = 0
    for size in SIZES:
        dt = roundtrip(size)
        ok += 1
        print(f"  integrity {size:>9} B ... PASS ({dt:.2f}s)")
    print(f"== integrity {ok}/{len(SIZES)} PASS ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
