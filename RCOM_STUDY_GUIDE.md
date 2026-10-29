# RCOM Lab 1 — Guia de estudo e defesa

Este guia descreve o código **tal como está nesta branch** (`Miguel`). Todos os nomes de
funções, constantes e comandos existem no projeto. Ordem recomendada de estudo no fim (§J); testes a fazer no laboratório na §I.

Ficheiros do projeto:

| Ficheiro | Papel |
|---|---|
| `src/serial_port.c/.h` | **Fornecido, não alterar.** `open/close/readByte/writeBytes` sobre `termios`. |
| `src/link_layer.c/.h` | **Camada de ligação** (header fornecido, `.c` implementado por nós). |
| `src/application_layer.c/.h` | **Camada de aplicação**: ficheiro ⇄ pacotes START/DATA/END. |
| `src/main.c` | Fornecido: lê argumentos e chama `applicationLayer` com 3 tentativas e 4 s. |
| `cable/cable.c` | Cabo virtual oficial (socat): `off/on`, `ber`, `prop`, `baud`. |
| `tests/` | Testes (não fazem parte da entrega do protocolo): unitários, integração, relay com injeção de falhas, eficiência. |

---

## A. Arquitetura

```
 applicationLayer()            ficheiro ⇄ pacotes (START / DATA / END) — NÃO conhece tramas
        │  llOpenTx/llSend/llCloseTx        llOpenRx/llReceive/llCloseRx
 link_layer.c                  pacotes ⇄ tramas: enquadramento, BCC1/BCC2, stuffing,
        │                      Stop-and-Wait, timeouts, retransmissões, duplicados
        │  openSerialPort / readByteSerialPort / writeBytesSerialPort / closeSerialPort
 serial_port.c                 termios, read()/write() sobre /dev/ttyS* ou /tmp/ttyS10|11
```

Separação de responsabilidades (é avaliada):

* A **link layer** trata cada pacote como bytes opacos. Nunca olha para `C = 1/2/3` do pacote.
* A **aplicação** não sabe o que são FLAG, BCC, stuffing, N(s), timeouts. Só chama `llSend`/`llReceive`.

### Organização interna de `link_layer.c` (de cima para baixo)

1. Constantes (`FLAG`, `A_TX`, `C_SET`, …), estado global e `Stats`.
2. **Tempo**: `startTicker`, `stopTicker`, `nowMs`.
3. **Escrever tramas**: `writeAll`, `sendSupervision`, `stuffByte`, `computeBcc2`, `buildInfoFrame`.
4. **Ler tramas**: `receiveFrame` (a **única** máquina de estados), `waitSupervision`.
5. `printStats`, `linkStart`, `linkStop`.
6. API: `llOpenTx/Rx`, `llSend`, `rxProcess` + `llReceive`, `llCloseTx/Rx`.

A decisão de ter **uma só** máquina de estados de receção (em vez de uma por função) é
deliberada: SET/UA/DISC/RR/REJ e tramas I partilham exatamente o mesmo enquadramento, só muda o
que se faz com a trama depois de lida.

---

## B. Fluxo completo

```
Application TX                                           Application RX
   │ runTx(): abre ficheiro, tamanho, nome                  │ runRx(): prepara "<ficheiro>.part"
   ▼                                                        ▼
llOpenTx()  ── SET (A=03,C=03) ───────────────────────►  llOpenRx()   (espera SET sem limite)
            ◄────────────── UA (A=03,C=07) ─────────────
   │  (timeout → repete SET, até 1+nRetransmissions)        │
   ▼                                                        ▼
llSend(START)   ── I0 ───────────────────────────────►  llReceive() → START (tamanho + nome)
                ◄─────────────────────── RR1 ──────────
llSend(DATA 1)  ── I1 ───────────────────────────────►  llReceive() → fwrite no .part
                ◄─────────────────────── RR0 ──────────
   … I0/I1 alternados, 997 bytes de ficheiro por pacote …
llSend(END)     ── I? ───────────────────────────────►  llReceive() → END: compara metadados,
                ◄─────────────────────── RR? ──────────    confere tamanho, rename(.part → final)
   ▼                                                        ▼
llCloseTx()     ── DISC (A=03) ──────────────────────►  llCloseRx()
                ◄────────────────── DISC (A=01) ───────
                ── UA (A=01) ────────────────────────►     fecha porta
   fecha porta + estatísticas                              estatísticas
```

