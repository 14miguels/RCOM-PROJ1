# Relatório de implementação — RCOM Lab 1

Branch de desenvolvimento: `Miguel` (ponto de partida `40015d5`). A branch `ricardo` não foi alterada.
Não foi feito push, reset --hard, clean nem force push. Na revisão final foram feitos o commit e o merge
local na `main` autorizados pelo utilizador (ver §12).

Ficheiros fornecidos e **não alterados**: `Makefile`, `src/main.c`, `src/*.h`, `src/serial_port.c`,
`cable/cable.c`.

Ambientes de teste: **macOS (arm64, Apple clang, socat)** e **Linux (Ubuntu 24.04, gcc 13.3, kernel 6.10
linuxkit aarch64) num container Docker Desktop**. Hardware RS-232 real: não disponível (ver §9).

---

## 1. Estado inicial da branch `Miguel`

| Parte | Estado encontrado |
|---|---|
| `llOpenTx`, `llOpenRx` | Funcionais, cada uma com a sua máquina de estados e `alarm()`; `llOpenRx` sem proteção para SET repetido. |
| `llSend` | Stop-and-Wait implementado com `alarm()`; fechava a porta em cada erro; sem estatísticas. |
| `llReceive` | Máquina de estados com RR/REJ/duplicados, **sem timeout** (bloqueava para sempre se o Tx morresse), sem tratar SET repetido nem DISC. **Bug de memória**: `raw_data[2050]` era destuffed para `destuffed[1050]` (overflow de pilha com tramas longas) e `memcpy` para `packet` sem limite. |
| `llCloseTx`, `llCloseRx` | `TODO` (devolviam 0 sem fazer nada). |
| `application_layer.c` | Só abria o link (`llOpenTx/Rx`); sem ficheiro, START/DATA/END, nem fecho. |
| Estatísticas | Não existiam. |
| 5 máquinas de estados quase idênticas copiadas em `llOpenTx`, `llOpenRx`, `llSend`, `llReceive`, (e outra na `ricardo`). | — |

## 2. Funcionalidades encontradas na `ricardo`

`origin/ricardo` (`c024c9c`) está **1 commit à frente** da `Miguel`: `llCloseRx` implementada
(espera DISC; envia DISC com `A=0x01`; espera UA com `A=0x01`; retransmite com `alarm`), mais as variáveis
`rxTimeout`/`rxMaxRet`. Os commits anteriores da `ricardo` (`b034ff1` llReceive, `e1a7e9b` llOpenRx)
já estavam incorporados na `Miguel` pelo merge `30ac0ee`.

## 3. Código aproveitado de `ricardo`

Aproveitei **a lógica** (não o texto), porque a integração literal manteria a 5ª cópia da máquina de estados:

* sequência do fecho do Rx (DISC recebido → DISC `A=0x01` → UA `A=0x01`, com retransmissão do DISC),
* endereços corretos no fecho,
* regras de `llReceive` (REJ/RR/duplicado) — reescritas em `rxProcess`.

O commit `c024c9c` da `ricardo` **não é antecessor** da `Miguel` nem da `main` (a lógica foi portada à mão), e esta
integração **não altera a branch `ricardo`**. Um `git merge origin/ricardo` daria conflito em todo o `link_layer.c`
(reestruturado); decidir isso fica para o utilizador e a equipa.

## 4. Código novo

* **`src/link_layer.c`** (reescrito): uma só máquina de estados `receiveFrame` para todas as tramas;
  `buildInfoFrame`/`stuffByte`; `llSend`/`llReceive` Stop-and-Wait; `llCloseTx`/`llCloseRx`; timeouts por
  ticker de 100 ms + relógio monotónico; escrita completa (`writeAll`); estatísticas e `S = R/C`.
* **`src/application_layer.c`**: `runTx`, `runRx`, pacotes START/DATA/END (TLV tamanho 1–8 bytes + nome),
  validação de comprimentos, escrita em `.part` + `rename`, validação de metadados e de tamanho.
* **`tests/`**: `test_unit.c` + `serial_mock.c` (testes unitários com porta simulada), `relay.c`
  (cabo virtual com injeção determinística de falhas), `driver.c` (como `main.c` mas com tentativas/timeout
  configuráveis), `run_integration.sh`, `run_stress.sh` (stress aleatório com invariante de integridade), `m1_strings.c` (Milestone 1), `efficiency.sh`, `Makefile`.
