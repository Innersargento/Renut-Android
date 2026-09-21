# Plume — sobreposição de CPU e GPU, 20/09/2026

Terceira etapa do mesmo dia. Ver `DESEMPENHO-2026-09-20.md` e
`DESEMPENHO-SPIN-2026-09-20.md`.

## O que era

`PlumeRenderer::SubmitAndPresent` executava a lista de comandos e esperava a
fence na mesma chamada. Um quadro em voo, então o quadro custava a **soma** da
gravação com o tempo de GPU em vez do maior dos dois. Com o spin já corrigido a
divisão estava quase equilibrada — ~46 ms gravando contra ~52 ms de GPU —, que é
o caso em que sobrepor rende mais.

## O que foi feito

Dois quadros em voo (`kPlumeFramesInFlight`), com um conjunto de recursos por
quadro dos dois lados:

- Renderizador: lista de apresentação, fence e semáforo de acquire por slot.
  `SubmitAndPresent` não espera mais; quem espera é `WaitForSlot`, chamado pelo
  processador quando vai reusar o slot.
- Processador: uma lista de comandos por slot, uma fatia do anel de upload por
  slot, e os recursos aposentados guardados por slot em vez de liberados no fim
  do quadro.

O anel foi de 128 para 256 MB: a cena usa ~66 MB por quadro e já bateu 126, e
meia fatia causaria overflow todo quadro — overflow é um dreno completo da GPU
no meio do quadro, exatamente o que a sobreposição existe para evitar.

## Os dois bugs que isso criou, e o que eram

**Um quadro sim, um não, granulado e escuro.** Alternância estrita. A primeira
tentativa de correção foi uma `VkMemoryBarrier` global no topo de cada lista,
que ordena execução — e não resolveu, porque o problema não era ordenação.
`Transition` só emite barreira quando o layout que ele rastreia **muda**, e os
dois quadros terminavam e começavam com os alvos em `COLOR_WRITE`: nenhuma
barreira de imagem era emitida entre duas submissões que escreviam a mesma
imagem UBWC, e os metadados de compressão ficavam inconsistentes.

`FlushImagesForNextFrame` passa toda imagem que o quadro escreveu para
`SHADER_READ` antes de fechar a lista, garantindo que o primeiro uso no quadro
seguinte transicione de volta. Transição de layout preserva conteúdo. Custo de
~70 barreiras de imagem por quadro, contra ~4 mil desenhos.

**Quadros inteiros pretos, intermitentes.** Amostrar uma imagem UBWC que a GPU
acabou de escrever devolvia dado velho. Resolvido com
`FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1`, posto no ambiente antes do
`dlopen` do Turnip — mesma solução que as builds do Winlator usam nesta GPU.
Virou a opção **One UI fix** no cartão de gráficos, ligada por padrão, sem
efeito no driver do sistema.

## Estado

Capturas de tela com a correção mostraram 16 e 18 FPS, contra 8 antes da
sobreposição e 6,6 antes da correção do spin. **Isso ainda não é uma medição**:
os números saíram de capturas isoladas, sem o protocolo de janelas alternadas, e
parte delas foi colhida enquanto ainda havia quadros pretos, que são baratos de
renderizar e inflam o FPS. Refazer o A/B com a tela limpa e com
`thermal_pwrlevel` registrado junto.

## O que ainda está na mesa

- **Audio Worker a 69% de um core**, acordando a cada ~2 ms. Com o spin
  removido, é a maior carga de CPU do processo — e CPU é potência tirada da GPU,
  que ainda opera sob o teto de `plume-gpu-clock-cap`.
- **Custo por passe de render**, que neste aparelho só sai com timestamp query
  do Vulkan: não há contador de GPU disponível (ver a seção 5 de
  `DESEMPENHO-SPIN-2026-09-20.md`).
