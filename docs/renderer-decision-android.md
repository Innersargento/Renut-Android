# Renut Android: avaliação inicial de renderização

Data: 2026-09-13. Checkout examinado: `296bc2890c717fc6519db6d80abf21ae8f878965`.
Status da avaliação original: recomendação de arquitetura; sem benchmark executado e sem APK produzido naquela etapa.

Atualização após autorização para iniciar o desenvolvimento: o usuário confirmou que a edição europeia funcionou na versão desktop recente. O XEX fornecido foi identificado e corresponde ao contido na ISO. A primeira base Android com Plume foi compilada, instalada e testada no aparelho; detalhes e limitações em [android/README.md](../android/README.md). O diagnóstico de apresentação não é benchmark do jogo.

Alvo informado pelo usuário: Snapdragon 8 Gen 2, 12 GB de RAM. Modelo exato do aparelho, versão Android, driver e condições de refrigeração ainda não informados. Começar com um perfil experimental de 1280×720 e 30 FPS, preservando efeitos, e avaliar 60 FPS como etapa posterior. Esses valores são metas propostas, não desempenho demonstrado.

## Decisão proposta

Usar Plume como base Vulkan para um protótipo de renderizador específico do Banjo-Kazooie: Nuts & Bolts. Preservar o caminho atual ReXGlue/Xenos como referência de compatibilidade. Só substituir a base por Vulkan direto se medições equivalentes demonstrarem um problema relevante que não seja resolvido com uma adaptação pequena do Plume.