Endereços (A): `0x03` = comandos do Tx e respostas do Rx; `0x01` = comandos do Rx e respostas do Tx.
Por isso o DISC do Rx e o UA final do Tx levam `A = 0x01`; tudo o resto leva `0x03`.

---

## C. Explicação das funções

### C.1 Camada de ligação (`src/link_layer.c`)

| Função | O que faz | Devolve / estado alterado | Erros |
|---|---|---|---|
| `llOpenTx(p)` | Abre a porta, envia SET e espera UA válido; se expira o timeout, reenvia SET. | `0` ok, `-1` erro. Define `isTx`, parâmetros, zera `txSeq`, estatísticas. | Parâmetros inválidos, porta inexistente, `1+nRetransmissions` SET sem UA → fecha a porta. |
| `llOpenRx(p)` | Abre a porta, espera SET (sem limite de tempo — o Rx pode arrancar primeiro) e responde UA. | `0`/`-1`. Zera `rxExpected`. | Porta inexistente, erro de leitura. |
| `llSend(buf, n)` | Constrói a trama I (BCC1, BCC2, stuffing), envia, espera RR/REJ (Stop-and-Wait). | `n` ou `-1`. Só alterna `txSeq` após RR correto. | `n` inválido (não fecha a ligação); sem RR após `1+nRetransmissions` envios → fecha a ligação. |
| `llReceive(pkt)` | Devolve o payload da próxima trama I **nova e válida**. Responde RR/REJ. Duplicados e SET repetidos são tratados aqui. | Tamanho do payload ou `-1`. Alterna `rxExpected` ao aceitar. | Silêncio > `(nRetransmissions+1)*timeout` (Tx morreu) → fecha; recebeu DISC → `-1` com `discSeen = TRUE`. |
| `llCloseTx()` | DISC → espera DISC → UA. Imprime estatísticas, fecha a porta. | `0`/`-1`. | Sem DISC do Rx após todas as tentativas. |
| `llCloseRx()` | Espera DISC (se ainda não o viu), responde DISC, espera UA. Imprime estatísticas, fecha. | `0`/`-1`. | Sem DISC; se só o UA final não chega, avisa e fecha na mesma (ver limitações). |

Funções internas importantes:

| Função | Papel |
|---|---|
| `receiveFrame(f, deadline)` | **Coração da camada.** Lê bytes até ter uma trama (supervisão ou I) com A, C e BCC1 válidos. Destuffing em linha. Devolve `RX_OK`, `RX_TIMEOUT` (deadline absoluto atingido) ou `RX_ERROR`. |
| `waitSupervision(a, c, deadline)` | Chama `receiveFrame` em ciclo até chegar a trama de supervisão pedida, ignorando as restantes. Usada em SET/UA/DISC. |
| `buildInfoFrame(out, ns, data, n)` | `FLAG A C BCC1 stuff(DATA BCC2) FLAG`. BCC2 sobre os bytes **originais**. |
| `stuffByte` | `0x7E → 7D 5E`, `0x7D → 7D 5D`. |
| `sendSupervision(a, c)` | Envia `FLAG A C A^C FLAG` e conta RR/REJ enviados. |
| `rxProcess(f, payload, len)` | Decisão do Rx para uma trama: entregar / REJ / RR de duplicado / UA a SET repetido / DISC. Partilhada por `llReceive` e `llCloseRx`. |
| `writeAll` | Repete `write` até enviar tudo (escritas parciais, EINTR). |
| `startTicker` / `stopTicker` | SIGALRM periódico de 100 ms (ver §E.3). |
| `linkStart` / `linkStop` | Abrir/fechar porta + ticker; garantem que fechar duas vezes é seguro. |
| `printStats` | Tabela de estatísticas (§F). |

### C.2 Camada de aplicação (`src/application_layer.c`)