* **Documentação**: `RCOM_STUDY_GUIDE.md`, este relatório.

## 5. Problemas corrigidos

1. Overflow de pilha em `llReceive` (buffers de 2050 → 1050) — agora o campo de dados está limitado a `MAX_DATA_FIELD` e o excesso marca a trama como má (testado com tramas de 3000 bytes).
2. `llReceive` sem timeout → agora desiste após `(nRetransmissions+1)×timeout` de silêncio.
3. SET repetido (UA perdido) não era respondido → `rxProcess` responde UA sem mexer no estado.
4. Race do `alarm()` com `read()` bloqueante (sinal antes do `read` ⇒ bloqueio até ao próximo byte) → ticker periódico + deadline monotónico.
5. Timeout "reiniciado" por bytes irrelevantes → deadline absoluto.
6. Retransmissão `REJ` e timeout partilham agora o mesmo limite (`1 + nRetransmissions`).
7. Escritas parciais / `EINTR` em `write` → `writeAll`.
8. Fecho: DISC duplicado, RR do END perdido, UA final perdido (limitação documentada).
9. Erros fatais fecham a porta de forma controlada; `llClose*` posterior só imprime estatísticas.
10. Ficheiro final só aparece após validar START/END + tamanho (`.part` + `rename`).
11. Nome da porta com mais de 49 caracteres é rejeitado (`LinkLayer.serialPort` tem 50), sem overflow.

## 6. Auditoria dos PDFs oficiais

Lidos por inteiro (texto extraído + diagramas inspecionados): *Data-Link Protocol Guide 2026-2027* (32 slides),
*First Lab Class Guide* (4 p.), *Second Lab Class Guide* (4 p., incl. diagrama da máquina de estados SET) e
*rcom2025-Lab1Presentation-additionalDetails* (slides 1–22 com texto). **O PDF "Serial Port Implementation Details"
não existe em `docs/` nem em nenhum lugar do disco**; o conteúdo equivalente (API da porta série, VMIN/VTIME) está
no slide 25 do Data-Link Guide, no Second Lab Class Guide e em `serial_port.h/.c`.

| Requisito oficial (fonte) | Implementação | Resultado |
|---|---|---|
| Valores FLAG/A/C, BCC1 = A^C (DLG 9–11, 14) | `link_layer.c` constantes; `A_TX=0x03`, `A_RX=0x01`; DISC do Rx e UA final do Tx com `0x01`; UA/RR/REJ do Rx com `0x03` | conforme |
| I: `C = 0x00/0x80`, BCC2 = XOR dos dados originais (DLG 11, 14) | `buildInfoFrame`, `computeBcc2` | conforme |
| Várias FLAGs no início; trama com cabeçalho errado ignorada sem ação (DLG 14, 15; rcom2025 20, 22) | `receiveFrame` (`ST_FLAG` aceita FLAGs repetidas; BCC1/A/C inválidos → ressincroniza, sem resposta) | conforme |
| Máquina de estados do Second Guide (Start, FLAG RCV, A RCV, C RCV, BCC OK, STOP; FLAG em qualquer estado volta a FLAG RCV; Other → Start) | `ST_START … ST_BCC_OK` + `ST_DATA` para tramas I (rcom2025 21) | conforme |
| Stuffing `7D 5E`/`7D 5D`, BCC sobre bytes originais, BCC2 verificado após destuffing (DLG 17; rcom2025 17) | `stuffByte`, destuffing em `receiveFrame` | conforme |
| SET/UA, DISC/DISC/UA (DLG 18, 22, 24) | `llOpenTx/Rx`, `llCloseTx/Rx` | conforme |
| Novo: RR; duplicado: descarta + RR; BCC2 errado novo: REJ; duplicado com BCC2 errado: RR (DLG 15; rcom2025 22) | `rxProcess` | conforme |
| Timer em I, SET e DISC; nº máximo de retransmissões configurável (DLG 15, 19; Second Guide) | `timeout` e `nRetransmissions` do `LinkLayer`; 1 envio + N retransmissões | conforme (interpretação documentada) |
| Timer desligado após ACK válido (DLG 19; Second Guide "alarm deve ser desligado") | deadline por envio; `stopTicker` ao fechar a ligação | conforme |
| API e assinaturas, retornos (0/negativo; nº de bytes) (DLG 21–24) | `link_layer.h` intacto; `llSend` devolve `bufSize`, `llReceive` o tamanho do payload, erros `-1` | conforme |
| Pacotes: DATA `C=2, L2 L1, P1..PK`, `K=256·L2+L1`; START/END `C=1/3` com TLV `T=0` tamanho, `T=1` nome; END repete START (DLG 27–28; rcom2025 6) | `buildControlPacket/parseControlPacket/buildDataPacket/parseDataPacket`, `sameInfo` | conforme |
| Independência de camadas (DLG 29) | link layer não interpreta pacotes; aplicação não conhece tramas | conforme |
| Estatísticas do protocolo (DLG 30) | `printStats` em `llCloseTx/Rx` | conforme |
| Eficiência S = R/C variando FER, T_prop, C, tamanho; repetir (DLG 31) | `tests/efficiency.sh` (BER, prop, tamanho); C não variado no CSV; repetições ainda por fazer | parcial (ver §9) |
| Milestones M1–M5 (DLG 3) | ver §11 | conforme |
| `main.c`, `serial_port.*`, headers e `cable.c` não alterados (First Guide) | `git diff` não os mostra | conforme |
| Sugestão rcom2025 14: duplicado "ignore or send RR" | seguimos DLG 15 (RR obrigatório) | conforme ao DLG |
| rcom2025 18: `read` não bloqueante | `serial_port.c` bloqueia (VMIN=1, VTIME=0); tratamos EINTR e `read()==0` | conforme (código fornecido) |

