#!/usr/bin/env python3
"""Prepares everything the Android app needs that is derived from the game, then
leaves the app itself to Android Studio / Gradle.

    python tools/prepare.py doctor                 what is missing on this machine
    python tools/prepare.py all --game <iso|dir|default.xex>
    python tools/prepare.py deps                   submodules (RexGlue, Plume), adrenotools, shader tool (pinned revisions)
    python tools/prepare.py game --game <...>      checks the executable and installs assets/default.xex
    python tools/prepare.py codegen                recompiles the executable to C++ (generated/android)
    python tools/prepare.py shaders [--dumps DIR]  recompiles the shaders to SPIR-V (generated/shaders)
    python tools/prepare.py dumps                  pulls shader captures from a connected phone (second pass)
    python tools/prepare.py apk                    optional: runs Gradle assembleDebug

Nothing derived from the game leaves this machine and none of it is committed:
generated/, assets/ and out/ are ignored by git. See docs/BUILD.md.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
EXT = Path(os.environ.get("RENUT_EXTERNAL") or ROOT / "external")
OUT = ROOT / "out" / "prepare"
LOGS = OUT / "logs"
GENERATED = ROOT / "generated"
IS_WINDOWS = os.name == "nt"

# The European retail executable is the only one the generated code and the hooks match.
XEX_SHA256 = "d57b15b73490ca8334fb2fc7a53e0b21936a187731fa4f891273f4feaf38d3bc"
XEX_PACKAGE = "org.renut.android"

# name -> (url, pinned commit, submodule paths or None for "all, recursively", patch to apply)
# RexGlue and Plume are not here: they are git submodules of this repository
# (thirdparty/rexglue-sdk, thirdparty/plume), used as released and never patched.
DEPENDENCIES = {
    "libucontext": ("https://github.com/kaniini/libucontext.git", "49e671dd52ff6791295d8161ad3b6da7dc5f6f9d", [], None),
    "libadrenotools": ("https://github.com/bylaws/libadrenotools.git", "8fae8ce254dfc1344527e05301e43f37dea2df80", None, None),
}

# The code generator, from the SDK release the sources are pinned to (sha256 as published by GitHub).
HOST_SDK_VERSION = "0.10.0"
HOST_SDK = {
    "win-amd64": "ad4485da853fbe932e9f5dbb643f9b5fe85dc73775ebbad4df6879fe6df0c11b",
    "linux-amd64": "0388edb3f8fb1444f1b0ad5d440cb02d9690ac20a76b04fd6b7dcc19f59328a6",
    "linux-arm64": "feeeccedc1c0c8b4737c0e847e814314dcb89db5a6fecd3c14e32df8bb79d0a6",
    "mac-amd64": "a71b12e8c91d7f9084683166c59a176634e6adc711f2c60f80cc4a286aa13642",
    "mac-arm64": "1192638b51a6963aa6a4f24b77ebcb90c16f7fd0b91b311e11a90100f5274326",
}

STEP = [0]


# ---- output helpers ------------------------------------------------------------------------------

def say(message=""):
    print(message, flush=True)


def step(title):
    STEP[0] += 1
    say("\n== %s" % title)


def fail(message):
    say("\nERRO: %s" % message)
    sys.exit(1)


def human(n):
    return "%.1f GB" % (n / 2 ** 30) if n >= 2 ** 30 else "%.0f MB" % (n / 2 ** 20)


def run(cmd, cwd=None, env=None, log=None, check=True, quiet=False):
    """Runs a command with its output in a log file; prints the tail of the log if it fails."""
    LOGS.mkdir(parents=True, exist_ok=True)
    log_path = LOGS / (log or "command.log")
    say("   $ " + " ".join(shlex.quote(str(c)) for c in cmd)[:200])
    merged = dict(os.environ)
    merged.update(env or {})
    started = time.time()
    with open(log_path, "a", encoding="utf-8", errors="replace") as handle:
        handle.write("\n$ %s\n" % " ".join(str(c) for c in cmd))
        handle.flush()
        process = subprocess.Popen([str(c) for c in cmd], cwd=str(cwd) if cwd else None, env=merged,
                                   stdout=handle, stderr=subprocess.STDOUT)
        last = started
        while True:
            try:
                code = process.wait(timeout=30)
                break
            except subprocess.TimeoutExpired:
                if not quiet and time.time() - last >= 60:
                    last = time.time()
                    say("   … %d min" % ((time.time() - started) // 60))
    if code and check:
        tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-30:]
        say("\n".join("   | " + line for line in tail))
        fail("o comando terminou com código %d. Log completo: %s" % (code, log_path))
    return code


def capture(cmd, cwd=None):
    result = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None, capture_output=True, text=True)
    return result.returncode, result.stdout.strip()


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url, destination, sha256):
    destination = Path(destination)
    if destination.is_file() and sha256_of(destination) == sha256:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    temp = destination.with_suffix(destination.suffix + ".part")
    say("   baixando %s" % url)
    request = urllib.request.Request(url, headers={"User-Agent": "reNut-prepare"})
    with urllib.request.urlopen(request) as response, open(temp, "wb") as out:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        shown = -1
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            out.write(block)
            done += len(block)
            if total and done * 10 // total != shown:
                shown = done * 10 // total
                say("   %d%% (%s)" % (shown * 10, human(done)))
    if sha256_of(temp) != sha256:
        temp.unlink()
        fail("o arquivo baixado de %s não confere com o checksum esperado. Se o site publicou outra versão, "
             "baixe a anterior manualmente e coloque em %s." % (url, destination))
    os.replace(temp, destination)
    return destination


def host_platform():
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "amd64"
    if IS_WINDOWS:
        return "win-" + arch
    return ("mac-" if sys.platform == "darwin" else "linux-") + arch


# ---- Linux tools: native, or WSL on Windows ------------------------------------------------------

def to_linux_path(path):
    if not IS_WINDOWS:
        return str(path)
    code, text = capture(["wsl.exe", "-e", "wslpath", "-a", str(path)])
    if code:
        fail("não consegui converter %s para um caminho do WSL." % path)
    return text


def linux_bash(script, env=None, log=None, check=True):
    """Runs a bash script where the shader tool builds: here on Linux/macOS, inside WSL on Windows."""
    if IS_WINDOWS:
        return run(["wsl.exe", "-e", "bash", "-lc", script], env=env, log=log, check=check)
    return run(["bash", "-lc", script], env=env, log=log, check=check)


def linux_has_wsl():
    if not IS_WINDOWS:
        return True
    code, _ = capture(["wsl.exe", "-e", "true"])
    return code == 0


SHADER_TOOLS = ["git", "clang", "clang++", "cmake", "ninja", "spirv-val"]


def missing_shader_tools():
    code, text = (capture(["wsl.exe", "-e", "bash", "-lc", "for t in %s; do command -v $t >/dev/null || echo $t; done" % " ".join(SHADER_TOOLS)])
                  if IS_WINDOWS else
                  capture(["bash", "-lc", "for t in %s; do command -v $t >/dev/null || echo $t; done" % " ".join(SHADER_TOOLS)]))
    return text.split() if code == 0 else SHADER_TOOLS


# ---- Java / Android SDK discovery ----------------------------------------------------------------

def java_candidates():
    candidates = [os.environ.get("JAVA_HOME", "")]
    if IS_WINDOWS:
        for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("LOCALAPPDATA", "")):
            candidates.append(os.path.join(base, "Android", "Android Studio", "jbr"))
            candidates.append(os.path.join(base, "Programs", "Android Studio", "jbr"))
    elif sys.platform == "darwin":
        candidates.append("/Applications/Android Studio.app/Contents/jbr/Contents/Home")
    else:
        candidates += ["/opt/android-studio/jbr", os.path.expanduser("~/android-studio/jbr"),
                       "/snap/android-studio/current/android-studio/jbr"]
        for version in (17, 21, 18, 19, 20):
            candidates += ["/usr/lib/jvm/java-%d-openjdk-amd64" % version, "/usr/lib/jvm/java-%d-openjdk-arm64" % version,
                           "/usr/lib/jvm/java-%d-openjdk" % version, "/usr/lib/jvm/jdk-%d" % version]
        which = shutil.which("javac") or shutil.which("java")
        if which:
            candidates.append(os.path.dirname(os.path.dirname(os.path.realpath(which))))
    return candidates


def find_java_home():
    """A JDK 17 to 21 when there is one (newer ones break the Gradle in use); otherwise the first JDK found."""
    exe = "java.exe" if IS_WINDOWS else "java"
    found = [home for home in dict.fromkeys(java_candidates()) if home and os.path.isfile(os.path.join(home, "bin", exe))]
    for home in found:
        if 17 <= java_major(home) <= 21:
            return home
    return found[0] if found else None


def java_major(home):
    # `java -version` prints to stderr.
    result = subprocess.run([os.path.join(home, "bin", "java.exe" if IS_WINDOWS else "java"), "-version"],
                            capture_output=True, text=True)
    line = (result.stderr or result.stdout).splitlines()[0] if (result.stderr or result.stdout) else ""
    digits = [p for p in line.replace('"', " ").split() if p[:1].isdigit()]
    if not digits:
        return 0
    first = digits[0].split(".")
    return int(first[1]) if first[0] == "1" and len(first) > 1 else int(first[0])


def find_android_sdk():
    props = ROOT / "android" / "local.properties"
    if props.is_file():
        for line in props.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("sdk.dir="):
                value = line.split("=", 1)[1].replace("\\:", ":").replace("\\\\", "\\")
                if os.path.isdir(value):
                    return value
    for env in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.path.isdir(os.environ.get(env, "")):
            return os.environ[env]
    guesses = [os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk"),
               os.path.expanduser("~/Android/Sdk"), os.path.expanduser("~/Android/sdk"),
               os.path.expanduser("~/Library/Android/sdk"), "/opt/android-sdk", "/usr/lib/android-sdk"]
    return next((g for g in guesses if g and os.path.isdir(g)), None)


# ---- stage: doctor -------------------------------------------------------------------------------

def stage_doctor(args):
    problems = 0

    def row(ok, what, hint=""):
        nonlocal problems
        say("  [%s] %s%s" % ("ok" if ok else "FALTA", what, "" if ok or not hint else "\n          -> " + hint))
        problems += 0 if ok else 1

    say("Verificando esta máquina (%s):" % host_platform())
    row(sys.version_info >= (3, 8), "Python %s" % platform.python_version(), "instale Python 3.8 ou mais novo")
    row(shutil.which("git") is not None, "git", "instale o Git")
    row(not IS_WINDOWS or len(str(ROOT)) <= 70, "caminho do projeto curto (%d caracteres)" % len(str(ROOT)),
        "o Windows limita caminhos a 260 caracteres e os submódulos são profundos: mova o projeto para algo como C:\reNut")
    free = shutil.disk_usage(ROOT).free
    row(free > 30 * 2 ** 30, "espaço livre: %s (precisa de uns 30 GB)" % human(free), "libere espaço na unidade do projeto")
    if IS_WINDOWS:
        wsl = linux_has_wsl()
        row(wsl, "WSL2 com uma distribuição Linux (para compilar a ferramenta de shaders)",
            "em um PowerShell de administrador: wsl --install -d Ubuntu")
    else:
        wsl = True
    if wsl:
        missing = missing_shader_tools()
        row(not missing, "ferramentas de shader no Linux/WSL%s" % ("" if not missing else " (faltam: %s)" % ", ".join(missing)),
            "Debian/Ubuntu: sudo apt install -y git clang cmake ninja-build spirv-tools "
            "| Fedora: sudo dnf install -y git clang cmake ninja-build spirv-tools "
            "| Arch: sudo pacman -S --needed git clang cmake ninja spirv-tools")
    java = find_java_home()
    version = java_major(java) if java else 0
    row(java is not None and 17 <= version <= 21, "JDK 17 a 21 (%s)" % (java or "não encontrado"),
        "use o JDK do Android Studio (pasta jbr) ou instale openjdk-17-jdk, e defina JAVA_HOME; "
        "o JDK 22+ quebra o Gradle atual")
    sdk = find_android_sdk()
    row(sdk is not None, "Android SDK (%s)" % (sdk or "não encontrado"),
        "instale o Android Studio e abra-o uma vez; ele baixa o SDK. Sem o Studio: ferramentas de linha de comando "
        "do Android (veja docs/BUILD.md). O Gradle baixa NDK 29 e CMake 3.22 sozinho")
    if sdk:
        row(os.path.isfile(os.path.join(sdk, "licenses", "android-sdk-license")), "licenças do Android SDK aceitas",
            "yes | %s --licenses" % os.path.join(sdk, "cmdline-tools", "latest", "bin", "sdkmanager"))
    say("")
    say("Tudo certo." if not problems else "%d item(ns) precisam de atenção antes do build completo." % problems)
    return 1 if problems else 0


# ---- stage: deps ---------------------------------------------------------------------------------

def git(*args, cwd=None, log="deps.log"):
    # Submodule paths get long on Windows.
    return run(["git", "-c", "core.longpaths=true", *args], cwd=cwd, log=log)


def apply_patch(repo, patch):
    """Applies the patch unless it is already there; refuses to touch a tree that has diverged."""
    patch = str(ROOT / patch)
    if run(["git", "-C", repo, "apply", "--reverse", "--check", patch], log="deps.log", check=False) == 0:
        say("   patch já aplicado: %s" % Path(patch).name)
        return
    if run(["git", "-C", repo, "apply", "--check", patch], log="deps.log", check=False) != 0:
        fail("o patch %s não se aplica a %s (alterações locais?). Nada foi sobrescrito." % (Path(patch).name, repo))
    git("-C", repo, "apply", patch)
    say("   patch aplicado: %s" % Path(patch).name)


def ensure_dependency(name):
    url, commit, submodules, patch = DEPENDENCIES[name]
    path = EXT / name
    if not (path / ".git").exists():
        say("   clonando %s" % name)
        git("-c", "core.autocrlf=false", "clone", "--no-checkout", url, str(path))
        git("-C", str(path), "config", "core.longpaths", "true")
        git("-C", str(path), "checkout", "--detach", commit)
    code, head = capture(["git", "-C", path, "rev-parse", "HEAD"])
    if head != commit:
        fail("%s está em %s, mas o projeto fixa %s. Suas alterações foram preservadas; revise manualmente." % (path, head[:12], commit[:12]))
    if submodules is None:
        git("-C", str(path), "submodule", "update", "--init", "--recursive")
    elif submodules:
        git("-C", str(path), "submodule", "update", "--init", "--depth", "1", "--", *submodules)
    if patch:
        apply_patch(str(path), patch)
    say("   %s @ %s" % (name, commit[:10]))


def host_sdk_binary():
    exe = "rexglue.exe" if IS_WINDOWS else "rexglue"
    base = EXT / ("rexglue-host-" + HOST_SDK_VERSION)
    for candidate in base.glob("*/bin/" + exe):
        return candidate
    return None


def ensure_host_sdk():
    existing = host_sdk_binary()
    if existing:
        say("   gerador de código: %s" % existing.relative_to(ROOT))
        return existing
    plat = host_platform()
    if plat not in HOST_SDK:
        fail("sem gerador de código publicado para %s." % plat)
    asset = "rexglue-sdk-%s-%s.zip" % (HOST_SDK_VERSION, plat)
    base = EXT / ("rexglue-host-" + HOST_SDK_VERSION)
    archive = download("https://github.com/rexglue/rexglue-sdk/releases/download/v%s/%s" % (HOST_SDK_VERSION, asset),
                       base / asset, HOST_SDK[plat])
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            z.extract(info, base)
            if not IS_WINDOWS and info.external_attr >> 16 & 0o111:
                os.chmod(base / info.filename, 0o755)
    binary = host_sdk_binary()
    if not binary:
        fail("o pacote do gerador não contém bin/rexglue.")
    return binary


def stage_deps(args):
    step("Dependências (revisões fixas e patches do projeto)")
    EXT.mkdir(exist_ok=True)
    say("   RexGlue e Plume: submódulos de thirdparty/ (sem patches)")
    git("-C", str(ROOT), "submodule", "update", "--init", "--recursive")
    for name in DEPENDENCIES:
        ensure_dependency(name)
    ensure_host_sdk()
    if args.skip_shader_tool:
        return
    if not linux_has_wsl():
        fail("WSL2 não encontrado: a ferramenta de shaders é compilada no Linux. Rode 'prepare.py doctor'.")
    missing = missing_shader_tools()
    if missing:
        fail("faltam ferramentas no Linux/WSL: %s\n  sudo apt install -y git clang cmake ninja-build spirv-tools" % ", ".join(missing))
    say("   preparando o conversor de shaders (clona e compila na primeira vez; vários minutos)")
    script = to_linux_path(TOOLS / "shaders" / "recompile_shaders.sh")
    linux_bash("%s bash %s --prepare-only" % (shader_work_env(), shlex.quote(script)), log="shader-tool.log")


def shader_work_env():
    work = os.environ.get("XENOS_RECOMP_WORK")
    return "XENOS_RECOMP_WORK=%s" % (shlex.quote(work) if work else "$HOME/.cache/renut-xenosrecomp")


# ---- stage: game ---------------------------------------------------------------------------------

def state_path():
    return OUT / "state.json"


def load_state():
    try:
        return json.loads(state_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_state(state):
    OUT.mkdir(parents=True, exist_ok=True)
    state_path().write_text(json.dumps(state, indent=1), encoding="utf-8")


def find_ci(folder, name):
    for entry in os.listdir(folder):
        if entry.lower() == name.lower():
            return Path(folder) / entry
    return None


def stage_game(args):
    step("Jogo")
    source = Path(args.game).expanduser() if args.game else None
    state = load_state()
    if source is None:
        if not state.get("game_dir"):
            fail("informe o jogo: --game <arquivo.iso | pasta extraída | default.xex>")
        source = Path(state["game_dir"])
    if not source.exists():
        fail("%s não existe." % source)
    bundle = None
    if source.is_file() and source.suffix.lower() == ".iso":
        sys.path.insert(0, str(TOOLS))
        import xdvdfs
        try:
            disc = xdvdfs.Disc(str(source))
        except xdvdfs.DiscError as e:
            fail("%s: %s" % (source.name, e))
        folder = OUT / "game"
        have = sum(f.stat().st_size for f in folder.rglob("*") if f.is_file()) if folder.exists() else 0
        if shutil.disk_usage(ROOT).free < max(0, disc.total - have) + 5 * 2 ** 30:
            fail("espaço insuficiente para extrair o disco (%s)." % human(disc.total))
        say("   extraindo %d arquivos (%s) para %s" % (len(disc.entries), human(disc.total), folder.relative_to(ROOT)))
        marks = [0]

        def progress(done, total):
            if done * 20 // total != marks[0]:
                marks[0] = done * 20 // total
                say("   %d%%" % (marks[0] * 5))
        disc.extract(str(folder), progress=progress)
        source = folder
    if source.is_dir():
        xex = find_ci(source, "default.xex")
        bundle = find_ci(source, "Bundle")
    else:
        xex = source
        bundle = find_ci(source.parent, "Bundle")
    if not xex or not Path(xex).is_file():
        fail("default.xex não encontrado em %s." % source)
    with open(xex, "rb") as f:
        if f.read(4) != b"XEX2":
            fail("%s não é um executável Xbox 360 (XEX2)." % xex)
    digest = sha256_of(xex)
    if digest != XEX_SHA256:
        fail("este default.xex (%s...) não é o da edição europeia validada (%s...). "
             "Só esse executável tem código gerado e hooks compatíveis." % (digest[:12], XEX_SHA256[:12]))
    target = ROOT / "assets" / "default.xex"
    target.parent.mkdir(exist_ok=True)
    if not target.is_file() or sha256_of(target) != digest:
        shutil.copyfile(xex, target)
    say("   default.xex verificado e instalado em assets/")
    state["game_dir"] = str(source)
    state["bundle"] = str(bundle) if bundle else ""
    save_state(state)
    if not bundle:
        say("   AVISO: pasta Bundle não encontrada; o passo de shaders vai precisar dela.")


# ---- stage: codegen ------------------------------------------------------------------------------

# cr_as_local turns ctx.cr6 into a plain local, so the same line is emitted without the "ctx." prefix.
UNRESOLVED_BEFORES = ('if (!ctx.cr6.eq) REX_FATAL("Unresolved branch from 0x82C79F68 to 0x82C79EE4");',
                      'if (!cr6.eq) REX_FATAL("Unresolved branch from 0x82C79F68 to 0x82C79EE4");')
UNRESOLVED_AFTERS = ("if (!ctx.cr6.eq) goto loc_82C79EE4;", "if (!cr6.eq) goto loc_82C79EE4;")


def fix_generated(folder):
    """RexGlue 0.10.0 emits a fatal branch for a label inside the same function; replaces that one reviewed line."""
    files = sorted(Path(folder).glob("*.cpp"))
    texts = {}
    for f in files:
        text = f.read_text(encoding="utf-8", errors="replace")
        if any(s in text for s in UNRESOLVED_BEFORES + UNRESOLVED_AFTERS):
            texts[f] = text
    hits = [f for f, t in texts.items() if any(s in t for s in UNRESOLVED_BEFORES)] or list(texts)
    if len(hits) != 1:
        fail("esperado exatamente um desvio conhecido em generated/android; encontrados %d. Regenere o código." % len(hits))
    path = hits[0]
    text = texts[path]
    pairs = list(zip(UNRESOLVED_BEFORES, UNRESOLVED_AFTERS))
    before, after = next(((b, a) for b, a in pairs if b in text), None) or next((b, a) for b, a in pairs if a in text)
    needle = before if before in text else after
    position = text.index(needle)
    start = text.rfind("DEFINE_REX_FUNC(", 0, position)
    body = text[start:position]
    if not body.startswith("DEFINE_REX_FUNC(sub_82C79EE4)") or "loc_82C79EE4:" not in body:
        fail("o destino do desvio não pertence à função esperada; correção não aplicada.")
    if needle == before:
        path.write_text(text.replace(before, after), encoding="utf-8", newline="")
    for f in files:
        if f != path and 'REX_FATAL("Unresolved' in f.read_text(encoding="utf-8", errors="replace"):
            fail("há outros desvios não resolvidos em %s; revise antes de compilar." % f.name)
    if 'REX_FATAL("Unresolved' in path.read_text(encoding="utf-8", errors="replace"):
        fail("restam desvios não resolvidos em %s." % path.name)


def stage_codegen(args):
    step("Recompilação do executável (C++ ARM64)")
    xex = ROOT / "assets" / "default.xex"
    if not xex.is_file() or sha256_of(xex) != XEX_SHA256:
        fail("assets/default.xex ausente ou diferente da edição validada. Rode o passo 'game'.")
    binary = host_sdk_binary()
    if not binary:
        fail("gerador de código ausente. Rode o passo 'deps'.")
    helper = GENERATED / "rexglue.cmake"
    saved = helper.read_bytes() if helper.is_file() else None
    try:
        run([binary, "--log-level", "info", "codegen", "renut_android_manifest.toml"], cwd=ROOT, log="codegen.log")
        # SDK 0.10 also overwrites the root helper regardless of the output directory.
        (GENERATED / "android").mkdir(parents=True, exist_ok=True)
        shutil.copyfile(helper, GENERATED / "android" / "rexglue.cmake")
    finally:
        if saved is not None:
            helper.write_bytes(saved)
        elif helper.exists():
            helper.unlink()
    fix_generated(GENERATED / "android")
    say("   C++ gerado em generated/android")


# ---- stage: shaders ------------------------------------------------------------------------------

def cache_hashes(cache_cpp):
    """Container hashes of the entries in a generated shader_cache.cpp."""
    text = Path(cache_cpp).read_text(encoding="utf-8", errors="replace")
    return {int(m, 16) for m in re.findall(r"^\t\{ (0x[0-9A-Fa-f]+),", text, re.M)}


def report_runtime_shaders(out_dir):
    """Tells how many of the shaders that only exist at run time came out of the captures.

    Bloom and the tessellated water are among them: without their shaders the draws are dropped and the
    effects simply do not appear, so a short capture session shows up as exactly that.
    """
    listed = set()
    for line in (TOOLS / "shaders" / "runtime-shaders.txt").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            listed.add(int(line, 16))
    have = cache_hashes(out_dir / "shader_cache.cpp")
    missing = listed - have
    say("   shaders gerados em runtime: %d de %d presentes" % (len(listed) - len(missing), len(listed)))
    domain = []
    log = LOGS / "shaders-recompile.log"
    if log.is_file():
        text = log.read_text(encoding="utf-8", errors="replace")
        text = text[max(text.rfind(chr(10) + "$ "), 0):]  # the log is appended to: only the last run counts
        domain = re.findall(r"Domain shader (0x[0-9A-Fa-f]+) is not in the input", text)
    if domain:
        say("   FALTAM os shaders de domínio da água tesselada: %s" % ", ".join(sorted(set(domain))))
    if missing:
        say("   AVISO: faltam %d shaders gerados em runtime. O que depende deles não aparece (bloom/brilho, água "
            "tesselada, outros efeitos)." % len(missing))
        say("   Eles só são capturados quando o jogo os cria. Jogue passando por água, lojas e pelo menos um mundo, "
            "depois repita: prepare.py dumps, prepare.py shaders, e recompile o app.")
        say("   As capturas ficam no aparelho e se acumulam entre sessões; não apague os dados do app.")
    return len(missing)


def python_tool(script, *args, log):
    run([sys.executable, "-I", str(TOOLS / "shaders" / script), *[str(a) for a in args]], log=log)


def build_xex_bin(target):
    sys.path.insert(0, str(TOOLS))
    import xex_basefile
    say("   extraindo a imagem do executável (xex.bin)")
    image = xex_basefile.extract((ROOT / "assets" / "default.xex").read_bytes())
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(image)
    return image


def build_internal(image, folder, extra_ucode):
    """Direct3D's own vertex shaders live in the executable as bare microcode, not as containers."""
    table = json.loads((TOOLS / "shaders" / "internal_vs.json").read_text(encoding="utf-8"))
    staging = OUT / "internal-ucode"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    files = []
    for entry in table:
        data = bytearray(image[entry["offset"]:entry["offset"] + entry["size"]])
        for index, value in entry["patch"].items():
            data[int(index)] = value
        path = staging / (entry["name"] + ".ucode")
        path.write_bytes(bytes(data))
        files.append(path)
    for path in extra_ucode:
        shutil.copyfile(path, staging / Path(path).name)
        files.append(staging / Path(path).name)
    folder.mkdir(parents=True, exist_ok=True)
    python_tool("wrap_raw_ucode.py", folder, *sorted(set(files)), log="shaders-internal.log")


