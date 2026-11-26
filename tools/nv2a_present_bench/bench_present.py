"""One unpaced run of a title built with the Vulkan rasteriser, and its frame
rate once a pad script has taken it to a spot. For the present comparison in
docs/technical/nv2a-renderer-port.md. Windows only.

  python bench_present.py vk|copy --exe EXE --cwd DIR --script PAD --out DIR
      [--save DIR --save-env NAME] [--env NAME=VALUE ...]

vk: the rasteriser presents. copy: RECOMP_NV2A_VK_PRESENT=0, the toolkit's
window shows guest memory.

--exe      the title's build; a copy named <name>-t.exe is run and removed
--cwd      the folder the title is run from
--script   a RECOMP_PAD_SCRIPT file (milliseconds:button, one a line)
--out      a new folder for this run's picture (and log, with --keep-log)
--save     a save folder, copied for the run and removed after it; --save-env
           is the variable the title reads that copy's path from
--env      the title's own variables, and the renderer's settings
--after    seconds before measuring (default: 6 after the script's last press)
--seconds  seconds measured (default 30)
--quiet    wait until other programs use less than this many cores (default 1)

The frame rate is the title bar's, which counts flips, read once a second and
the same on both sides. The log is thrown away unless --keep-log. No RECOMP_
variable is taken from the caller's environment. The picture, spot.bmp, is
asked for after the measuring, so it costs the measured frames nothing: look at
it to see that the run got to the spot. The window never takes the focus; it
does read the pads. A screensaver, another power plan or a busy machine spoils
the figures."""
import argparse
import ctypes as C
import glob
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes as W

k = C.WinDLL('kernel32', use_last_error=True)
u = C.WinDLL('user32')
k.OpenProcess.restype = W.HANDLE
k.GetProcessTimes.argtypes = [W.HANDLE] + [C.c_void_p] * 4
k.CreateFileW.restype = W.HANDLE
k.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
k.DeviceIoControl.argtypes = [W.HANDLE, W.DWORD, C.c_void_p, W.DWORD, C.c_void_p, W.DWORD, C.c_void_p, C.c_void_p]
k.CloseHandle.argtypes = [W.HANDLE]
CHUNK = 1 << 16


def set_sparse(path):
    h = k.CreateFileW(path, 0x40000000, 3, None, 3, 0x80, None)   # write, open existing
    if h != W.HANDLE(-1).value:
        got = W.DWORD()
        k.DeviceIoControl(h, 0x900C4, None, 0, None, 0, C.byref(got), None)   # FSCTL_SET_SPARSE
        k.CloseHandle(h)


def copy_sparse(src, dst):
    """A disk image in a save folder is mostly holes: keep it that way."""
    size, zero = os.path.getsize(src), bytes(CHUNK)
    open(dst, 'wb').close()
    set_sparse(dst)
    with open(src, 'rb') as a, open(dst, 'r+b') as b:
        at, tail = 0, b''
        while True:
            data = a.read(CHUNK)
            if not data:
                break
            if data != zero:
                b.seek(at)
                b.write(data)
            at, tail = at + len(data), data[-1:]
        if size:
            # the length: truncate() would write the zeros out
            b.seek(size - 1)
            b.write(tail)


def system_busy():
    idle, kernel, user = C.c_uint64(), C.c_uint64(), C.c_uint64()
    k.GetSystemTimes(C.byref(idle), C.byref(kernel), C.byref(user))
    return (kernel.value + user.value - idle.value) / 1e7


def process_busy(handle):
    t = [C.c_uint64() for _ in range(4)]
    k.GetProcessTimes(handle, *[C.byref(x) for x in t])
    return (t[2].value + t[3].value) / 1e7


def window_title(pid):
    found = []

    @C.WINFUNCTYPE(C.c_bool, W.HWND, W.LPARAM)
    def each(window, _):
        owner = W.DWORD()
        u.GetWindowThreadProcessId(window, C.byref(owner))
        if owner.value == pid and u.IsWindowVisible(window):
            text = C.create_unicode_buffer(200)
            u.GetWindowTextW(window, text, 200)
            if 'FPS:' in text.value:
                found.append(text.value)
        return True
    u.EnumWindows(each, 0)
    return found[0] if found else ''