| Função | Papel |
|---|---|
| `applicationLayer(...)` | Preenche `LinkLayer` (valida o tamanho do nome da porta), chama `runTx`/`runRx`, escreve `FAILED` em stderr se der erro. |
| `runTx(ll, ficheiro)` | `stat`/`fopen` → `llOpenTx` → START → DATA… → END → `llCloseTx`. Ficheiro vazio ⇒ só START e END. |
| `runRx(ll, ficheiro)` | `llOpenRx` → START → escreve `ficheiro.part` → DATA… → END → valida → `rename` → `llCloseRx`. |
| `buildControlPacket` / `parseControlPacket` | START/END: TLV `T=0` tamanho (1–8 bytes, big endian) e `T=1` nome (opcional). O parse rejeita TLV truncado, tamanho em falta/duplicado, `L` inválido. |
| `buildDataPacket` / `parseDataPacket` | `C=2, L2, L1, dados` com `K = 256*L2 + L1`; o parse confere `K == tamanho real − 3`. |
| `sameInfo` | Compara metadados de START e END. |

O nome do ficheiro recebido no START **só serve de verificação**: o Rx escreve no nome dado na linha
de comandos (evita *path traversal*).

---

## D. Máquinas de estados

### D.1 Receção de tramas — `receiveFrame`

```
          FLAG                A_TX/A_RX              C conhecido            A^C certo
START ───────────► FLAG_RCV ──────────────► A_RCV ───────────────► C_RCV ────────────────┐
  ▲                 │ ▲ FLAG (repetida)       │ FLAG                  │ errado → START     │
  │ qualquer outro  │ └──────────┘            └──► FLAG_RCV           │ (conta badBcc1)    │
  └─────────────────┘ (byte inesperado → START)                                            ▼
                                                    supervisão: BCC_OK ── FLAG ──► trama pronta (RX_OK)
                                                    tramas I:   DATA ─ bytes (destuffing) ─ FLAG ─► RX_OK
```

| Estado | Transição |
|---|---|
| `ST_START` | `FLAG` → `ST_FLAG`. Qualquer outro byte é lixo e ignora-se. |
| `ST_FLAG` | `FLAG` fica (várias FLAGs seguidas); `A_TX`/`A_RX` → `ST_A`; outro → `ST_START`. |
| `ST_A` | `FLAG` → `ST_FLAG`; C conhecido (SET, UA, DISC, RR0/1, REJ0/1, I0/1) → `ST_C`; outro → `ST_START`. |
| `ST_C` | `b == A^C` → `ST_BCC_OK` (supervisão) ou `ST_DATA` (I). Senão `badBcc1++` e volta a sincronizar (se `b` for FLAG vai para `ST_FLAG`). |
| `ST_BCC_OK` | `FLAG` → trama completa. Outro byte → `ST_START`. |
| `ST_DATA` | `0x7D` marca escape; o byte seguinte tem de ser `5E`/`5D` (dá `7E`/`7D`), senão trama marcada má. `FLAG` fecha a trama (FLAG logo a seguir a ESC = escape incompleto = má). Se os dados excedem `MAX_DATA_FIELD` a trama fica má e **nada é escrito fora do buffer**. |

Detalhes que podem ser perguntados:

* No fim de cada trama, `resync = TRUE`: a FLAG de fecho pode ser a FLAG de abertura da seguinte,
  por isso a próxima leitura começa em `ST_FLAG`.
* Uma trama I com BCC1 correto mas dados maus (BCC2, escape, tamanho) **é devolvida** com
  `dataOk = FALSE`; quem decide entre REJ e RR é `rxProcess`. Uma trama com BCC1 errado **nunca**
  chega ao chamador (é ignorada, sem resposta).
* `dataOk` = sem erro de escape/tamanho **e** `dataLen ≥ 1` **e** `XOR(dados) == BCC2`.

### D.2 Estados do protocolo

```
Tx:  fechado ─llOpenTx─► ligado (txSeq=0) ─llSend*─► ligado ─llCloseTx─► fechado
Rx:  fechado ─llOpenRx─► ligado (rxExpected=0) ─llReceive*─► ligado ─llCloseRx─► fechado
Erro fatal (sem RR, silêncio, erro de I/O): linkStop() → fechado; chamar llClose* só imprime estatísticas.
```

---

## E. Stop-and-Wait

### E.1 Números de sequência (módulo 2)