def decompress_xctd(source, destination):
    """Bundle/4f holds XCTD files (the console's file compression); a small C decoder over libmspack unpacks them."""
    mspack = ROOT / "thirdparty" / "rexglue-sdk" / "thirdparty" / "libmspack" / "libmspack" / "mspack"
    if not (mspack / "lzxd.c").is_file():
        fail("libmspack ausente em thirdparty/. Rode o passo 'deps'.")
    destination.mkdir(parents=True, exist_ok=True)
    script = to_linux_path(TOOLS / "shaders" / "decompress_xctd.sh")
    env = "MSPACK_DIR=%s XCTD_BIN=%s" % (shlex.quote(to_linux_path(mspack)),
                                         shlex.quote(to_linux_path(OUT / "xctd_decompress")))
    linux_bash("%s bash %s %s %s" % (env, shlex.quote(script), shlex.quote(to_linux_path(source)),
                                     shlex.quote(to_linux_path(destination))), log="shaders-xctd-unpack.log")


def merged_domain_list(dumps):
    """The repository's list of vertex shaders run as the domain stage of a tessellated draw, plus the ones the
    device log named (prepare.py dumps)."""
    lines = (TOOLS / "shaders" / "domain-shaders.txt").read_text(encoding="utf-8").splitlines()
    extra = dumps / "domain-shaders.txt"
    if extra.is_file():
        have = {l.strip().upper() for l in lines}
        added = [l.strip() for l in extra.read_text(encoding="utf-8").splitlines()
                 if l.strip() and l.strip().upper() not in have]
        if added:
            say("   formas de domínio extras vindas do aparelho: %s" % ", ".join(added))
            lines += ["", "# from the device log (prepare.py dumps)"] + added
    target = OUT / "domain-shaders.txt"
    OUT.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return target