def wait_until_quiet(limit):
    began = time.time()
    while True:
        busy, at = system_busy(), time.time()
        time.sleep(5)
        load = (system_busy() - busy) / (time.time() - at)
        if load < limit:
            return load
        print('waiting, %.2f cores busy' % load, flush=True)
        if time.time() - began > 90:
            sys.exit('never quiet')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('side', choices=('vk', 'copy'))
    ap.add_argument('--exe', required=True)
    ap.add_argument('--cwd', required=True)
    ap.add_argument('--script', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--save')
    ap.add_argument('--save-env')
    ap.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    ap.add_argument('--after', type=float)
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--quiet', type=float, default=1.0)
    ap.add_argument('--keep-log', action='store_true')
    args = ap.parse_args()
    if bool(args.save) != bool(args.save_env):
        ap.error('--save and --save-env go together')

    source = os.path.abspath(args.exe)
    name = os.path.splitext(os.path.basename(source))[0]
    exe = os.path.join(os.path.dirname(source), name + '-t.exe')
    for image in (os.path.basename(source), os.path.basename(exe)):
        listed = subprocess.run(['tasklist', '/fi', 'imagename eq ' + image, '/nh'],
                                capture_output=True, text=True).stdout
        if image.lower() in listed.lower():
            sys.exit(image + ' is already running')
    script, out = os.path.abspath(args.script), os.path.abspath(args.out)
    after = args.after
    if after is None:
        with open(script) as f:
            after = max(int(line.split(':')[0]) for line in f if line.strip()) / 1000 + 6
    load = wait_until_quiet(args.quiet)

    os.makedirs(out)
    save = os.path.join(out, 'save')
    flag = os.path.join(out, 'dump.on')
    env = {n: v for n, v in os.environ.items() if not n.upper().startswith('RECOMP_')}
    env.update(RECOMP_NV2A_VK='1', RECOMP_AUDIO_MUTE='1', RECOMP_FLIP_PACING='0',
               RECOMP_PRESENT_VSYNC='immediate', RECOMP_PB_REPORT_MS='1000000000',
               RECOMP_FB_DUMP=os.path.join(out, 'fb'), RECOMP_FB_DUMP_FLIPS=flag,
               RECOMP_PAD_SCRIPT='@' + script)
    if args.side == 'copy':
        env['RECOMP_NV2A_VK_PRESENT'] = '0'
    if args.save:
        shutil.copytree(os.path.abspath(args.save), save, copy_function=copy_sparse)
        env[args.save_env] = save
    env.update(pair.split('=', 1) for pair in args.env)

    shutil.copy2(source, exe)
    show = subprocess.STARTUPINFO()
    show.dwFlags, show.wShowWindow = subprocess.STARTF_USESHOWWINDOW, 4   # SW_SHOWNOACTIVATE
    log = open(os.path.join(out, 'stderr.log'), 'wb') if args.keep_log else subprocess.DEVNULL
    game = None
    try:
        began = time.perf_counter()
        game = subprocess.Popen([exe], cwd=args.cwd, env=env, stdout=subprocess.DEVNULL, stderr=log,
                                startupinfo=show)
        while game.poll() is None and time.perf_counter() - began < after:
            time.sleep(0.5)
        theirs, mine = k.OpenProcess(0x1000, False, game.pid), k.OpenProcess(0x1000, False, os.getpid())
        fps, others, seen = [], [], window_title(game.pid)
        busy = (system_busy(), process_busy(theirs), process_busy(mine))
        began = mark = time.perf_counter()
        while game.poll() is None and time.perf_counter() - began < args.seconds:
            time.sleep(0.1)
            title = window_title(game.pid)
            if title and title != seen:
                seen = title
                fps.append(float(title.split('FPS:')[1].split('|')[0]))
            if time.perf_counter() - mark >= 1:
                now, at = (system_busy(), process_busy(theirs), process_busy(mine)), time.perf_counter()
                others.append(((now[0] - busy[0]) - (now[1] - busy[1]) - (now[2] - busy[2])) / (at - mark))
                busy, mark = now, at
        if game.poll() is None:
            before = set(glob.glob(os.path.join(out, 'fb*.bmp')))
            open(flag, 'w').close()
            time.sleep(0.3)
            os.remove(flag)
            time.sleep(0.5)
            new = sorted(set(glob.glob(os.path.join(out, 'fb*.bmp'))) - before)
            if new:
                os.replace(new[-1], os.path.join(out, 'spot.bmp'))
        else:
            print('THE TITLE ENDED by itself, code', game.poll())
        fps = fps[1:]                       # the first second began before the measuring
        if fps:
            ranked = sorted(fps)
            print('%s: mean %.1f fps, median %.1f, least %.1f, most %.1f (%d readings)' % (
                args.side, sum(fps) / len(fps), ranked[len(ranked) // 2], ranked[0], ranked[-1], len(fps)))
            print('  in order:', ' '.join('%.0f' % x for x in fps))
        if others:
            print('  other programs, cores: mean %.2f max %.2f; before the run %.2f' % (
                sum(others) / len(others), max(others), load))
        print('  picture at the spot:', os.path.exists(os.path.join(out, 'spot.bmp')))
    finally:
        if game and game.poll() is None:
            game.kill()
            game.wait()
        time.sleep(0.5)
        os.remove(exe)
        shutil.rmtree(save, ignore_errors=True)
        for path in glob.glob(os.path.join(out, 'fb*')):
            os.remove(path)


if __name__ == '__main__':
    main()