**Divergências reais encontradas: nenhuma.** O único ajuste feito nesta revisão foi em código de teste:
`tests/relay.c` precisava de `_GNU_SOURCE` para compilar no glibc (descoberto ao correr em Linux).

## 7. Testes realizados e resultados reais

| Suite | Como correr | O que cobre |
|---|---|---|
| Unitários (ASan+UBSan) | `make -C tests unit` | stuffing, BCC1/BCC2, construção e parsing de tramas, máquina de estados (lixo, FLAGs repetidas, FLAG partilhada, BCC1 errado, escape inválido/incompleto, trama enorme), fuzz de 3000 round-trips, `llOpen*`, `llSend` (módulo 2, REJ, RR inválido, timeout, escritas parciais), `llReceive` (ordem, duplicados, BCC2, BCC1, SET repetido, DISC, silêncio), `llClose*`, pacotes START/END/DATA |
| Integração (2 processos + SHA-256, ASan+UBSan) | `make -C tests integration` | 12 tipos de ficheiro; ~20 cenários de falha injetada; falhas permanentes; kill de Tx/Rx; erros de sistema; Milestone 1 |
| Stress aleatório | `./tests/run_stress.sh [runs] [drop%] [corrupt%] [dup%]` | perda, corrupção de qualquer byte (FLAGs/A/C incluídos) e duplicação aleatórias, com semente; invariante: concluído ⇒ SHA-256 igual, senão `FAILED` sem ficheiro |
| Cabo oficial | `make run_cable` + `run_rx` + `run_tx` | transferência normal, BER, cabo off/on, outros baud rates |
| Eficiência | `./tests/efficiency.sh` | S vs. tamanho de trama, BER, atraso de propagação |

### 7.1 macOS (arm64, Apple clang) — executado

* Unitários: **26 testes, 225 checks, 0 falhas** (ASan+UBSan, `-Wall -Wextra -Wpedantic`).
* Integração: **49 passed, 0 failed**.
* Stress: **40/40 transferências idênticas** (perda 12 %, corrupção 12 %, duplicação 8 %, 8 retransmissões, timeout 1 s) e antes 20/20 com 5/5/3 %; **0 violações do invariante**.
* Cabo oficial (9600 baud, `penguin.gif`): normal 14 tramas, 0 retransmissões, 11.75 s, S=0.783; `ber 0.00002`: 3 REJ, idêntico; cabo off ~6 s: 2 timeouts, 1 duplicado, idêntico; `ber 0.0005` falhou de forma controlada (sem ficheiro final).

