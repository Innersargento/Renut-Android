# Compilar o reNut para Android (Windows e Linux)

O APK contém código e shaders **derivados do seu jogo**, então ele não pode ser
distribuído pronto: cada pessoa gera o seu a partir do próprio disco. O script
`tools/prepare.py` faz tudo o que é derivado do jogo; o app em si é compilado
pelo Android Studio (ou pelo Gradle), como qualquer projeto Android.

```
python tools/prepare.py all --game <jogo>      ->  depois: Android Studio > Build APK
```

Nada gerado sai da sua máquina e nada disso entra no Git (`generated/`,
`assets/`, `external/` e `out/` são ignorados).

> **Leia primeiro:** só a **edição europeia** de Banjo-Kazooie: Nuts & Bolts,
> **sem atualização**, funciona. O script confere o SHA-256 do `default.xex` e
> recusa qualquer outro.

---

## 1. O que você precisa

| Item | Windows 10/11 | Linux |
| --- | --- | --- |
| Python 3.8+ | python.org (marque *Add to PATH*) | já vem; senão `python3` |
| Git | git-scm.com | pacote `git` |
| Compilador e ferramentas de shader | **WSL2 com Ubuntu** (a ferramenta é compilada no Linux) | `clang cmake ninja spirv-tools` |
| JDK **17 a 21** | o do Android Studio (pasta `jbr`) | `openjdk-17-jdk` ou o `jbr` do Android Studio |
| Android SDK | Android Studio (baixa o SDK) | Android Studio **ou** as ferramentas de linha de comando (seção 3) |
| Espaço livre | ~30 GB | ~30 GB |
| O jogo | `.iso`, pasta extraída ou `default.xex` | idem |

O JDK 22 ou mais novo **não** serve (quebra o Gradle usado). Se `JAVA_HOME`
apontar para um, corrija antes.

### Windows: preparar o WSL (uma vez)

Em um PowerShell **como administrador**: `wsl --install -d Ubuntu`, reinicie se
pedir e abra o Ubuntu uma vez para criar o usuário. Depois, dentro do Ubuntu:

```bash
sudo apt update
sudo apt install -y git clang cmake ninja-build spirv-tools
```

### Linux: instalar os pacotes

```bash
# Debian / Ubuntu / Mint
sudo apt update && sudo apt install -y git python3 clang cmake ninja-build spirv-tools openjdk-17-jdk unzip
# Fedora
sudo dnf install -y git python3 clang cmake ninja-build spirv-tools java-17-openjdk-devel unzip
# Arch / Manjaro
sudo pacman -S --needed git python clang cmake ninja spirv-tools jdk17-openjdk unzip
```

Distribuições de CPU não x86-64 (ARM) também têm gerador de código publicado
(`linux-arm64`), mas só x86-64 foi usada para validar este guia.

---

## 2. Passo a passo

### 2.1 Baixe o projeto num caminho curto

```bash
git clone <url do repositório> reNut
cd reNut
```

- **Windows:** use um caminho curto, como `C:\reNut`. O Windows limita caminhos a
  260 caracteres e os submódulos são profundos.
- **Linux dentro do WSL:** clone no sistema de arquivos do Linux (`~/reNut`), não
  em `/mnt/c/...`: é muitas vezes mais rápido.

### 2.2 Confira a máquina

```bash
python3 tools/prepare.py doctor        # Windows: python tools\prepare.py doctor
```

Cada linha sai como `[ok]` ou `[FALTA]` com o comando para resolver. Só siga
adiante com tudo `[ok]` (o item da licença do SDK pode ser resolvido na seção 3).

### 2.3 Gere tudo

```bash
python3 tools/prepare.py all --game "/caminho/Banjo-Kazooie NB (Europe).iso"
```

`--game` aceita:

- o arquivo `.iso` (o script extrai para `out/prepare/game`, ~6,6 GB);
- uma pasta já extraída, com `default.xex` e `Bundle/`;
- o próprio `default.xex` (os shaders ainda precisam da pasta `Bundle` ao lado).

O que acontece, em ordem (a primeira vez demora; cada etapa é segura de repetir
e pula o que já está pronto):

| Etapa | O que faz | Tempo aproximado |
| --- | --- | --- |
| `deps` | inicializa os submódulos (RexGlue e Plume, sem patches), clona libadrenotools e libucontext nas revisões fixas, baixa o gerador de código, compila o conversor de shaders | 10–25 min (download de ~1,3 GB + compilação) |
| `game` | extrai o ISO e confere o `default.xex` | 2–5 min |
| `codegen` | recompila o executável do Xbox 360 para C++ (`generated/android`) | segundos |
| `shaders` | extrai e descomprime os shaders do jogo e converte tudo para SPIR-V (`generated/shaders`) | ~20 min (a conversão usa em grande parte um único núcleo) |