def stage_shaders(args):
    step("Shaders (Xenos -> SPIR-V)")
    state = load_state()
    bundle = state.get("bundle") or ""
    if not bundle or not os.path.isdir(bundle):
        fail("pasta Bundle do jogo desconhecida. Rode: prepare.py game --game <...>")
    if not (ROOT / "assets" / "default.xex").is_file():
        fail("assets/default.xex ausente. Rode o passo 'game'.")
    if not linux_has_wsl():
        fail("WSL2 não encontrado. Rode 'prepare.py doctor'.")
    inp = OUT / "shader-input"
    inp.mkdir(parents=True, exist_ok=True)

    # 1. the executable, and the Direct3D shaders inside it
    image = build_xex_bin(inp / "xex.bin")
    dumps = Path(args.dumps) if args.dumps else (OUT / "dumps")
    extra_ucode = sorted(dumps.rglob("vs_*.ucode")) if dumps.is_dir() else []
    build_internal(image, inp / "internal", extra_ucode)

    # 2. every shader the game ships in its bundles
    caff = inp / "caff"
    if not (caff.is_dir() and any(caff.iterdir())) or args.force:
        say("   extraindo shaders dos bundles do jogo (vários minutos)")
        python_tool("extract_caff_shaders.py", bundle, caff, log="shaders-caff.log")
    xctd_source = Path(bundle) / "4f"
    xctd_ready = Path(args.xctd_dir) if args.xctd_dir else OUT / "xctd-raw"
    if args.xctd_dir is None and xctd_source.is_dir():
        say("   descomprimindo os arquivos XCTD de Bundle/4f")
        decompress_xctd(xctd_source, xctd_ready)
    xctd_out = inp / "xctd"
    if xctd_ready.is_dir() and (not xctd_out.is_dir() or args.force):
        python_tool("extract_caff_shaders.py", xctd_ready, xctd_out, log="shaders-xctd.log")

    # 3. what only exists at run time (a second pass: see `prepare.py dumps`)
    if dumps.is_dir():
        shader_dump = (dumps / "shader-dump") if (dumps / "shader-dump").is_dir() else dumps
        target = inp / "dump"
        target.mkdir(exist_ok=True)
        count = 0
        for f in shader_dump.glob("*.bin"):
            shutil.copyfile(f, target / f.name)
            count += 1
        say("   %d capturas de runtime incluídas (%s)" % (count, dumps))
    else:
        say("   sem capturas de runtime: ~150 shaders gerados pelo jogo só aparecem depois de uma sessão no aparelho "
            "(veja 'prepare.py dumps')")

    # 4. recompile
    out = OUT / "shaders-out"
    out.mkdir(parents=True, exist_ok=True)
    domain = merged_domain_list(dumps)
    script = to_linux_path(TOOLS / "shaders" / "recompile_shaders.sh")
    say("   recompilando (clona/compila a ferramenta na primeira vez)")
    linux_bash("%s DOMAIN_SHADERS=%s bash %s %s %s" % (shader_work_env(), shlex.quote(to_linux_path(domain)),
                                                       shlex.quote(script), shlex.quote(to_linux_path(inp)),
                                                       shlex.quote(to_linux_path(out))), log="shaders-recompile.log")

    report_runtime_shaders(out)

    # 5. install what the Android build consumes
    if args.no_install:
        say("   --no-install: resultado em %s" % out.relative_to(ROOT))
        return
    target = GENERATED / "shaders"
    target.mkdir(parents=True, exist_ok=True)
    for name in ("shader_cache.cpp", "shader_common.h", "shader_spirv.smolv", "shader_ucode_map.inc"):
        if not (out / name).is_file():
            fail("a recompilação não produziu %s." % name)
        shutil.copyfile(out / name, target / name)
    say("   cache de shaders instalado em generated/shaders")