| Trama | Controlo C | Significado |
|---|---|---|
| I0 / I1 | `0x00` / `0x80` | Trama com N(s)=0 / 1 (só Tx → Rx) |
| RR0 / RR1 | `0xAA` / `0xAB` | "Recebi bem; **espero** a trama 0 / 1" |
| REJ0 / REJ1 | `0x54` / `0x55` | "Trama 0 / 1 com erro; reenvia" |

`RR(n)` não confirma "a trama n" — confirma tudo até à anterior e **pede a n**. Por isso depois de I0
vem RR1.

### E.2 Regras do Tx (`llSend`)

```
envia I(Ns)  → espera resposta até ao deadline (timeout segundos)
   RR(Ns^1)         → sucesso, txSeq ^= 1                   (só aqui o número de sequência avança)
   REJ(Ns)          → reenvia imediatamente (conta como tentativa)
   timeout          → reenvia
   RR(Ns), REJ(outro), resposta com A errado ou BCC1 errado → IGNORA (mesmo deadline)
total de envios = 1 + nRetransmissions; depois disso → erro
```

`nRetransmissions` = **retransmissões**, não envios totais (com 3 tentativas ⇒ no máximo 4 envios).
A mesma convenção vale para SET e DISC.

### E.3 Regras do Rx (`rxProcess`, chamada por `llReceive`)

| Situação | Ação | Estado |
|---|---|---|
| I(Ns) com `Ns == rxExpected`, BCC2 ok | entrega, envia **RR(rxExpected^1)** | `rxExpected ^= 1` |
| I(Ns) com `Ns == rxExpected`, BCC2 mau | descarta, envia **REJ(rxExpected)** | mantém |
| I(Ns) com `Ns != rxExpected` (duplicado), BCC2 ok ou mau | **não entrega**, envia **RR(rxExpected)** | mantém |
| BCC1 mau | ignora, sem resposta | mantém |
| SET repetido (UA perdido) | responde UA outra vez | mantém |
| DISC | `llReceive` devolve `-1` e `discSeen = TRUE`; `llCloseRx` responde | — |

Por que o duplicado leva RR e não silêncio: o Tx só repete porque não viu o RR; se o Rx ficasse
calado, o Tx esgotava as tentativas.

### E.4 Timeouts (como funciona)

`serial_port.c` bloqueia em `read()` até chegar um byte. Para ter timeout sem *busy waiting*:

1. `startTicker()` instala um `SIGALRM` periódico de **100 ms** (`setitimer`), sem `SA_RESTART`:
   cada tick interrompe o `read()` com `EINTR`.
2. Os ciclos de leitura voltam a verificar `nowMs() >= deadline` (relógio **monotónico**).
3. O deadline é **absoluto** e calculado uma vez quando se envia; bytes irrelevantes que chegam
   **não o adiam** (há um teste unitário para isto).

Vantagem face a um `alarm()` único: se o sinal disparar imediatamente antes do `read()` bloquear
(corrida clássica), o próximo tick, no máximo 100 ms depois, acorda a leitura — não há
bloqueio permanente. `writeAll` também trata `EINTR` e escritas parciais.

O Rx também tem limite: se não recebe **nenhuma** trama válida durante
`(nRetransmissions+1) × timeout` segundos, `llReceive` desiste (um Tx vivo retransmitiria a cada
`timeout`). `llOpenRx` espera sem limite de propósito (o Rx pode ser lançado primeiro).

---

## F. Byte stuffing — exemplo concreto

Payload (pacote da aplicação) = `01 7E 7D 02` (4 bytes), N(s) = 0.

1. `BCC1 = A ^ C = 03 ^ 00 = 03`
2. `BCC2 = 01 ^ 7E ^ 7D ^ 02 = 00` (calculado sobre os bytes **originais**)
3. Dados + BCC2 antes de stuffing: `01 7E 7D 02 00`
4. Stuffing: `7E → 7D 5E`, `7D → 7D 5D` ⇒ `01 7D 5E 7D 5D 02 00`
5. Trama no fio (as FLAGs de fora **não** levam stuffing):

```
7E  03  00  03  01 7D 5E 7D 5D 02  00  7E
FLAG A   C  BCC1 \_____DATA_____/ BCC2 FLAG
```

