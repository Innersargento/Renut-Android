# Plume — a espera da fence e o teto de clock da GPU, 20/09/2026

Continuação de `DESEMPENHO-2026-09-20.md`, que precisa ser lido com a ressalva da
seção 1: as medições daquele documento foram colhidas sob um teto de clock que
ninguém estava observando.

Aparelho: Samsung SM-S918B, Adreno 740, driver Turnip v26.3.0-R5. Showdown Town,
mesmo trecho, câmera parada, `drawn` entre 1,17 e 1,25 milhões por 300 swaps e
`passes` em 24300 — ou seja, carga de trabalho constante entre as janelas.

## 1. A GPU roda a 295-401 dos 719 MHz, e isso não é térmico do kernel

```
thermal_pwrlevel = 7      →  devfreq/max_freq = 295 MHz
freq_table_mhz   = 719 680 615 550 475 401 348 295 220 124
cooling_device34 (devfreq-3d00000.qcom,kgsl-3d0) = 0
cooling_device35 (gpu)                           = 0
```

Os cooling devices do kernel em `cur_state=0` enquanto o teto está no nível 7
mostram que quem limita é a política de vendor da Samsung, não o thermal do
Linux. Aconteceu com bateria a 54% **carregando** e GPU a 56-61 °C, então a regra
"medir acima de 50% de bateria" do documento anterior não protege a medição.

Consequência: os 12-17 µs por desenho e a conclusão de estar "uma ordem de
grandeza" além do que um Adreno 740 precisa estão inflados por um fator entre
1,8 e 2,4 só de clock.

**Registrar `thermal_pwrlevel` e `clock_mhz` em toda medição**, e descartar a
janela em que o teto se mexeu.

## 2. A espera da fence era um busy-wait que queimava um core prime

simpleperf na Main XThread, 14.863 amostras em 15 s:

```
__imp__gameMain
 └─ 92,53%  __imp__appMainDraw
     └─ 98,45%  sub_8227B218 → sub_82287590 → sub_8228F558
                  ├─ 40,01%  [nela mesma]
                  └─ 43,97%  __imp__sub_8264DCD0   (D3D_CBlocker_Check)
```

Com o glue de chamada são ~88% do core num único laço de polling. O Perfetto
concorda pelo outro lado: *Running* 19,2 s de 20 s, fixada na CPU7, 92 ms de sono
no total. São ~13,5 milhões de chamadas por quadro, ~90 milhões por segundo.

No console cada passe do laço gasta algumas centenas de ciclos em instruções
`db16cyc`, que o recompilador descarta como comentário. Sem elas o laço vira
espera pura.

## 3. A mudança

`plume_d3d.cpp` registra um handler em `D3D_CBlocker_Check` no mesmo padrão de
`Clear`/`ClearF`: **observa e devolve `false`**, então a função original roda
inteira. Nenhum código do guest foi reimplementado e a lógica de fence e de
timeout de hang ficou intacta.

O teste de progresso é o que a própria original faz:

```
device = [blocker + 0];  fence = [[device + 10896]];  cache = [blocker + 8]
fence == cache  →  a GPU não andou  →  dorme
```

Salvaguardas: não dorme se o bit 1 de `[device + 10941]` estiver setado (device
marcado como travado ou pronto, a original retorna 0 na hora); e só dorme depois
de 256 passes sem progresso, que a 90 M polls/s são poucos microssegundos — uma
espera curta termina em velocidade de spin.

Ajustável sem recompilar: `<files>/plume-spin` em microssegundos, `0` restaura o
busy-wait. Padrão do binário: 250 µs, sobre uma espera de ~100 ms. O arquivo é
relido a cada 2 s **e** a cada 65536 chamadas, e como dormindo a taxa cai para
~4 mil/s, uma troca pode levar até ~16 s para pegar.

## 4. O que rendeu

Quatro janelas alternadas de ~200-260 s:

| janela | plume-spin | ms/quadro | Main XThread | thermal_pwrlevel |
| --- | --- | --- | --- | --- |
| A1 | 250 | ~126 | — | 6-7 |
| B1 | 0 | **152** | 98% de um core | 5-7, quase tudo 7 |
| A2 | 250 | 98,5 – 137 | **15%** | 3-7 (chegou a 550 MHz) |
| B2 | 0 | **153** | 98% de um core | **7 em 750/750 amostras** |

**6,6 FPS estáveis para 8,0-10,2 FPS, 17-35% menos tempo de quadro.** O uso de
CPU da Main XThread cai de 98% de um Cortex-X3 para 12-15%.

O mecanismo está no dado: com o spin a GPU **nunca sai do nível mais baixo** —
em B2, 750 de 750 amostras em `thermal_pwrlevel=7`. Com o sleep ela sobe. CPU e
GPU dividem um orçamento de potência, e o core prime estava consumindo o da GPU.

Ressalvas: A1 também rodou com teto baixo, então dormir **melhora a chance** de o
teto subir, não garante. E a variação dentro de A2 (98,5 a 137 ms com trabalho
idêntico) é maior que o efeito em comparações isoladas — o que sustenta a
conclusão é a repetibilidade das duas janelas com spin, 152 e 153 ms, não uma
janela sozinha.

## 5. O que ainda está na mesa

- **Sobrepor CPU e GPU.** `PlumeRenderer::SubmitAndPresent` faz
  `executeCommandLists` e `waitForCommandFence` na mesma chamada, um quadro em
  voo, então o quadro é a soma de ~53 ms de gravação com o tempo de GPU, nunca o
  máximo dos dois.
- **A Audio Worker a 69% de um core**, acordando a cada ~2 ms, é o segundo maior
  consumidor de CPU e não foi investigada.
- **Contadores de GPU por estágio continuam indisponíveis** neste aparelho: o
  Perfetto não habilita os tracepoints do kgsl (build de usuário da Samsung; os
  nomes aparecem na lista de falhas do trace e com a GPU ocupada vêm zero
  eventos), e o Turnip instalado não tem Perfetto compilado. O caminho que resta
  para custo por passe de render é timestamp query do Vulkan dentro do Plume.

## Reprodução

- CPU de uma thread, cuidando do `comm` com espaço:
  `sed 's/.*) //' /proc/<pid>/task/<tid>/stat | awk '{print $12+$13}'` (ticks de 10 ms)
- Teto e clock da GPU: `/sys/class/kgsl/kgsl-3d0/{thermal_pwrlevel,clock_mhz,devfreq/max_freq}`
- Ocupação: `/sys/class/kgsl/kgsl-3d0/gpubusy` (ocupado e total desde a última leitura)
- Perfil: `adb shell run-as org.renut.android /system/bin/simpleperf record -t <tid> -g -f 1000 --duration 15 -o ...`
- Alternar sem recompilar: `echo 0 > files/plume-spin` (busy-wait) ou remover o arquivo (250 µs)