# ---- stage: dumps --------------------------------------------------------------------------------

def find_adb():
    found = shutil.which("adb")
    if found:
        return found
    sdk = find_android_sdk()
    if sdk:
        candidate = os.path.join(sdk, "platform-tools", "adb.exe" if IS_WINDOWS else "adb")
        if os.path.isfile(candidate):
            return candidate
    return None


def stage_dumps(args):
    step("Capturas de shader do aparelho")
    adb = find_adb()
    if not adb:
        fail("adb não encontrado (instale as platform-tools do Android SDK).")
    state = subprocess.run([adb, "get-state"], capture_output=True, text=True)
    if state.stdout.strip() != "device":
        fail("nenhum aparelho conectado pelo adb (ative a depuração USB e autorize este computador).")
    target = OUT / "dumps"
    target.mkdir(parents=True, exist_ok=True)
    pulled = 0
    for name in ("shader-dump", "plume-miss"):
        result = subprocess.run([adb, "exec-out", "run-as", XEX_PACKAGE, "tar", "c", "-C", "files", name], capture_output=True)
        if not result.stdout:
            continue
        temp = OUT / (name + ".tar")
        temp.write_bytes(result.stdout)
        try:
            with tarfile.open(temp) as t:
                for member in t.getmembers():
                    if member.isfile() and ".." not in member.name:
                        try:
                            t.extract(member, target, filter="data")
                        except TypeError:  # Python without extraction filters
                            t.extract(member, target)
                        pulled += 1
        except tarfile.TarError:
            pass
        temp.unlink()
    log = subprocess.run([adb, "exec-out", "run-as", XEX_PACKAGE, "cat", "files/rexglue-runtime.log"], capture_output=True)
    wanted = set(re.findall(r"patch draw dropped: vs=([0-9A-Fa-f]{16})", log.stdout.decode("utf-8", errors="replace")))
    if wanted:
        # Water that has no domain form (tools/shaders/domain-shaders.txt) is dropped; the next recompile adds it.
        known = set()
        domain_file = target / "domain-shaders.txt"
        if domain_file.is_file():
            known = {l.strip().upper() for l in domain_file.read_text(encoding="utf-8").splitlines() if l.strip()}
        merged = sorted(known | {h.upper() for h in wanted})
        domain_file.write_text("\n".join(merged) + "\n", encoding="utf-8")
        say("   %d shader(s) de água sem forma de domínio registrados em %s" % (len(merged), domain_file.relative_to(ROOT)))
    if not pulled and not wanted:
        fail("nada para baixar: o app precisa ser a build de depuração (run-as) e já ter rodado o jogo.")
    say("   %d arquivos em %s. Rode 'prepare.py shaders' de novo para incorporá-los." % (pulled, target.relative_to(ROOT)))