Se o BCC2 for `7E` ou `7D`, também é escapado (ex.: payload `7E` ⇒ BCC2 `7E` ⇒ `7E 03 00 03 7D 5E 7D 5E 7E`).
No Rx: `7D 5E → 7E`, `7D 5D → 7D`; `7D` seguido de outro byte, ou `7D` mesmo antes da FLAG, é erro.

Estes exemplos são testes unitários (`test_build_info_frame`, `test_receive_info`).

### Estatísticas (impressas em `llCloseTx`/`llCloseRx`)

| Tx | Rx |
|---|---|
| tramas I enviadas (total) e retransmissões | tramas I recebidas (BCC1 ok) |
| timeouts | entregues à aplicação / duplicadas (não entregues) |
| RR e REJ recebidos | com erro de dados (BCC2/escape/tamanho) |
| bytes úteis confirmados | tramas descartadas por BCC1 |
| tempo, **R** (débito útil), **S = R/C** | RR e REJ enviados, bytes úteis, tempo, R, S |

"Bytes úteis" são os bytes de payload da camada de ligação (inclui os cabeçalhos dos pacotes de
aplicação mas **não** FLAG/A/C/BCC/stuffing nem retransmissões). Uma retransmissão conta em "tramas
enviadas" mas **não** em bytes úteis.

---

## G. Perguntas de apresentação (com respostas)

