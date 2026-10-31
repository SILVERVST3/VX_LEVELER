# VX_Leveler SILVER

Nivelador de voz em **VST3 com ARA 2**, inspirado no módulo **Dynamics** do Noiseworks VoiceAssist.
Visual prateado com textos em azul-marinho (mesma família do RefPro SILVER / SILVER-MB).

## Como funciona

- **Modo ARA (REAPER, Studio One, Cubase, Logic, etc.):** o plugin lê o take inteiro e analisa
  a loudness (K-weighting BS.1770, quadros de 10 ms). Com o take todo na mão, ele nivela olhando
  **antes e depois** de cada frase, sem a "pressa" de um vocal rider em tempo real.
- **Sem ARA:** cai automaticamente num rider em tempo real com lookahead de 300 ms (latência reportada ao host).

## Controles (seção DYNAMICS)

| Controle | Faixa | Padrão | O que faz |
|---|---|---|---|
| Target | LUFS-I Match / LUFS-I Custom / LUFS-S Match / LUFS-S Custom | LUFS-I Match | Match mantém a loudness original (integrada ou pico short-term); Custom usa o valor do knob Target |
| Target (knob) | -32 a -10 LUFS | -23 | Alvo nos modos Custom |
| Speed | 100 ms a 5 s | 750 ms | Janela de reação. Fala: mais rápido. Canto: mais lento |
| Smoothing | 0 a 100 % | 50 % | Suaviza a curva de ganho |
| Noisefloor | Auto / Custom (-80 a -20 dB) | Auto | Abaixo dele (pausas, respiros, ruído de sala) nada é levantado |
| Catch Peaks | 0 a 100 % | 50 % | Redução extra em picos rápidos |
| Range | ±3 a ±24 dB | ±18 dB | Limite do ganho automático (extra, não existe no VoiceAssist) |
| Output | ±12 dB | 0 dB | Ganho de saída |

## Janela de edição (ARA)

- Cinza claro = forma de onda original; azul-acinzentado = processada; linha azul-marinho = curva de ganho;
  tracejado = curva automática; laranja = suas edições manuais.
- **Arraste para cima/baixo** sobre a forma de onda para subir/descer o ganho naquele trecho (Shift = ajuste fino).
- **Shift + roda**: largura do pincel (faixa tracejada). **Ctrl + roda**: zoom. **Roda**: rolar. **Duplo clique**: enquadrar.
- **Undo** / **Clear Edits**. As edições ficam salvas dentro do projeto do host (arquivo ARA).
- **Export WAV**: grava a voz processada (24 bits) de cada clipe.
- **Export MIDI**: automação de volume em CC7 (canal 1, 120 BPM): -15 dB = 0, 0 dB = 63, +15 dB = 127.

## REAPER

Insira o **VX_Leveler SILVER** como FX do item (take FX) ou da trilha. O REAPER reconhece o ARA automaticamente.
A análise começa sozinha; o status aparece no topo da janela.

## Compilação (GitHub Actions)

1. Suba esta pasta para um repositório no GitHub.
2. O workflow `.github/workflows/build.yml` baixa o JUCE 8.0.4 e o ARA SDK 2.2.0 e compila no Windows.
3. Baixe o artefato `VX_Leveler_SILVER_VST3_Windows` e copie a pasta `VX_Leveler SILVER.vst3` para
   `C:\Program Files\Common Files\VST3\`.

Compilação local: clone `JUCE` e `ARA_SDK` (com `--recursive`) dentro desta pasta, ou deixe o CMake baixar sozinho.

## Arquivos

- `Source/LevelerDSP.*` – análise e cálculo da curva (C++ puro), rider em tempo real
- `Source/VXDocumentController.*` – ARA: análise por fonte, edições por clipe, renderer, salvar/restaurar
- `Source/PluginProcessor.*` – parâmetros, cache das curvas, modo sem ARA
- `Source/PluginEditor.*`, `Source/SilverLookAndFeel.*` – interface
- `Tests/LevelerTest.cpp` – teste offline do algoritmo (`-DVX_BUILD_TESTS=ON`)