# ---- stage: apk (optional) -----------------------------------------------------------------------

def stage_apk(args):
    step("APK (Gradle)")
    sdk = find_android_sdk()
    java = find_java_home()
    if not sdk or not java:
        fail("Android SDK ou JDK não encontrados. Rode 'prepare.py doctor'.")
    props = ROOT / "android" / "local.properties"
    if not props.is_file():
        props.write_text("sdk.dir=%s\n" % sdk.replace("\\", "\\\\").replace(":", "\\:"), encoding="utf-8")
    env = {"JAVA_HOME": java}
    if IS_WINDOWS:
        # Gradle's socket directory may be remapped by packaged shells.
        socket_dir = OUT / "java-tmp"
        socket_dir.mkdir(parents=True, exist_ok=True)
        env["JAVA_TOOL_OPTIONS"] = "-Djdk.net.unixdomain.tmpdir=%s" % socket_dir
    wrapper = ROOT / "android" / ("gradlew.bat" if IS_WINDOWS else "gradlew")
    command = [wrapper] if IS_WINDOWS else ["sh", wrapper]  # no dependency on the executable bit
    run(command + ["assembleDebug", "--no-daemon", "--console=plain"], cwd=ROOT / "android", env=env, log="gradle.log")
    say("   APK: android/app/build/outputs/apk/debug/app-debug.apk")


