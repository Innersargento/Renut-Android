# Renut Android — runtime ARM64 e apresentação Plume

APK de desenvolvimento ARM64 com importação local, identificação do XEX europeu, runtime RexGlue 0.10.0, código do jogo recompilado, backend Xenos/Vulkan, teste Plume e integração opcional com adrenotools. **Já inicia o executável e renderiza imagens reais da abertura; menu e jogabilidade ainda não foram confirmados.** O build local contém código máquina derivado do XEX; não empacota a ISO, o XEX nem os assets. Código gerado e binários permanecem fora do Git.

Estado de retomada: [RETOMADA-2026-09-13.md](../docs/RETOMADA-2026-09-13.md).

## Compilar no Windows

Instalar JDK 17 ou 21, SDK Android plataforma 35, NDK 29.0.14206865 e CMake Android 3.22.1. O Gradle Wrapper usa 8.10.2 e o plugin Android 8.8.2.

Na raiz do repositório:

```powershell
./tools/bootstrap-android.ps1
./tools/bootstrap-rexglue.ps1
# Coloque o default.xex europeu em assets/default.xex.
./tools/generate-android.ps1
# Crie android/local.properties com sdk.dir apontando para seu SDK.
./tools/build-android.ps1 -JavaHome 'C:/Program Files/Java/jdk-17'
```

Saída: `android/app/build/outputs/apk/debug/app-debug.apk`. O script de build contém um ajuste de diretório de sockets temporários do Java necessário neste ambiente Windows. Dependências ficam em `external/`, fora do Git, fixadas pelo bootstrap. As licenças distribuídas estão em `app/src/main/assets/licenses/`.

## O que testar

- **Iniciar Banjo (experimental):** requer os assets completos, inicia o ponto de entrada real com o backend Xenos/Vulkan do RexGlue, em paisagem. Há controles básicos Start, A, B e direcional para testes de menu; não é um controle completo para jogar.
- **Configurar controle externo:** na tela inicial, selecione um controle USB ou Bluetooth. "Mapear automaticamente" aplica os botões e eixos padrão do Android; toque em cada comando para capturar manualmente uma tecla ou movimento de eixo. O perfil é salvo por controle e usado ao iniciar o jogo. A opção "Mostrar controles na tela" permite ocultar os comandos sobrepostos sem desativar o controle externo.
- **Vídeos do jogo:** o próprio jogo decodifica os filmes ASF/WMV3 originais e os desenha no pipeline gráfico, com moldura, fades, legendas e mistura com a cena 3D. Não é necessário preparar vídeos.
- **Importar apenas o XEX:** identifica SHA-256 e cabeçalho XEX2. A hash aceita corresponde ao executável fornecido pelo usuário e está em `config/european-build.json`.
- **Validar runtime ARM64:** processo separado que verifica troca de contexto, inicializa memória/kernel, registra e confere toda a tabela recompilada, carrega o XEX e executa a função original `0x82296D30` com dados controlados. Não chama o ponto de entrada do jogo. O relatório fica em `files/runtime-report.txt`; o log detalhado em `files/rexglue-runtime.log`. Ao voltar, o processo nativo é encerrado.
- **Testar runtime com Plume:** carrega o mesmo runtime e XEX, injeta o backend gráfico nativo e apresenta 180 quadros de teste pela chamada de kernel `VdSwap`. A imagem colorida quadriculada é produzida pelo teste na memória física convidada; não é uma cena do Banjo. Usa o driver selecionado na UI.
- **Importar imagem ISO:** copia a imagem para armazenamento privado, valida o XEX e extrai XDVDFS com progresso e cancelamento. Requer espaço temporário para ISO + extração (aproximadamente 14,5 GB decimais para esta imagem, além da reserva). A seleção ativa só muda após validação. Importações anteriores são preservadas nesta prévia; ainda não existe gerenciamento de exclusão pela UI. Não há retomada após encerrar o aplicativo.
- **Importar driver Adreno:** ZIP com `meta.json` e a biblioteca indicada por `libraryName` na raiz. Valida caminho, tamanho total, minApi quando declarado e ELF ARM64. Hooks adrenotools são empacotados no APK; o driver não é fornecido.
- **Testar Plume:** um processo separado carrega o driver selecionado, consulta os formatos da superfície e apresenta uma forma animada. Cada nova execução inicia outro processo, evitando trocar drivers em uma instância Vulkan viva.
- **Exportar diagnóstico:** salva relatório pelo seletor Android, incluindo identidade da GPU/driver, tempos CPU e timestamps GPU quando disponíveis.

O teste desenha uma carga mínima e serializa quadros com fence. Não mede desempenho do Banjo, não é benchmark térmico e não valida shaders, texturas, profundidade ou o runtime recompilado. O relatório usa a resolução efetiva da SurfaceView; o perfil 720p do jogo ainda não foi implementado.

## Validação realizada em 2026-09-13