Plume é uma interface de baixo nível para APIs gráficas. A tradução dos comandos, recursos e shaders do jogo continua sendo trabalho do Renut. Portanto, a opção recomendada também envolve um renderizador próprio, construído sobre uma infraestrutura existente. [Descrição oficial do Plume](https://github.com/renderbag/plume).

| Caminho | Trabalho reaproveitado | Trabalho novo | Avaliação inicial |
| --- | --- | --- | --- |
| ReXGlue/Xenos atual | Integração existente do Renut | Portabilidade Android/ARM64 e correções | Referência inicial; medir antes de abandonar |
| Camada do Banjo + Plume/Vulkan | Abstração de recursos e comandos | Integração do jogo, shaders e Android | Melhor candidato para o renderizador especializado |
| Camada do Banjo + Vulkan direto | Bibliotecas auxiliares escolhidas | Também toda a infraestrutura de renderização | Maior custo e manutenção; ganho ainda desconhecido |

Essa classificação é julgamento de engenharia, não resultado de FPS ou estimativa de prazo.

## Evidência local

- `CMakeLists.txt` e `generated/rexglue.cmake` vinculam o projeto a `rex::graphics`, além de sistema, kernel e UI. Não existe integração Plume nesses arquivos.
- `generated/` contém somente `rexglue.cmake` nesta inspeção; a entrada do projeto depende de `generated/renut_init.h`. O executável recompilado ainda precisa ser gerado neste checkout.
- `renut_manifest.toml` declara SDK 0.8.0, enquanto o CMake gerado procura 0.7.4 por padrão. Fixar uma versão compatível antes dos testes.
- Os presets locais cobrem Windows/Linux AMD64, sem configuração Android. Isso não comprova impossibilidade de ARM64; mostra o trabalho de integração pendente.
- O README declara edição americana sem atualização. A ISO informada existe e tem 7.835.492.352 bytes; seu nome indica edição europeia. Seu conteúdo e executável não foram validados. Comparar identidade, versão, hash e mapa de código do `default.xex` antes de reutilizar os hooks de endereço fixo de `config/renut_hooks.toml`. Diferença no nome da edição, isoladamente, não prova incompatibilidade binária.
- `src/renut_engine/FPS.cpp` atribui a `gpuMS` um cronômetro ao redor de `appMainDraw`. Isso mede tempo decorrido na CPU, possivelmente incluindo esperas, e não isola a execução da GPU. `Timer.h` usa relógio de sistema e truncamento para milissegundos. Não usar esse indicador para decidir o backend; verificar também sua inclusão efetiva no build.
- O README sugere desativar efeitos. Uma comparação de fidelidade deve definir explicitamente quais efeitos estão ativos em cada caminho.

## Plume e Android

O backend consultado contém criação de superfície Android e consulta de dimensões de `ANativeWindow`. A inicialização solicita Vulkan 1.2 e usa `volkInitialize()`. Isso oferece um ponto de partida concreto, mas não certifica o jogo nem todos os celulares. A integração com driver customizado precisa controlar o carregamento antes da criação da instância Vulkan. [Código Vulkan](https://raw.githubusercontent.com/renderbag/plume/main/plume_vulkan.cpp).

O projeto avisa que sua API ainda não é estável; fixar um commit e manter adaptações pequenas e documentadas. [README Plume](https://github.com/renderbag/plume).

## O trabalho específico do jogo

Mapear o ponto de interceptação do renderizador e o ciclo de recursos: buffers, formatos de vértices, texturas, estados de desenho, shaders, render targets, profundidade e sincronização. Investigar resolves, layout tiled, endian e eventual uso de memory export conforme evidência do jogo. Não presumir que todas essas funções sejam necessárias em todos os passes.

XenosRecomp é candidato para converter shaders Xbox 360 a HLSL e depois SPIR-V. Seu upstream foi adaptado a Sonic Unleashed e documenta recursos incompletos, incluindo memory export e indexação dinâmica de registradores. Exige validar os shaders reais do Banjo e implementar o que faltar. Plume não resolve essa tradução. [XenosRecomp](https://github.com/hedge-dev/XenosRecomp).

Manter a recompilação CPU e o runtime como frente independente. C++ gerado não garante sozinho compatibilidade Android: testar ABI, memória, threads, arquivos, áudio e dependências. O upstream ReXGlue apresenta builds Linux ARM64, que não equivalem a builds Android. [ReXGlue](https://raw.githubusercontent.com/rexglue/rexglue-sdk/main/README.md).

## Adrenotools e aplicativo

Integrar `libadrenotools` como opção para Adreno compatível, mantendo driver do sistema. A biblioteca documenta Android 9+ ARM64 e carregamento por aplicativo. [Projeto](https://github.com/bylaws/libadrenotools).

Carregar a biblioteca customizada antes de iniciar o backend, obter dela o ponto de entrada Vulkan e adaptar o carregador do Plume para usar essa origem. O header exige `useLegacyPackaging = true`, hooks no diretório de bibliotecas nativas e driver em armazenamento interno privado. Validar GPU/driver realmente selecionados e apresentar erro legível se falhar. Troca de driver deve reiniciar o processo para evitar misturar instâncias e tabelas de funções. [Contrato da API](https://github.com/bylaws/libadrenotools/blob/master/include/adrenotools/driver.h).

A futura UI deve ter biblioteca do jogo, importação com progresso/cancelamento e validação da versão, seleção de driver, controles e diagnóstico exportável. O importador fornece os dados locais esperados pelo jogo; importar uma ISO arbitrária não substitui gerar e validar o código recompilado correspondente.

## Protocolo para medir a decisão

1. Identificar aparelho, SoC/GPU, RAM, Android e drivers; fixar commits, compilador, opções, resolução interna e efeitos. Validar primeiro a edição do executável.
2. Estabelecer referência visual do jogo em uma plataforma funcional. Portar e medir o caminho atual em Android quando viável; resultados desktop servem para compatibilidade, não para concluir desempenho mobile.
3. Fazer um protótipo Android Plume com apresentação, textura, profundidade e seleção real de driver. Esse teste valida a infraestrutura, não a velocidade do Banjo.
4. Capturar ou reproduzir cargas representativas do jogo: menu, área externa, veículo complexo e efeitos/transparências, usando câmera, estado e comandos reproduzíveis.
5. Comparar a mesma carga especializada sobre Plume e uma implementação Vulkan direta mínima somente quando necessária para isolar custo da abstração. Usar os mesmos shaders, recursos e trabalho visual. Comparar com Xenos separadamente, pois mudanças na tradução podem explicar o ganho.
6. Fazer pelo menos três execuções por configuração, alternando a ordem e controlando temperatura inicial. Separar cache frio de cache aquecido. Medir também uma sessão sustentada de 20 minutos.
7. Registrar tempo de quadro p50/p95/p99, tempo CPU de preparação/submissão, timestamps GPU quando suportados, frames apresentados, memória, compilação de pipelines, travamentos e estado térmico. Marcar métricas indisponíveis; não substituir tempo GPU por cronômetro CPU. Usar relógio monotônico de alta resolução para CPU.
8. Validar imagens e efeitos antes de comparar velocidade. Fazer validação gráfica em execução separada da medição de desempenho. Não obter ganho simplesmente removendo sombras ou mudando resolução.

Critério proposto: aprovar Plume se executar corretamente as cargas e cumprir o orçamento de quadro definido para o aparelho. Investigar Vulkan direto se houver limitação de recurso ou overhead reproduzível atribuível ao Plume. Exigir ganho maior que a variação entre execuções, sem regressão visual/térmica, antes de assumir a manutenção adicional. Nenhum percentual de ganho foi medido.

Metas possíveis: 33,33 ms por quadro para 30 FPS; 16,67 ms para 60 FPS, condicionado também à lógica e física do jogo. O alvo inicial deve privilegiar fidelidade e estabilidade sustentada. “100% da potência gráfica” não é garantia técnica: uso total da GPU pode significar saturação. Avaliar trabalho útil, fluidez, consumo e limites térmicos.

## Limites desta etapa

Foi feita inspeção de arquivos e consulta a fontes primárias, sem executar o jogo, extrair a ISO ou medir um aparelho. A enumeração do WSL retornou acesso negado nesta sessão; a avaliação documental não dependeu dele. SoC e RAM foram informados; modelo exato do aparelho permanece pendente. As fontes upstream consultadas usam branches móveis; registrar commits exatos ao iniciar o protótipo.