# ---- main ----------------------------------------------------------------------------------------

def stage_all(args):
    stage_deps(args)
    stage_game(args)
    stage_codegen(args)
    stage_shaders(args)
    say("\nPronto. Agora, no Android Studio: abra a pasta android/ e use Build > Build APK(s) "
        "(ou 'prepare.py apk').\nDepois de jogar uma vez no aparelho, 'prepare.py dumps' e 'prepare.py shaders' "
        "completam os shaders gerados em runtime.")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Prepara o código e os shaders do reNut para o build Android.")
    parser.add_argument("stage", choices=["doctor", "all", "deps", "game", "codegen", "shaders", "dumps", "apk"])
    parser.add_argument("--game", help="ISO, pasta extraída do jogo ou default.xex")
    parser.add_argument("--dumps", help="pasta com capturas de shader do aparelho (padrão: out/prepare/dumps)")
    parser.add_argument("--xctd-dir", help="pasta com os arquivos XCTD já descomprimidos (avançado; normalmente não é necessário)")
    parser.add_argument("--skip-shader-tool", action="store_true", help="deps: não prepara o conversor de shaders")
    parser.add_argument("--force", action="store_true", help="refaz extrações já existentes")
    parser.add_argument("--no-install", action="store_true", help="shaders: deixa o resultado em out/prepare sem tocar em generated/")
    args = parser.parse_args()
    stages = {"doctor": stage_doctor, "all": stage_all, "deps": stage_deps, "game": stage_game,
              "codegen": stage_codegen, "shaders": stage_shaders, "dumps": stage_dumps, "apk": stage_apk}
    code = stages[args.stage](args)
    return code or 0


if __name__ == "__main__":
    sys.exit(main())