- `assembleDebug`: compilação ARM64 bem-sucedida e instalação por ADB no SM-S918B, Android 16, SM8550 / Adreno 740.
- `lintDebug`: concluído sem erros; permanecem avisos de localização de textos, suporte ChromeOS e recomendação de API de espaço livre.
- Importação do XEX pelo seletor Android: concluída, hash europeia identificada, estado persistido.
- Leitor XDVDFS executado na ISO real no computador: 1.981 arquivos, 6.572.764.988 bytes extraídos previstos; hash do XEX interno coincide com o arquivo fornecido.
- Testes Java: extração de fixture, rejeição de caminhos de travessia, referência fora do arquivo, árvore malformada e imagem curta.
- Plume no driver do sistema: animação inspecionada visualmente e timestamps GPU válidos depois de corrigir o formato da superfície para RGBA8. A primeira tentativa em BGRA8 era inválida; seus números foram descartados.
- Ainda não validados: extração completa da ISO na UI Android, pacote de driver alternativo, falta real de espaço, cancelamento em todos os pontos, jogos/shaders e execução prolongada.

Uma execução válida de diagnóstico (570 amostras após 30 quadros de aquecimento; superfície 1032×1898) registrou quadro p50 8,32844 ms, p95 8,58974 ms, p99 8,78688 ms; preparação/submissão CPU p50 0,161823 ms; GPU p50 0,192604 ms e p95 0,197187 ms. Esses números caracterizam apenas a limpeza animada desta versão de debug.

## Integração RexGlue

Validação no SM-S918B, Android 16, em 2026-09-13: APK 0.2 instalado por atualização; 38.983 funções compiladas e verificadas na tabela do runtime; teste de fibers aprovado; memória virtual mapeada em `0x100000000`, física em `0x200000000`; kernel inicializado; XEX europeu carregado; função original `0x82296D30` executada por ponteiro obtido do dispatcher, copiando corretamente o byte de teste e retornando zero. O teste terminou e o processo foi encerrado ao voltar. Evidências locais: `out/renut-runtime-device-report.txt`, `out/rexglue-runtime-device.log` e `out/renut-runtime.png`.

O ELF de `librenut_runtime.so` foi inspecionado como `ELF64 / AArch64`. A desmontagem da função testada contém instruções ARM64 `ldr`, `ldrb`, `str`, `strb` e `ret`. Isso verifica a recompilação nativa de CPU; não comprova o boot nem o desempenho do jogo. Artefato local: `out/renut-0.2.0-runtime-debug.apk`, SHA-256 `76d189fef7fee273ae42d69ab10b4bb738220622e7b9dcf601fc591cd4d95998`.

`assembleDebug` e `lintDebug` finais concluídos: zero erros, 47 avisos. `app/lint.xml` limita exceções a classes SDL: BLE opcional desativado pela ausência de permissão e guardas existentes; listener de API 29 selecionado somente nessa versão; registro legado de receiver protegido por SDK<33. Não desativa verificações no código do reNut. A permissão normal VIBRATE acompanha o suporte de haptics da SDL.

O APK é um projeto Android separado e preserva o build desktop. `renut_android_manifest.toml` usa o mapa `config/renut_funcs.toml`, incluindo os endereços de setjmp/longjmp. Não importa automaticamente os hooks desktop de janela, cheats ou os fixes dependentes da versão antiga do SDK. A compatibilidade desses hooks ainda exige revisão antes de lançar o jogo.

SDK fixado em `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2` (0.10.0), gerador Windows oficial com SHA-256 verificado. O C++ gerado em `generated/android` é compilado pelo NDK para `librenut_runtime.so`; `librexruntime.so` reúne core, filesystem, system, kernel, UI sem backends desktop, entrada e áudio/XMA. A ligação usa a verificação de símbolos indefinidos do NDK, sem imports do jogo substituídos por funções vazias.

`android/rexglue/CMakeLists.txt` reutiliza as listas de fontes do SDK fixado. `android-platform.patch` adapta chrono/libc++, ucontext, mutexes Bionic e janela nativa Android. `libucontext` está fixado em `49e671dd52ff6791295d8161ad3b6da7dc5f6f9d`; os offsets da ABI são conferidos por static_assert e o teste faz suspensão/retomada de uma fiber. Arquivos importados usam armazenamento privado; a ponte de ContentResolver também implementa o acesso a content URIs do runtime.

As classes Java da SDL vêm do mesmo submódulo fixado, com inicialização explícita antes do RexGlue. A biblioteca JNI do reNut define seu próprio `JNI_OnLoad`. O teste no aparelho detectou e permitiu corrigir a ausência dessas classes e a diferença de aliases `/data/user/0` versus `/data/data`. A ponte content URI está compilada, mas o teste de XEX exercitou arquivos privados, não essa ponte.

O gerador 0.10.0 emite um desvio não resolvido em `0x82C79F68` para um rótulo existente na mesma função (`0x82C79EE4`). `fix-android-codegen.ps1` verifica a função e o rótulo antes de substituir somente esse erro por um salto interno. Falha se encontrar outros desvios não resolvidos. O aviso de tamanho da função `0x82D37708` significa que a unidade gerada excede o tamanho de divisão configurado; não é erro de compilação.