A saída mostra o que está sendo feito. Um minuto sem mensagens é normal em
etapas longas (a cada minuto aparece `… N min`). Os registros completos ficam
em `out/prepare/logs/`; se algo falhar, o script mostra o final do registro e o
caminho do arquivo.

Ao terminar aparece **"Pronto."**

### 2.4 Compile o app

**Pelo Android Studio:** abra a pasta `android/` (File > Open), espere o *Gradle
sync*, conecte o aparelho (ou use o menu **Build > Build APK(s)**).

**Pela linha de comando:**

```bash
python3 tools/prepare.py apk
```

O APK sai em `android/app/build/outputs/apk/debug/app-debug.apk`. A primeira
compilação baixa o NDK 29 e o CMake 3.22 e compila os ~39 mil trechos do jogo:
leva um bom tempo e vários GB de disco em `android/app/.cxx` e `build/`.

### 2.5 Instale no aparelho

```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

(`adb` vem em `platform-tools` do Android SDK.) No aparelho, ative **Opções do
desenvolvedor > Depuração USB**. No Linux, se `adb devices` listar o aparelho
como `no permissions`, instale as regras udev (`sudo apt install
android-sdk-platform-tools-common`) e reconecte o cabo.

Na primeira abertura o app pede para importar o jogo (o mesmo ISO ou pasta);
isso é só para os arquivos de dados e é independente do que você compilou aqui.

---

## 3. Android SDK sem Android Studio (Linux)

Se você não quer instalar o Android Studio:

```bash
mkdir -p ~/Android/Sdk/cmdline-tools && cd ~/Android/Sdk/cmdline-tools
# baixe "Command line tools only" para Linux em
# https://developer.android.com/studio#command-line-tools-only
unzip ~/Downloads/commandlinetools-linux-*_latest.zip
mv cmdline-tools latest
export ANDROID_HOME=~/Android/Sdk
yes | ~/Android/Sdk/cmdline-tools/latest/bin/sdkmanager --licenses
~/Android/Sdk/cmdline-tools/latest/bin/sdkmanager "platform-tools" "platforms;android-35"
```

`sdkmanager --licenses` mostra os termos do Android SDK e pede que você os
aceite; leia e responda você mesmo. O Gradle baixa o restante (build-tools, NDK
29, CMake 3.22) sozinho depois disso. Deixe `ANDROID_HOME` definido (por
exemplo em `~/.bashrc`) ou crie `android/local.properties` com
`sdk.dir=/home/voce/Android/Sdk`.

---

## 4. Segunda passada: shaders gerados em runtime

Cerca de 150 shaders (3%) **não existem em nenhum arquivo do jogo**: o próprio
jogo os gera ou altera enquanto roda, e só ele sabe quando. Eles só podem ser
capturados no aparelho, **e só os que o jogo chegou a criar durante as suas
sessões**.

Entre eles estão justamente o **bloom (o brilho)** e a **água tesselada**. Sem
esses shaders o app roda, mas esses desenhos são descartados: o brilho não
aparece e a água some. Se for isso que você vê, faltam capturas.

```bash
python3 tools/prepare.py dumps         # baixa as capturas do aparelho via adb
python3 tools/prepare.py shaders       # recompila incluindo as capturas
python3 tools/prepare.py apk           # (ou Build APK no Android Studio) e reinstale
```

O passo `shaders` termina com um resumo:

```
   shaders gerados em runtime: 152 de 152 presentes
