"""Self-check for prof_event_marker_gui: marking keeps behaviour, unmarking restores the text.

Run: python test_prof_event_marker.py
Executes the sample with minilua (built with LuaJIT) when it is present.
"""

import os
import subprocess
import sys
import tempfile

import prof_event_marker_gui as marker

SAMPLE = r'''
local depth, begins, ends, inner = 0, 0, 0, -1
function PROF_EVENT_BEGIN(name) depth = depth + 1; begins = begins + 1 end
function PROF_EVENT_END(...) depth = depth - 1; ends = ends + 1; return ... end

-- minilua has no print/tostring/select
local function str(v) if v == nil or v == true or v == false then return type(v) == "nil" and "nil" or (v and "true" or "false") end return v .. "" end
local function multi() return 1, nil, 3 end
local function empty_ret(x) if x then return end x = 1 end
local function varargs(...) return ... end
local function tail(n) if n == 0 then return "done" end return tail(n - 1) end
local function anon() return function(a) return a * 2 end end
local function tbl() local t = {1; 2, n = 3} return t[1] + t[2] + t.n end
local function branches(x) if x == 1 then return "one" elseif x == 2 then return "two" else return "many" end end
local function rep(x) repeat if x > 3 then return x end x = x + 1 until false end
local function loop() for i = 1, 3 do if i == 2 then return i end end end
local function fallthrough() local y = 1 end
local function semi() return 5; end
local obj = {} function obj:method(a) return self == obj, a end
local function nested_paren() return (multi()) end
local function strs() return "end", 'return', [[ end ]] end -- keywords inside strings
local function heavy() inner = depth return 7 end
local function returns_heavy() return heavy() end
local function tight()return{}end
local function err() error("boom") end

local out = {}
local function add(a, b, c, d) out[#out + 1] = str(a) .. "," .. str(b) .. "," .. str(c) .. "," .. str(d) end
add(multi()) add(empty_ret(true)) add(empty_ret(false)) add(varargs(1, nil, 3))
add(tail(50)) add(anon()(21)) add(tbl()) add(branches(1), branches(2), branches(3))
add(rep(1)) add(loop()) add(fallthrough()) add(semi()) add(obj:method("m"))
add(nested_paren()) add(strs()) add(returns_heavy()) add(type(tight()))
io.write(table.concat(out, "|"), "\n")
io.write("balance ", begins - ends, " ", depth, "\n")
-- a marked caller sees heavy() inside its own zone: depth 2, unmarked 0
io.write("inner ", inner, "\n")
io.write("error ", str(pcall(err)), "\n")
'''


def find_minilua():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "..", "3rd party", "luajit-2", "src", "host", "minilua.exe")
    return path if os.path.isfile(path) else None


def run_lua(lua, text):
    fd, path = tempfile.mkstemp(suffix=".lua")
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        stream.write(text)
    try:
        result = subprocess.run([lua, path], capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        return result.stdout.replace(path, "<file>").splitlines()
    finally:
        os.unlink(path)


def main():
    marked, stats, _ = marker.mark_text(SAMPLE, "sample.script")
    assert stats.marked == 21 and stats.protected == 2, stats
    assert "return PROF_EVENT_END( heavy())" in marked
    assert 'return PROF_EVENT_END( "done")' in marked
    assert "return PROF_EVENT_END() end" in marked
    assert marker.mark_text(marked, "sample.script")[0] == marked, "marking twice changed the text"

    restored, removed = marker.unmark_text(marked, "sample.script")
    # the only difference: the space marking adds after `do` in `tight()return`
    assert restored == SAMPLE.replace("tight()return", "tight() return"), "unmark did not restore the original"
    assert removed > 0

    lua = find_minilua()
    if not lua:
        print("minilua not found, skipped the run check")
        return 0
    before = run_lua(lua, SAMPLE)
    after = run_lua(lua, marked)
    assert before[0] == after[0], (before[0], after[0])
    assert after[1] == "balance 0 0", after[1]
    assert before[2] == "inner 0" and after[2] == "inner 2", (before[2], after[2])
    assert before[3] == after[3], (before[3], after[3])
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