O compilador produz AArch64 diretamente no Windows; Termux não é necessário para este fluxo. Não existe ainda um fluxo de build Termux validado. O pool de compilação limita quatro tarefas simultâneas; a primeira compilação recompila centenas de unidades C/C++.

O botão de validação somente CPU continua usando o modo de análise do SDK. O novo teste com Plume injeta a implementação gráfica no runtime e desativa esse modo; isso não comprova áudio, entrada nem o boot do jogo. Não há medição de FPS do Banjo nem promessa de utilização de 100% da GPU.

## Ponte RexGlue → Plume (0.3)

`android/rexglue/plume_graphics.cpp` implementa `IGraphicsSystem`. O patch do SDK acrescenta o ponto opcional `PresentFrontBuffer` e o chama dentro do kernel `VdSwap`, depois da conversão do descritor de textura. Quando o backend consome o quadro, o buffer de comandos recebe NOPs; os demais backends preservam o caminho PM4 anterior.

O teste percorre memória física do runtime → ABI real de `VdSwap` → backend Plume → Vulkan → superfície Android. A apresentação aceita RGBA8 linear ou tiled 2D, conversões de endian e swizzle, com validação de formato, dimensões, pitch, limites e acesso à memória. O ajuste de proporção e a conversão de pixels são feitos na CPU, com barras pretas. Trata-se de uma ponte inicial de framebuffer, não de uma implementação de processamento gráfico Xenos.

Uploads, cópia para o swapchain e apresentação usam Plume. O primeiro quadro é copiado de volta da GPU e comparado integralmente com a imagem convertida. Somente essa leitura diagnóstica usa `vkCmdCopyImageToBuffer` diretamente, pois a versão fixada do Plume não implementa a direção imagem → buffer em sua abstração de cópia. Flush/invalidate cobrem memória não coerente; a espera de aquisição inclui comandos de transferência, e a conclusão da submissão tem fence com limite de dois segundos.

No SM-S918B / Adreno 740, o teste apresentou 180 quadros com o driver do sistema e conferiu um readback completo da GPU. A imagem foi inspecionada e também confirmada pelo usuário. O caminho exercitado usa framebuffer linear sem troca de endian; oito vetores estáticos verificam endereços tiled, mas ainda não houve validação tiled com uma captura real do jogo. O encaminhamento de driver alternativo está implementado e ainda não foi testado com um pacote de driver.

Evidências locais: `out/renut-plume-live.png`, `out/renut-plume-device-report.txt`, `out/renut-plume-device.log` e `out/plume-final-checks.log`. APK local: `out/renut-0.3.0-plume-bridge-debug.apk`. `assembleDebug` e `lintDebug` concluíram sem erros.

O APK final foi instalado e repetiu os 180 quadros e a conferência da GPU. Também rejeitou descritores com formato inválido, pitch zero e endereço físico fora dos limites. SHA-256 do APK: `a489e061a07c3d29f4269f4d20f6631f2653bef2963c300a4bc12b0285f177b3`.

Ainda falta revisar os hooks e o boot, importar os assets completos e implementar a tradução dos comandos Xenos, shaders e operações de renderização/EDRAM. As operações de ring buffer, writeback e interrupções gráficas não implementadas falham explicitamente. O teste chama `VdSwap` com dados controlados; não executa o ponto de entrada do jogo nem mede seu desempenho.

## Execução Xenos/Vulkan (0.4)

Esta etapa sucede a ponte de apresentação 0.3 descrita acima. O aplicativo agora compila e injeta `librexgpu-xenos.so`, reutilizando o processador PM4, tradução Xenos → SPIR-V, caches de shaders/pipelines/texturas, render targets, EDRAM e apresentação Vulkan do SDK. **A renderização real usa o backend Vulkan do RexGlue; não passa pelo Plume.** O Plume permanece como teste independente e ponte de framebuffer controlado. Isso evita tratar uma abstração de renderização como se ela já traduzisse a GPU do Xbox 360.

O boot real passou pelo logotipo Rare e exibiu a tela das peças douradas. Os logs registraram shaders do jogo traduzidos, módulos Vulkan criados e pelo menos 3.300 chamadas de swap em 1280×720. Esse contador não mede FPS efetivamente apresentados. A imagem permaneceu nas peças douradas durante a observação; menu, cenas jogáveis e controles ainda precisam de validação. Foram corrigidas uma combinação de sampler base-map com anisotropia e uma corrida de encerramento de cliente de áudio que derrubavam as primeiras execuções.

Os 1.981 assets já extraídos pelo usuário foram copiados por ADB para a importação privada ativa; o SHA-256 do XEX foi conferido no aparelho. Isso não substitui a validação pendente da extração integral pela UI. O carregador Xenos aceita o driver selecionado via adrenotools; somente o driver do sistema foi testado.

O último build, com `-O2` no backend gráfico, passou em `assembleDebug` e `lintDebug` (`out/xenos-final-build.log`), mas **ainda não foi instalado/testado**, pois o usuário pediu para interromper e salvar o trabalho. O aparelho ficou com o build imediatamente anterior, que contém as correções de sampler/áudio, controles básicos e carregador adrenotools. Veja o documento de retomada para os próximos testes e evidências.