```

Se aparecer um número menor, o resumo diz quantos faltam e, no caso da água,
quais shaders de domínio faltam (`FALTAM os shaders de domínio da água ...`).
Enquanto não for **152 de 152**, faltam efeitos. Para completar:

1. Jogue com a build de depuração instalada **passando pelo que falta**: a
   água (o voo de abertura e o mar do primeiro mundo), lojas e pelo menos um
   mundo inteiro. Quanto mais variado, mais shaders são criados.
2. Repita `dumps`, `shaders` e recompile o app (as capturas **se acumulam** no
   aparelho entre as sessões; só pedem uma nova passada).
3. Reinstale o APK novo. Os shaders ficam dentro do app: sem recompilar, nada
   muda.

Cuidados:

- `dumps` exige a **build de depuração** (usa `run-as`), o aparelho conectado
  com a depuração USB autorizada e o jogo já ter rodado.
- **Não apague os dados do app** nem desinstale entre as passadas: as capturas
  moram em `files/shader-dump` do aparelho. Atualizar o app por cima (instalar
  `-r`) mantém tudo.
- Os arquivos `plume-miss` também são baixados, mas só os do tipo `vs_*.ucode`
  são aproveitados; para pixel shaders o que vale são as capturas completas
  (`shader-dump`).

---

### A água continua sem aparecer, mesmo com 150 de 152

A água tesselada é desenhada pelo shader de vértice do jogo rodando como
"estágio de domínio". Nada no shader diz isso: a lista está em
`tools/shaders/domain-shaders.txt`, e um shader de água que não esteja nela é
descartado (`patch_draws=0` e `skipped_prim` fixo no log).

O app avisa quando isso acontece, uma vez por shader, em
`files/rexglue-runtime.log`:

```
Renut: Plume patch draw dropped: vs=E3EE93145D133624 has no domain form (...)
```

`prepare.py dumps` lê essas linhas e `prepare.py shaders` já as inclui na
compilação seguinte (aparece como `formas de domínio extras vindas do
aparelho`). Então o ciclo é o mesmo: jogar, `dumps`, `shaders`, recompilar.
Se achar um hash que vale para todo mundo, acrescente-o em
`domain-shaders.txt`.

Um shader de domínio que o jogo cria a partir de microcódigo "solto" só chega
pelo `plume-miss` (por isso o `dumps` baixa essa pasta também): sem ela o
container nem existe para a lista apontar.

---

## 5. Problemas comuns

| Mensagem / sintoma | O que fazer |
| --- | --- |
| `não é a edição europeia validada` | O `default.xex` tem outro SHA-256 (outra região ou atualização). Só a europeia sem atualização tem código gerado e hooks compatíveis. |
| `está em X, mas o projeto fixa Y` | Uma pasta em `external/` foi alterada à mão. O script não sobrescreve nada: mova a pasta ou volte à revisão fixa. |
| `o patch ... não se aplica` | Mesmo caso: alterações locais em `external/`. |
| `faltam ferramentas no Linux/WSL: ...` | Instale os pacotes da seção 1 e rode `doctor` de novo. |
| `espaço insuficiente` | Libere espaço; o ISO extraído usa ~6,6 GB e o build nativo vários GB. |
| Gradle falha por versão do Java | Use JDK 17 a 21 e defina `JAVA_HOME`; `doctor` mostra o que foi encontrado. |
| `Failed to install the following Android SDK packages ... licenses` | Aceite as licenças: `yes \| sdkmanager --licenses`. |
| `Permission denied` ao rodar `./gradlew` | Use `python3 tools/prepare.py apk` ou `sh android/gradlew ...`. |
| Quer repetir uma etapa do zero | Apague a pasta correspondente (`generated/android`, `generated/shaders`, `out/prepare/shader-input`) e rode a etapa de novo; `--force` refaz as extrações de shaders. |
| Erro durante `deps` (rede) | Repita o comando: o que já foi baixado é reaproveitado. |
| `rexglue: error while loading shared libraries` ou erro de `GLIBC` (Linux) | A distribuição é antiga demais para o gerador de código publicado. Use uma versão mais nova (Ubuntu 22.04 ou superior, Fedora atual). |

Outras opções do script: `prepare.py <etapa>` roda uma etapa só
(`deps`, `game`, `codegen`, `shaders`, `dumps`, `apk`); `RENUT_EXTERNAL=<pasta>`
muda onde ficam as dependências; `XENOS_RECOMP_WORK=<pasta>` muda onde o
conversor de shaders é compilado (padrão `~/.cache/renut-xenosrecomp`).

## 6. Como funciona (resumo)

- **Dependências fixas:** o RexGlue (tag `v0.10.0`) e o Plume são submódulos em
  `thirdparty/`, usados como foram publicados — o reNut não os altera nem aplica
  patches (veja `docs/DEPENDENCIAS.md`). `deps` inicializa os submódulos e clona
  o resto (libadrenotools, libucontext) no *commit* exato; nada é compilado com
  versão "mais nova".
- **Código do jogo:** o gerador do RexGlue traduz o PowerPC do `default.xex`
  para C++ (`generated/android`), que o NDK compila para ARM64.
- **Shaders:** a imagem do executável (`xex.bin`) é extraída do próprio
  `default.xex`; os bundles do jogo (inclusive os `.xctd`, descomprimidos por
  `tools/shaders/xctd_decompress.c`) trazem o resto. O conversor
  (reNut-XenosRecomp + `tools/shaders/renut-xenosrecomp.patch`) produz SPIR-V.
- **Scripts anteriores:** os `bootstrap-*.ps1`, `generate-android.ps1` e
  `fix-android-codegen.ps1` (só Windows) foram substituídos por `prepare.py`.