1. **Porque Stop-and-Wait?** É o ARQ mais simples: uma trama em voo, confirmada antes da seguinte. O enunciado pede-o; basta 1 bit de número de sequência e não há janelas nem buffers.
2. **Porque existe BCC1 e BCC2?** BCC1 protege o cabeçalho (A, C): se estiver errado nem se sabe de que tipo de trama se trata, por isso a trama é ignorada sem resposta. BCC2 protege os dados: se falhar, o cabeçalho é de confiança, logo o Rx pode responder REJ.
3. **Porque precisamos de stuffing?** A FLAG `0x7E` delimita tramas; se os dados a contiverem, o Rx cortaria a trama a meio. Escapamos `7E` e `7D`, mantendo a transparência para qualquer ficheiro binário.
4. **O que acontece se perdermos um RR?** O Tx expira, reenvia a mesma trama; o Rx deteta que N(s) ≠ esperado, **não entrega** e responde de novo RR(esperado). O Tx avança. (Teste `rr_lost`.)
5. **Como detetamos duplicados?** Comparando N(s) da trama com `rxExpected`. Se diferem é repetição de uma trama já entregue.
6. **Porque alternamos entre I0 e I1?** Porque com um bit o Rx distingue "trama nova" de "repetição da anterior". Sem isso, um RR perdido faria o ficheiro ter blocos duplicados.
7. **O que acontece se o Rx receber I0 duas vezes?** A segunda é duplicado: não é entregue, responde RR1 (próxima esperada). Estado inalterado. (`test_receive_duplicate_not_delivered`.)
8. **O que acontece quando o BCC2 está errado?** Trama nova ⇒ descarta e envia REJ(esperado) ⇒ Tx reenvia imediatamente. Trama duplicada com BCC2 errado ⇒ RR(esperado) (não vale a pena pedir de novo algo que já tínhamos).
9. **Porque a aplicação não trata retransmissões?** Separação de camadas: a ligação oferece um canal fiável; a aplicação só vê pacotes ordenados, sem duplicados.
10. **O que acontece se o transmissor morrer?** O Rx não recebe nada durante `(nRetransmissions+1)×timeout`, `llReceive` fecha a porta e devolve `-1`; a aplicação apaga o `.part` e **não** cria o ficheiro final. (Teste `tx_killed_mid_transfer`.)
11. **E se o recetor morrer?** O Tx não obtém RR, esgota `1+nRetransmissions` envios e falha. (`rx_killed_mid_transfer`.)
12. **Como funciona o timeout?** Tick periódico de 100 ms interrompe o `read` bloqueante; compara-se o relógio monotónico com um deadline absoluto. Não consome CPU nem se estende com lixo (§E.4).
13. **Qual a diferença entre RR e REJ?** RR = "recebi bem, envia a trama N"; REJ = "recebi com erro, reenvia já a trama N" (evita esperar o timeout).
14. **Como garantimos que o ficheiro não fica corrompido?** BCC2 por trama + retransmissão; no fim comparamos metadados START/END e o tamanho; só então fazemos `rename(.part → final)`. Os testes comparam SHA-256.
15. **Quais as limitações do protocolo?** Uma trama em voo (ineficiente com latência alta); BCC2 é um XOR (não deteta todos os erros, p.ex. dois bits trocados na mesma coluna); o último UA do fecho não é confirmado; sem controlo de fluxo.
16. **Como calculamos a eficiência?** `S = R/C`, com `R` = bits úteis / tempo e `C` = baud rate. Impresso nas estatísticas (o §7 do `IMPLEMENTATION_REPORT.md` tem medições reais).
17. **Porque `S` nunca chega a 1?** Cada byte usa 10 bit-times (start + 8 + stop) ⇒ máximo 0.8; além disso cabeçalhos, BCC, RR de 5 bytes e espera entre tramas.
18. **Porque o endereço A muda no fecho?** `A=0x03` para comandos do Tx e respostas do Rx; `A=0x01` para comandos do Rx e respostas do Tx. O DISC do Rx é um comando do Rx; o UA final do Tx é uma resposta do Tx.
19. **E se o UA inicial se perder?** O Tx reenvia SET; o Rx (já em `llReceive`) responde UA outra vez sem mexer no estado (`set_lost`, `ua_lost`, `set_duplicated`).
20. **E se o UA final do fecho se perder?** Não há 4ª mensagem: o Rx reenvia DISC `nRetransmissions` vezes e fecha mesmo assim com um aviso; o ficheiro já estava validado. É uma limitação inerente (problema dos dois exércitos). (`final_ua_lost`.)
21. **E se o RR do END se perder?** O Tx reenvia o END; o Rx já está em `llCloseRx` e continua a responder RR(esperado) a duplicados enquanto espera o DISC. (`end_rr_lost`.)
22. **Porque BCC1 errado não gera resposta?** Não sabemos se A/C estão certos; reagir a uma trama de tipo desconhecido poderia causar respostas erradas. O timeout do Tx recupera.
23. **Como evitamos overflow de buffers?** Dados limitados a `MAX_DATA_FIELD`; excedido ⇒ trama marcada como má e bytes descartados. `llSend` rejeita `bufSize > MAX_PAYLOAD_SIZE`. Testado com ASan/UBSan e tramas de 3000 bytes.
24. **Porque escrever em `.part` e só depois `rename`?** Para nunca apresentar um ficheiro incompleto como válido nem substituir um ficheiro existente por um truncado.
25. **O que é `nRetransmissions`?** Retransmissões depois do primeiro envio ⇒ máximo `1 + nRetransmissions` envios. Vale para SET, I e DISC.
26. **Como se garante que `llReceive` nunca entrega duplicados?** Só devolve quando `Ns == rxExpected` **e** BCC2 ok; ao devolver faz `rxExpected ^= 1`. Qualquer outra trama I nunca chega a copiar dados para `packet`.
27. **Que acontece com ficheiro vazio?** START (tamanho 0) e END; nenhum DATA. `fread` não é chamado. (`empty`.)
28. **Porque o START leva o tamanho?** Permite ao Rx confirmar no END que recebeu tudo, e rejeitar mais bytes do que anunciado.
29. **Um 0x00 nos dados é problema?** Não: tratamos tudo com tamanhos explícitos (`memcpy`, `fread/fwrite`); nunca `strlen/strcpy` em dados.
30. **Porque o ticker é periódico em vez de `alarm()` por tentativa?** Evita a corrida "sinal antes do read bloquear" e dispensa rearmar/desarmar o alarme em cada ramo de saída.

---

## H. Guia de demonstração (comandos reais)

Todos a partir da raiz do projeto. O `Makefile` não pode ser alterado e tem uma particularidade:
`make cable` é ignorado porque já existe a pasta `cable/` — usar `make -B cable` na primeira vez.

### H.1 Compilar

```
make                     # compila bin/main (flags do Makefile: -Wall)
make -B cable            # compila bin/cable (necessário uma vez; requer socat)
```