### 7.2 Linux (Ubuntu 24.04 aarch64, gcc 13.3, kernel 6.10 linuxkit, Docker Desktop) — executado

* `make` do projeto (flags do Makefile, `-Wall`): **sem warnings**; `make -B cable` compila sem erros.
* Unitários: **225 checks, 0 falhas** (ASan+UBSan+LeakSanitizer; confirmado que o LSan deteta um leak de teste neste container, logo "sem leaks" é evidência real).
* Integração: **48 passed, 0 failed** (um caso a menos do que no macOS: o teste de ficheiro sem permissões é saltado quando se corre como root).
* Stress: **40/40 idênticas** (perda 10 %, corrupção 10 %, duplicação 5 %), 0 violações.
* Cabo oficial + `bin/main` do Makefile: normal idêntico (SHA-256 igual, 14 tramas, S=0.782); `ber 0.00002` idêntico (2 REJ); cabo off 6 s idêntico (2 timeouts, 1 duplicado); **19200 baud** idêntico (5.89 s, S=0.782); **57600** idêntico (1.97 s, S=0.778); **115200** idêntico (1.01 s, S=0.761); Tx lançado 6 s antes do Rx: idêntico (2 timeouts de SET); Tx lançado 20 s antes do Rx: `could not establish the connection` + `FAILED`, limpo.

### 7.3 Eficiência (cabo oficial, macOS, 9600 baud, 1 execução por célula)

| Bytes de ficheiro/trama | BER 0, prop 0 | BER 0, prop 50 ms | BER 1e-5, prop 0 | BER 1e-5, prop 50 ms |
|---|---|---|---|---|
| 100 | S=0.716 (13.2 s) | 0.384 (24.6 s) | 0.535 (17.7 s, 4 REJ) | 0.384 (24.6 s, 0 REJ) |
| 250 | 0.760 | 0.545 | 0.743 (1 REJ) | 0.545 (0 REJ) |
| 500 | 0.775 | 0.636 | 0.742 (1 REJ) | 0.585 (2 REJ) |
| 997 | 0.783 | 0.689 | 0.783 (0 REJ) | 0.587 (2 REJ) |

(`tests/results/efficiency.csv`.) Máximo teórico 0.8 (8N1). S cresce com a trama e cai com o atraso de propagação. São amostras únicas: as diferenças pequenas entre colunas de BER não são significativas.

## 8. Limitações conhecidas

* **UA final do fecho** não é confirmado (3-way handshake). Se se perder, o Rx reenvia DISC `nRetransmissions` vezes e fecha com aviso; devolve 0 porque o ficheiro já estava validado.
* `main.c` ignora o resultado de `applicationLayer` (é `void`) e termina sempre com código 0: o sucesso verifica-se por `File ... sent/received` / `FAILED` e pela comparação de ficheiros.
* `llOpenRx` espera por SET **sem limite** (o Rx pode ser lançado primeiro). Se o Tx falhar antes de abrir a ligação (ex.: ficheiro inexistente), o Rx fica à espera até ser interrompido.
* Rx morto com `kill -9` deixa `ficheiro.part` (não se pode limpar); o ficheiro final nunca é criado. SIGINT não restaura o `termios` da porta.
* BCC2 é um XOR: não deteta todos os padrões de erro.
* O ticker de 100 ms interrompe chamadas bloqueantes durante a transferência (incl. `printf` para um pipe lento); só se imprime pouco, e `fread/fwrite` de ficheiros regulares não são interrompidos.
* `make cable`/`make run_cable` não compilam o cabo porque existe a pasta `cable/` (Makefile não alterável): usar `make -B cable`.

## 9. O que NÃO foi verificado

