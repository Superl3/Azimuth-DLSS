"""Structural guard against the startup regression: the Visible trace must
not acquire or call DXGI interfaces after the borrowed factory call.
Actual same-HWND destruction/replacement still needs a runtime test."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
src = (root / "src/visible_trace.inl").read_text(encoding="utf-8")
code = re.sub(r"/\*.*?\*/|//[^\n]*", "", src, flags=re.S)
for bad in ("IDXGISwapChain", "GetBuffer(", "GetLastPresentCount(", "GetFrameStatistics(",
            "GetCurrentBackBufferIndex(", "QueryInterface(", "AddRef(", "Release(",
            "->lpVtbl", "VT_SLOT(", "patch_slot("):
    assert bad not in code, f"unsafe Visible trace interface access: {bad}"
assert "retainedForTrial=0" in code
hooks = (root / "src/hooks.c").read_text(encoding="utf-8")
assert "visible_swapchain(*sc,device)" not in hooks
assert "fg_trace_swapchain(*sc,device,&snapshot)" in hooks
assert "visible_note_chain(attempt,(uintptr_t)hwnd,(uintptr_t)*sc,&snapshot)" in hooks
print("PASS: Visible trace has no owning or borrowed DXGI interface calls")