### H.2 Criar portas virtuais (terminal 1)

```
make run_cable           # cria /tmp/ttyS10 (Tx) e /tmp/ttyS11 (Rx); fica interativo
```
Comandos no terminal do cabo: `off`, `on`, `ber 0.00002`, `prop 50000`, `baud 19200`, `help`.

### H.3 / H.4 Recetor e transmissor (terminais 2 e 3; **primeiro o Rx**)

```
make run_rx              # ./bin/main /tmp/ttyS11 9600 rx penguin-received.gif
make run_tx              # ./bin/main /tmp/ttyS10 9600 tx penguin.gif
```

### H.5 Comparar ficheiros

```
make check_files         # diff -s penguin.gif penguin-received.gif  → "identical"
shasum -a 256 penguin.gif penguin-received.gif      # (Linux: sha256sum)
```

### H.6 Demonstrar retransmissões

*Com o cabo oficial* (durante a transferência, ~12 s a 9600 baud, no terminal do cabo):

```
off          # esperar ~5 s (< 3 tentativas × 4 s)
on           # → timeouts e retransmissões; duplicados no Rx
```
ou, antes de arrancar Rx/Tx: `ber 0.00002` (REJ + retransmissões).

*Determinístico, com o relay de teste* (sem socat; faz falhar exatamente as tramas escolhidas):

```
make -C tests all
./tests/build/relay /tmp/rcomA /tmp/rcomB rx2tx:RR:3:drop tx2rx:I:5:data &
./tests/build/driver /tmp/rcomB 9600 rx out.gif 3 1 &
./tests/build/driver /tmp/rcomA 9600 tx penguin.gif 3 1
shasum -a 256 penguin.gif out.gif ; kill %1
```
O relay escreve `[relay] rx2tx RR#3: drop` em stderr quando injeta cada falha. Ações: `drop bcc1 data trunc
escend badesc dup garbage delay:<ms>` (documentação no topo de `tests/relay.c`).

### H.7 Consultar estatísticas

São impressas automaticamente no fim, nos dois lados (`===== Link layer statistics =====`):
retransmissões, timeouts, REJ, duplicados, débito R e eficiência S.

### H.8 Testes automáticos (não precisam de socat nem de portas reais)

```
make -C tests test          # unitários (ASan+UBSan) + integração (pseudoterminais, SHA-256)
make -C tests unit
make -C tests integration
./tests/run_stress.sh 40 10 10 5    # 40 transferências com perda 10 %, corrupção 10 % (qualquer byte), duplicação 5 %
./tests/efficiency.sh       # medições de S com o cabo oficial; escreve tests/results/efficiency.csv
```

Requisitos: `gcc`, `make`, `bash`, `python3`, `sha256sum` ou `shasum`. `efficiency.sh` precisa também de `socat`.
O limite do campo `serialPort` é de 50 caracteres: os testes usam links curtos em `/tmp/rcom-p.*`.

`run_stress.sh` verifica o invariante essencial: **uma transferência dada como concluída tem sempre o mesmo
SHA-256 do original; caso contrário o protocolo diz `FAILED` e não deixa ficheiro final.**

---

## I. Testes que TÊM de ser feitos no laboratório (PCs Linux, cabo RS-232 real)

Tudo o que se segue foi validado só com portas virtuais (macOS e Linux/Ubuntu 24.04 em Docker). **Nunca
foi executado em hardware real.** Fazer pela ordem; marcar cada linha.

Preparação (os dois PCs; GTKTerm **fechado**):

```
git pull && make                      # ou copiar o projeto; compila bin/main sem warnings
ls -l /dev/ttyS0 /dev/ttyUSB0         # confirmar que o utilizador tem permissão (grupo dialout)
make -C tests test                    # os testes de software também no Linux do laboratório
```

Portas (First Lab Class Guide): `/dev/ttyS0` no PC da bancada (tux2), `/dev/ttyUSB0` no PC pequeno da secretária.
**Os dois lados têm de usar o mesmo baud rate.**