* **Hardware RS-232 real** (`/dev/ttyS0`, `/dev/ttyUSB0`, cabo físico, drivers 8250/USB-serial): não testado. A secção §I do `RCOM_STUDY_GUIDE.md` lista os testes obrigatórios a fazer no laboratório, com comandos.
* **Linux x86 nativo**: o Linux testado é aarch64 num container Docker Desktop (kernel real, glibc, ptys), não os PCs do laboratório.
* Disco cheio *real* (ENOSPC) e falta de memória: o erro de escrita foi provocado com `ulimit -f` (mesmo caminho `fwrite` falha); não há `malloc` no código. Permissão negada na porta série: só testadas porta inexistente e ficheiro de entrada sem permissão.
* Eficiência: amostras únicas por célula; o slide 31 do DLG pede repetir medições; **C (baud rate) não foi variado** nas medições do CSV (apenas confirmada a transferência a 19200/57600/115200). FER foi variada via BER do cabo.
* O PDF "Serial Port Implementation Details" não existe no disco (ver §6).
* Valgrind não foi usado (LeakSanitizer em Linux foi).

## 10. Estado por requisito

| Requisito | Estado | Evidência |
|---|---|---|
| SET/UA | PASS | `test_open`; integração `set_lost`, `ua_lost`, `set_duplicated`; cabo oficial (macOS e Linux) |
| Máquina de estados (BCC1, FLAGs, lixo) | PASS | `test_receive_*`, `garbage_before_frames`, stress |
| Stop-and-Wait (I0/I1, RR/REJ, módulo 2) | PASS | `test_send_*`, `test_receive_in_order`, integração |
| Byte stuffing / transparência | PASS | `test_stuffing`, `test_build_info_frame`, fuzz, `flags_0x7E`, `escapes_0x7D`, `mixed_7E_7D` |
| BCC2 / REJ | PASS | `test_receive_bad_bcc2`, `bcc2_corrupt`, `incomplete_escape`, `invalid_escape` |
| Duplicados nunca entregues | PASS | `test_receive_duplicate_not_delivered`, `rr_lost`, `i_frame_duplicated`, stress |
| Timeouts e retransmissões | PASS | `test_send_timeout_then_ok`, `i_frame_lost`, `rr_delayed_late`, cabo off/on |
| Falha permanente → erro controlado | PASS | `cable_cut_*`, `no_ack_at_all`, `always_corrupt`, kill de Tx/Rx, Tx 20 s antes do Rx |
| DISC / DISC / UA | PASS | `test_close_*`, `disc_lost`, `disc_reply_lost` |
| UA final perdido | PASS (comportamento documentado) | `final_ua_lost` |
| Application (START/DATA/END, TLV) | PASS | `test_app_*`, integração |
| Integridade (SHA-256) | PASS | integração, stress, cabo oficial |
| Ficheiro vazio / 0x00 | PASS | `empty`, `zeros_0x00` |
| Ficheiro incompleto nunca válido | PASS | `.part` + `rename`; `preserve_existing_file`, `disk_full_write_error`, stress |
| Estatísticas | PASS | `printStats`; contadores verificados na integração |
| Eficiência S = R/C | PARCIAL (amostras únicas, ver §9) | `tests/results/efficiency.csv` |
| Compilação limpa | PASS | macOS (apenas `-Wstrict-prototypes` dos headers fornecidos) e Linux gcc 13 (`-Wall`, sem warnings) |
| Linux (container aarch64) | PASS | §7.2 |
| Hardware RS-232 real | **NOT TESTED** | §9, §I do guia |
| Memory leaks | PASS em Linux (LSan); não aplicável em macOS | §7.2 |

## 11. Milestones

| Milestone | Estado | Evidência |
|---|---|---|
| M1 — strings pela porta série | PASS (portas virtuais) | `tests/m1_strings.c`, integração |
| M2 — SET/UA e máquinas de estados | PASS | `llOpenTx/Rx`, `test_open`, `set_lost`, `ua_lost` |
| M3 — Stop-and-Wait | PASS | `llSend/llReceive`, `test_send_*`, `test_receive_*` |
| M4 — Timeouts e retransmissões | PASS | `i_frame_lost`, `rr_lost`, `rr_delayed_late`, cabo off/on, BER, stress |
| M5 — Aplicação e API completa | PASS em macOS e Linux (portas virtuais); hardware real NOT TESTED | integração completa, cabo oficial |

## 12. Integração na `main`

Ver o final desta conversa e `git log`: commit(s) na `Miguel` seguidos de merge normal (`--no-ff`) na `main` local.
Sem push. `ricardo` intacta. Os 3 PDFs em `docs/` e `.DS_Store` não foram incluídos nos commits (ficheiros do
utilizador/pessoais).