| # | Teste | Comandos (Rx primeiro, depois Tx) | Resultado esperado |
|---|---|---|---|
| 1 | Transferência normal | Rx: `./bin/main /dev/ttyS0 9600 rx penguin-received.gif`<br>Tx: `./bin/main /dev/ttyUSB0 9600 tx penguin.gif` | Mensagens `File ... sent/received (10968 bytes)`; 14 tramas I, 0 retransmissões; ~11.8 s |
| 2 | Integridade | `diff -s penguin.gif penguin-received.gif` e `sha256sum penguin.gif penguin-received.gif` | `identical`; hashes iguais |
| 3 | Tx arranca antes do Rx | arrancar Tx; ~6 s depois, arrancar Rx | Tx mostra `llOpenTx: no UA (attempt 1/4)…`, transferência termina bem, ficheiros iguais |
| 4 | Cabo desligado e religado | durante a transferência, retirar o cabo ~6 s e voltar a ligar (< 16 s no total) | conclui; Tx com `Timeouts ≥ 1`, `retransmissions ≥ 1`; Rx com `duplicates` ≥ 0; ficheiros iguais |
| 5 | Cabo desligado por muito tempo | retirar o cabo > 20 s a meio | Tx `llSend: no acknowledgement…` + `Transmission of penguin.gif FAILED`; Rx `nothing received… giving up` + `Reception … FAILED`; **não existe** `penguin-received.gif` novo nem `.part` |
| 6 | Outros baud rates | `./bin/main /dev/ttyS0 19200 rx …` e `./bin/main /dev/ttyUSB0 19200 tx …` (idem 115200) | ficheiros iguais; `S ≈ 0.76–0.78` |
| 7 | Ruído físico (opcional) | tocar/dobrar o cabo durante a transferência | REJ/timeouts nas estatísticas; ficheiro igual ou `FAILED` limpo — nunca ficheiro diferente dado como válido |
| 8 | Ctrl-C no Tx a meio | `Ctrl-C` no Tx | Rx termina sozinho em ~16 s com `FAILED`; sem ficheiro final nem `.part` |
| 9 | Ficheiro maior / binário | `head -c 200000 /dev/urandom > big.bin` e transferir (a 9600 baud ≈ 3.5 min; usar 115200) | `sha256sum` igual |
| 10 | Eficiência (relatório) | repetir ≥ 3 vezes por configuração: `gcc -Wall -DAPP_CHUNK_SIZE=100 -o bin/main_100 src/*.c` (também 250, 500) e correr Tx/Rx com `bin/main_100`; ler `Efficiency S = R/C` e `Elapsed time` nos dois lados | tabela S vs tamanho de trama (S sobe com a trama; máx. teórico 0.8 a 8N1) |

Sem cable.c no laboratório não é possível variar BER nem atraso de propagação; para isso usar a máquina pessoal
(`./tests/efficiency.sh`, `cable` com `ber`/`prop`).

Se o #1 falhar: (a) correr `ls -l /dev/ttyS0` (permissões); (b) verificar o baud rate; (c) confirmar que o GTKTerm
está fechado; (d) ver `llOpenTx: no UA` — se o Tx nunca recebe UA, o cabo/porta está errado, não o protocolo.

---

## J. Ordem recomendada para estudar o código

1. `src/link_layer.h` e `src/serial_port.h` (a API).
2. `link_layer.c` — constantes e `Stats`; depois **§FRAME WRITING**: `stuffByte`, `computeBcc2`, `buildInfoFrame`.
3. **`receiveFrame`** (a máquina de estados). Percorrer com o exemplo de §F.
4. `waitSupervision`, `llOpenTx`, `llOpenRx` (SET/UA).
5. `llSend` e `rxProcess`/`llReceive` (Stop-and-Wait). Ler com a tabela §E.2/E.3.
6. `llCloseTx`, `llCloseRx` (DISC/DISC/UA e as limitações).
7. Timeouts: `startTicker`, `nowMs` e o comentário "TIME".
8. `application_layer.c`: pacotes (`build/parse*`), depois `runTx` e `runRx`.
9. `tests/test_unit.c` (cada teste é uma regra do protocolo), a lista de cenários em
   `tests/run_integration.sh` e o invariante de `tests/run_stress.sh`.
10. Antes da aula: ler a §I (testes no laboratório).
